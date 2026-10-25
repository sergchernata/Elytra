/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebExtensions/Messages.h>

namespace WebExtensions {

static constexpr size_t max_catalog_file_size = 16 * MiB;
// Guards against placeholders that expand into themselves.
static constexpr size_t max_replacements = 1000;
static constexpr size_t max_expanded_length = 1 * MiB;

// Chrome's MessageBundle::IsValidName: ASCII letters, digits, '_' and '@'.
static bool is_valid_name(StringView name)
{
    if (name.is_empty())
        return false;
    for (auto c : name) {
        if (!is_ascii_alphanumeric(c) && c != '_' && c != '@')
            return false;
    }
    return true;
}

static String to_lowercase(StringView name)
{
    return name.to_ascii_lowercase_string();
}

enum class OnUndefined : u8 {
    Fail,
    Keep,
};

// Port of Chrome's MessageBundle::ReplaceVariables. Finds begin...end delimited names and
// replaces those found through `lookup`, continuing the scan just inside the replacement.
template<typename Lookup>
static ErrorOr<ByteString> replace_variables(StringView input, StringView begin_delimiter, StringView end_delimiter, OnUndefined on_undefined, Lookup&& lookup)
{
    ByteString message = input;
    size_t begin_index = 0;
    size_t replacements = 0;
    while (true) {
        auto begin = message.find(begin_delimiter, begin_index);
        if (!begin.has_value())
            return message;
        begin_index = *begin + begin_delimiter.length();
        if (begin_index >= message.length())
            return message;
        auto end = message.find(end_delimiter, begin_index);
        if (!end.has_value())
            return message;
        auto name = message.substring_view(begin_index, *end - begin_index);
        if (!is_valid_name(name))
            continue;
        Optional<String> value = lookup(to_lowercase(name));
        if (!value.has_value()) {
            if (on_undefined == OnUndefined::Fail)
                return Error::from_string_literal("Variable used but not defined.");
            continue;
        }
        if (++replacements > max_replacements)
            return Error::from_string_literal("Too many substitutions in message.");
        auto start = begin_index - begin_delimiter.length();
        auto after = *end + end_delimiter.length();
        message = ByteString::formatted("{}{}{}", message.substring_view(0, start), *value, message.substring_view(after));
        if (message.length() > max_expanded_length)
            return Error::from_string_literal("Message is too long after substitution.");
    }
}

// Port of base::ReplaceStringPlaceholders, which chrome.i18n.getMessage applies.
static String replace_positional_placeholders(StringView format, Vector<String> const& substitutions)
{
    StringBuilder builder;
    for (size_t i = 0; i < format.length(); ++i) {
        if (format[i] != '$') {
            builder.append(format[i]);
            continue;
        }
        if (i + 1 >= format.length())
            continue;
        ++i;
        if (format[i] == '$') {
            while (i < format.length() && format[i] == '$') {
                builder.append('$');
                ++i;
            }
            --i;
            continue;
        }
        if (format[i] < '1' || format[i] > '9')
            continue;
        size_t index = format[i] - '1';
        if (index < substitutions.size())
            builder.append(substitutions[index]);
    }
    return builder.to_string_without_validation();
}

static bool is_rtl_locale(StringView locale)
{
    auto language = locale;
    if (auto separator = locale.find_any_of("_-"sv); separator.has_value())
        language = locale.substring_view(0, *separator);
    static constexpr Array rtl_languages { "ar"sv, "ckb"sv, "dv"sv, "fa"sv, "he"sv, "iw"sv, "ks"sv, "ps"sv, "sd"sv, "syr"sv, "ug"sv, "ur"sv, "yi"sv };
    for (auto rtl : rtl_languages) {
        if (language.equals_ignoring_ascii_case(rtl))
            return true;
    }
    return false;
}

Optional<String> normalize_locale_name(StringView locale)
{
    if (locale.is_empty() || locale.length() > 64)
        return {};
    StringBuilder builder;
    for (auto c : locale) {
        if (c == '-')
            builder.append('_');
        else if (is_ascii_alphanumeric(c) || c == '_')
            builder.append(c);
        else
            return {};
    }
    return builder.to_string_without_validation();
}

ErrorOr<void> MessageCatalog::add_catalog(StringView locale, StringView json)
{
    if (!Utf8View { json }.validate())
        return Error::from_string_literal("Message catalog is not valid UTF-8.");
    auto stripped = TRY(strip_json_comments(json));
    auto parsed = TRY(JsonParser::parse(stripped));
    if (!parsed.is_object())
        return Error::from_string_literal("Message catalog is not a JSON dictionary.");

    HashMap<String, String> messages;
    TRY(parsed.as_object().try_for_each_member([&](String const& key, JsonValue const& value) -> ErrorOr<void> {
        if (!is_valid_name(key))
            return Error::from_string_literal("Name of a message key is invalid. Only ASCII [a-z], [A-Z], [0-9] and \"_\" are allowed.");
        if (key.starts_with_bytes("@@"sv))
            return Error::from_string_literal("Message names starting with \"@@\" are reserved.");
        if (!value.is_object())
            return Error::from_string_literal("Message entry is not a dictionary.");
        auto const& entry = value.as_object();
        auto message = entry.get_string("message"sv);
        if (!message.has_value())
            return Error::from_string_literal("There is no \"message\" element for a message key.");

        HashMap<String, String> placeholders;
        if (auto placeholder_value = entry.get("placeholders"sv); placeholder_value.has_value()) {
            if (!placeholder_value->is_object())
                return Error::from_string_literal("Invalid \"placeholders\" element in message catalog.");
            TRY(placeholder_value->as_object().try_for_each_member([&](String const& name, JsonValue const& placeholder) -> ErrorOr<void> {
                if (!is_valid_name(name))
                    return Error::from_string_literal("Name of a placeholder is invalid.");
                if (!placeholder.is_object())
                    return Error::from_string_literal("Invalid placeholder in message catalog.");
                auto content = placeholder.as_object().get_string("content"sv);
                if (!content.has_value())
                    return Error::from_string_literal("Invalid \"content\" element for a placeholder.");
                placeholders.set(to_lowercase(name), *content);
                return {};
            }));
        }

        auto replaced = TRY(replace_variables(*message, "$"sv, "$"sv, OnUndefined::Fail, [&](String const& name) -> Optional<String> {
            return placeholders.get(name).copy();
        }));
        messages.set(to_lowercase(key), MUST(String::from_byte_string(replaced)));
        return {};
    }));

    m_catalogs.append(move(messages));
    if (auto normalized = normalize_locale_name(locale); normalized.has_value())
        m_loaded_locales.append(normalized.release_value());
    return {};
}

void MessageCatalog::set_ui_locale(StringView locale)
{
    m_ui_locale = normalize_locale_name(locale).value_or({});
}

Optional<String> MessageCatalog::lookup(StringView lowercase_name) const
{
    if (lowercase_name.starts_with("@@"sv)) {
        bool rtl = is_rtl_locale(m_ui_locale);
        if (lowercase_name == "@@extension_id"sv) {
            if (m_extension_id.is_empty())
                return {};
            return m_extension_id;
        }
        if (lowercase_name == "@@ui_locale"sv)
            return m_ui_locale;
        if (lowercase_name == "@@bidi_dir"sv)
            return rtl ? "rtl"_string : "ltr"_string;
        if (lowercase_name == "@@bidi_reversed_dir"sv)
            return rtl ? "ltr"_string : "rtl"_string;
        if (lowercase_name == "@@bidi_start_edge"sv)
            return rtl ? "right"_string : "left"_string;
        if (lowercase_name == "@@bidi_end_edge"sv)
            return rtl ? "left"_string : "right"_string;
        return {};
    }
    for (auto const& catalog : m_catalogs) {
        if (auto message = catalog.get(lowercase_name); message.has_value())
            return *message;
    }
    return {};
}

Optional<String> MessageCatalog::get_message(StringView name, Vector<String> const& substitutions) const
{
    if (substitutions.size() > 9)
        return {};
    auto message = lookup(to_lowercase(name));
    if (!message.has_value())
        return {};
    return replace_positional_placeholders(*message, substitutions);
}

String MessageCatalog::substitute_manifest_messages(StringView value) const
{
    if (!value.contains("__MSG_"sv))
        return MUST(String::from_utf8(value));
    auto result = replace_variables(value, "__MSG_"sv, "__"sv, OnUndefined::Keep, [&](String const& name) {
        return lookup(name);
    });
    if (result.is_error())
        return MUST(String::from_utf8(value));
    return MUST(String::from_byte_string(result.value()));
}

ErrorOr<MessageCatalog> MessageCatalog::load(StringView extension_root, StringView default_locale, Vector<String> preferred_locales, StringView extension_id)
{
    MessageCatalog catalog;
    catalog.m_extension_id = TRY(String::from_utf8(extension_id));

    Optional<String> normalized_default;
    if (!default_locale.is_empty()) {
        normalized_default = normalize_locale_name(default_locale);
        if (!normalized_default.has_value())
            return Error::from_string_literal("Invalid value for default locale.");
    }

    Vector<String> chain;
    for (auto const& preferred : preferred_locales) {
        auto locale = normalize_locale_name(preferred);
        if (!locale.has_value())
            continue;
        if (catalog.m_ui_locale.is_empty())
            catalog.m_ui_locale = *locale;
        StringView current = locale->bytes_as_string_view();
        while (!current.is_empty()) {
            auto entry = MUST(String::from_utf8(current));
            if (!chain.contains_slow(entry))
                chain.append(move(entry));
            auto separator = current.find_last('_');
            if (!separator.has_value())
                break;
            current = current.substring_view(0, *separator);
        }
    }
    if (catalog.m_ui_locale.is_empty() && normalized_default.has_value())
        catalog.m_ui_locale = *normalized_default;

    auto locales_directory = LexicalPath::join(extension_root, "_locales"sv).string();
    bool has_locales = FileSystem::is_directory(locales_directory);

    if (!normalized_default.has_value()) {
        if (has_locales)
            return Error::from_string_literal("Localization used, but default_locale wasn't specified in the manifest.");
        return catalog;
    }
    if (!has_locales)
        return Error::from_string_literal("Default locale was specified, but _locales subtree is missing.");

    chain.remove_all_matching([&](auto const& locale) { return locale == *normalized_default; });
    chain.append(*normalized_default);

    for (auto const& locale : chain) {
        auto path = LexicalPath::join(locales_directory, locale, "messages.json"sv).string();
        bool is_default = locale == *normalized_default;
        if (!FileSystem::exists(path)) {
            if (is_default)
                return Error::from_string_literal("Default locale is defined but default data couldn't be loaded.");
            continue;
        }
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        auto size = TRY(file->size());
        if (size > max_catalog_file_size)
            return Error::from_string_literal("Message catalog is too large.");
        auto contents = TRY(file->read_until_eof());
        TRY(catalog.add_catalog(locale, StringView { contents.bytes() }));
    }
    return catalog;
}

void localize(Manifest& manifest, MessageCatalog const& catalog)
{
    auto localize_string = [&](String& value) {
        value = catalog.substitute_manifest_messages(value);
    };
    auto localize_optional = [&](Optional<String>& value) {
        if (value.has_value())
            localize_string(*value);
    };

    localize_string(manifest.name);
    manifest.name = MUST(String::from_utf8(manifest.name.bytes_as_string_view().trim_whitespace()));
    localize_optional(manifest.short_name);
    localize_optional(manifest.description);
    if (manifest.action.has_value())
        localize_optional(manifest.action->default_title);
    for (auto& command : manifest.commands)
        localize_optional(command.description);
}

}
