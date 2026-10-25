/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace WebExtensions {

struct Manifest;

// chrome.i18n message catalogs from _locales/<locale>/messages.json.
class MessageCatalog {
public:
    // Loads catalogs for each preferred locale and its parents (en_US -> en), then default_locale.
    // Fails like Chrome does: _locales without a default_locale, a default_locale without a
    // catalog, or any malformed catalog that is loaded. An extension with neither is fine.
    static ErrorOr<MessageCatalog> load(StringView extension_root, StringView default_locale, Vector<String> preferred_locales, StringView extension_id = {});

    MessageCatalog() = default;

    // Adds one locale's messages.json at the lowest priority so far.
    ErrorOr<void> add_catalog(StringView locale, StringView json);

    void set_extension_id(String id) { m_extension_id = move(id); }
    void set_ui_locale(StringView locale);

    // chrome.i18n.getMessage: names are case-insensitive, $1..$9 are filled from substitutions
    // and "$$" becomes "$". Also answers the predefined "@@" messages.
    Optional<String> get_message(StringView name, Vector<String> const& substitutions = {}) const;

    // Replaces __MSG_name__ tokens. Unknown names are left as they are.
    String substitute_manifest_messages(StringView value) const;

    // The ui locale with underscores, like "en_US" (for @@ui_locale).
    String const& ui_locale() const { return m_ui_locale; }
    // Locales whose catalogs were loaded, highest priority first.
    Vector<String> const& loaded_locales() const { return m_loaded_locales; }
    bool is_empty() const { return m_catalogs.is_empty(); }

private:
    Optional<String> lookup(StringView lowercase_name) const;

    // Lowercase message name -> message with named placeholders already replaced.
    Vector<HashMap<String, String>> m_catalogs;
    Vector<String> m_loaded_locales;
    String m_extension_id;
    String m_ui_locale;
};

// "en-US" -> "en_US". Returns an empty Optional for names that aren't safe locale names.
Optional<String> normalize_locale_name(StringView);

// Localizes name, short_name, description, action title and command descriptions.
void localize(Manifest&, MessageCatalog const&);

}
