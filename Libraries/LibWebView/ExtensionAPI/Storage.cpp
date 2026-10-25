/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibWebExtensions/StorageArea.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionAPI/Management.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>

namespace WebView {

// chrome.storage: local and sync are saved per extension, session lasts as long as the browser runs. As in Chrome, every
// area needs the "storage" permission, and storage.session is for the extension's own pages until one of them opens it
// to content scripts (setAccessLevel), for this run.
class StorageAPI final : public ExtensionAPI {
public:
    explicit StorageAPI(ExtensionRuntime&);

private:
    void define(StringView area, StringView function, ExtensionRuntime::Handler);

    WebExtensions::StorageArea& storage_area(StringView extension_id, StringView area);
    void save_storage_area(StringView extension_id, StringView area);

    virtual void extension_uninstalled(StringView extension_id) override
    {
        auto prefix = MUST(String::formatted("{}/", extension_id));
        m_storage_areas.remove_all_matching([&](auto const& key, auto const&) { return key.starts_with_bytes(prefix); });
    }

    // "<extension id>/<area>" -> area.
    HashMap<String, WebExtensions::StorageArea> m_storage_areas;
};

NonnullOwnPtr<ExtensionAPI> create_storage_api(ExtensionRuntime& runtime)
{
    return make<StorageAPI>(runtime);
}

WebExtensions::StorageArea& StorageAPI::storage_area(StringView extension_id, StringView area)
{
    auto key = MUST(String::formatted("{}/{}", extension_id, area));
    return m_storage_areas.ensure(key, [&] {
        auto const* extension = runtime().registry().find(extension_id);
        bool unlimited = extension && extension->manifest().has_permission("unlimitedStorage"sv);
        auto quota = area == "sync"sv ? WebExtensions::StorageQuota::sync()
            : area == "session"sv     ? WebExtensions::StorageQuota::session()
                                      : WebExtensions::StorageQuota::local(unlimited);

        if (runtime().storage_directory().has_value() && area != "session"sv) {
            auto path = LexicalPath::join(*runtime().storage_directory(), extension_id, ByteString::formatted("{}.json", area)).string();
            auto loaded = WebExtensions::StorageArea::load_from_file(path, quota);
            if (!loaded.is_error())
                return loaded.release_value();
            dbgln("Extensions: Can't read {}'s {} storage: {}", extension_id, area, loaded.error());
        }
        return WebExtensions::StorageArea { quota };
    });
}

void StorageAPI::save_storage_area(StringView extension_id, StringView area)
{
    if (!runtime().storage_directory().has_value() || area == "session"sv)
        return;
    auto directory = LexicalPath::join(*runtime().storage_directory(), extension_id).string();
    if (auto result = Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes); result.is_error()) {
        dbgln("Extensions: Can't make {}: {}", directory, result.error());
        return;
    }
    auto path = LexicalPath::join(directory, ByteString::formatted("{}.json", area)).string();
    if (auto result = storage_area(extension_id, area).save_to_file(path); result.is_error())
        dbgln("Extensions: Can't save {}'s {} storage: {}", extension_id, area, result.error());
}

static ErrorOr<JsonValue, String> error_string(Error const& error)
{
    return MUST(String::formatted("{}", error));
}

void StorageAPI::define(StringView area, StringView function, ExtensionRuntime::Handler handler)
{
    auto name = MUST(String::formatted("storage.{}.{}", area, function));
    runtime().register_function(name, [this, area, handler = move(handler)](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
        if (!runtime().management().has_permission(context.extension_id, "storage"sv))
            return reply("Access to extension API denied."_string);
        if (area == "session"sv && context.type == "content_script"sv && !runtime().content_scripts_may_use_session_storage(context.extension_id))
            return reply("Access to storage is not allowed from this context."_string);
        handler(context, arguments, move(reply));
    });
}

StorageAPI::StorageAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    for (auto area : { "local"sv, "sync"sv, "session"sv }) {
        define(area, "get"sv, [this, area](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            auto keys = arguments.is_empty() ? JsonValue {} : arguments.at(0);
            auto items = storage_area(context.extension_id, area).get(keys);
            if (items.is_error())
                return reply(error_string(items.error()));
            reply(JsonValue { items.release_value() });
        });

        auto report_changes = [this, area](StringView extension_id, JsonObject changes) {
            if (changes.is_empty())
                return;
            save_storage_area(extension_id, area);
            runtime().dispatch_event(extension_id, MUST(String::formatted("storage.{}.onChanged", area)), JsonArray { Vector<JsonValue> { changes } });
            runtime().dispatch_event(extension_id, "storage.onChanged"sv, JsonArray { Vector<JsonValue> { move(changes), JsonValue { MUST(String::from_utf8(area)) } } });
        };

        define(area, "set"sv, [this, area, report_changes](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            if (arguments.is_empty() || !arguments.at(0).is_object())
                return reply("storage.set needs an object of items"_string);
            auto changes = storage_area(context.extension_id, area).set(arguments.at(0).as_object());
            if (changes.is_error())
                return reply(error_string(changes.error()));
            report_changes(context.extension_id, changes.release_value());
            reply(JsonValue {});
        });

        define(area, "remove"sv, [this, area, report_changes](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            auto keys = arguments.is_empty() ? JsonValue {} : arguments.at(0);
            auto changes = storage_area(context.extension_id, area).remove(keys);
            if (changes.is_error())
                return reply(error_string(changes.error()));
            report_changes(context.extension_id, changes.release_value());
            reply(JsonValue {});
        });

        define(area, "clear"sv, [this, area, report_changes](ExtensionRuntime::Context const& context, JsonArray const&, ExtensionRuntime::Reply reply) {
            report_changes(context.extension_id, storage_area(context.extension_id, area).clear());
            reply(JsonValue {});
        });

        define(area, "getBytesInUse"sv, [this, area](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            auto keys = arguments.is_empty() ? JsonValue {} : arguments.at(0);
            auto bytes = storage_area(context.extension_id, area).get_bytes_in_use(keys);
            if (bytes.is_error())
                return reply(error_string(bytes.error()));
            reply(JsonValue { static_cast<u64>(bytes.value()) });
        });

        define(area, "getKeys"sv, [this, area](ExtensionRuntime::Context const& context, JsonArray const&, ExtensionRuntime::Reply reply) {
            JsonArray keys;
            for (auto const& key : storage_area(context.extension_id, area).get_keys())
                keys.must_append(key);
            reply(JsonValue { move(keys) });
        });

        // setAccessLevel({ accessLevel }): only for session, and only from the extension's own pages.
        define(area, "setAccessLevel"sv, [this, area](ExtensionRuntime::Context const& context, JsonArray const& arguments, ExtensionRuntime::Reply reply) {
            auto level = !arguments.is_empty() && arguments.at(0).is_object() ? arguments.at(0).as_object().get_string("accessLevel"sv) : Optional<String> {};
            if (!level.has_value() || !level->is_one_of("TRUSTED_CONTEXTS"sv, "TRUSTED_AND_UNTRUSTED_CONTEXTS"sv))
                return reply(MUST(String::formatted("Error in invocation of storage.{}.setAccessLevel(object accessOptions, optional function callback): Error at parameter 'accessOptions': Error at property 'accessLevel': Value must be one of TRUSTED_AND_UNTRUSTED_CONTEXTS, TRUSTED_CONTEXTS.", area)));
            if (context.type == "content_script"sv)
                return reply("Context cannot set the storage access level"_string);
            if (area != "session"sv)
                return reply("This StorageArea is not available for setting access level"_string);
            runtime().set_content_scripts_may_use_session_storage(context.extension_id, level == "TRUSTED_AND_UNTRUSTED_CONTEXTS"sv);
            reply(JsonValue {});
        });
    }
}

}
