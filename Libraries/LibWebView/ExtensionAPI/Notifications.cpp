/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Random.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Fetch/Infrastructure/DataURL.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ExtensionAPI/Notifications.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

NonnullOwnPtr<ExtensionAPI> create_notifications_api(ExtensionRuntime& runtime)
{
    return make<NotificationsAPI>(runtime);
}

using Context = ExtensionRuntime::Context;
using Reply = ExtensionRuntime::Reply;

static constexpr size_t maximum_image_size = 4 * MiB;
static constexpr size_t maximum_text_length = 64 * KiB;
static constexpr size_t maximum_notifications_per_extension = 256;

ErrorOr<ExtensionResource, String> read_extension_resource(ExtensionRuntime const& runtime, Context const& context, URL::URL const& url, size_t maximum_size)
{
    auto const* extension = runtime.registry().find(context.extension_id);
    if (!extension)
        return "The extension isn't loaded"_string;

    if (url.scheme() == "data"sv) {
        auto data_url = Web::Fetch::Infrastructure::process_data_url(url);
        if (data_url.is_error())
            return "Invalid data: URL"_string;
        if (data_url.value().body.size() > maximum_size)
            return "The data is too large"_string;
        return ExtensionResource { move(data_url.value().body), data_url.value().mime_type.essence() };
    }

    if (url.scheme() == "blob"sv) {
        // A blob of the extension's own, made in one of its pages.
        if (!context.page || !context.page->is_open())
            return "The blob: URL can't be read"_string;
        auto* store = context.page->client().session().blob_url_store.ptr();
        if (!store)
            return "The blob: URL can't be read"_string;
        auto entry = store->resolve(Utf16String::from_utf8(url.serialize()), {});
        if (!entry.has_value() || !entry->origin.is_same_origin(extension->origin()))
            return "The blob: URL isn't one of the extension's"_string;
        auto const* blob = entry->object.get_pointer<Web::FileAPI::SerializedBlobURLEntry::Blob>();
        if (!blob)
            return "The blob: URL isn't a blob"_string;
        if (blob->data.size() > maximum_size)
            return "The blob is too large"_string;
        auto bytes = ByteBuffer::copy(blob->data.bytes());
        if (bytes.is_error())
            return "Out of memory"_string;
        return ExtensionResource { bytes.release_value(), blob->type };
    }

    if (url.scheme() == "chrome-extension"sv) {
        if (url.serialized_host() != extension->id())
            return "The file isn't one of the extension's"_string;
        StringBuilder path;
        for (auto segment : url.path_segments()) {
            path.append('/');
            path.append(URL::percent_decode(segment));
        }
        auto bytes = extension->read_file_bytes(path.string_view(), maximum_size);
        if (!bytes.has_value())
            return "The extension has no such file"_string;
        return ExtensionResource { bytes.release_value(), {} };
    }

    return MUST(String::formatted("{}: URLs aren't allowed", url.scheme()));
}

// Arguments are checked before anything is done: a renderer can send anything.
static ErrorOr<void, String> apply_options(ExtensionRuntime const& runtime, Context const& context, ExtensionNotification& notification, JsonObject const& options)
{
    auto string_option = [&](StringView key, String& target) -> ErrorOr<void, String> {
        auto value = options.get(key);
        if (!value.has_value() || value->is_null())
            return {};
        if (!value->is_string())
            return MUST(String::formatted("{} must be a string", key));
        if (value->as_string().bytes().size() > maximum_text_length)
            return MUST(String::formatted("{} is too long", key));
        target = value->as_string();
        return {};
    };
    auto bool_option = [&](StringView key, bool& target) -> ErrorOr<void, String> {
        auto value = options.get(key);
        if (!value.has_value() || value->is_null())
            return {};
        if (!value->is_bool())
            return MUST(String::formatted("{} must be a boolean", key));
        target = value->as_bool();
        return {};
    };
    auto image_option = [&](StringView key) -> ErrorOr<Optional<ByteBuffer>, String> {
        auto value = options.get(key);
        if (!value.has_value() || value->is_null())
            return Optional<ByteBuffer> {};
        if (!value->is_string())
            return MUST(String::formatted("{} must be a string", key));
        auto url = URL::Parser::basic_parse(value->as_string(), context.url);
        if (!url.has_value())
            return "Unable to download all specified images."_string;
        auto resource = read_extension_resource(runtime, context, *url, maximum_image_size);
        if (resource.is_error())
            return "Unable to download all specified images."_string;
        return Optional<ByteBuffer> { move(resource.value().bytes) };
    };

    if (auto type = options.get("type"sv); type.has_value() && !type->is_null()) {
        if (!type->is_string() || !first_is_one_of(type->as_string(), "basic"sv, "image"sv, "list"sv, "progress"sv))
            return "Invalid notification type"_string;
        notification.type = type->as_string();
    }
    TRY(string_option("title"sv, notification.title));
    TRY(string_option("message"sv, notification.message));
    TRY(string_option("contextMessage"sv, notification.context_message));
    TRY(bool_option("requireInteraction"sv, notification.require_interaction));
    TRY(bool_option("silent"sv, notification.silent));
    if (auto value = options.get("isClickable"sv); value.has_value() && !value->is_null() && !value->is_bool())
        return "isClickable must be a boolean"_string;

    if (auto value = options.get("priority"sv); value.has_value() && !value->is_null()) {
        auto priority = value->get_integer<i64>();
        if (!priority.has_value())
            return "priority must be an integer"_string;
        notification.priority = static_cast<i32>(clamp(*priority, -2, 2));
    }
    if (auto value = options.get("eventTime"sv); value.has_value() && !value->is_null()) {
        auto time = value->get_double_with_precision_loss();
        if (!value->is_number() || !time.has_value())
            return "eventTime must be a number"_string;
        notification.event_time = *time;
    }

    if (auto value = options.get("buttons"sv); value.has_value() && !value->is_null()) {
        if (!value->is_array())
            return "buttons must be an array"_string;
        Vector<String> buttons;
        // Chrome shows two at most, and ignores the rest.
        for (size_t i = 0; i < value->as_array().size() && buttons.size() < 2; ++i) {
            auto const& button = value->as_array().at(i);
            if (!button.is_object())
                return "Each button must be an object"_string;
            auto title = button.as_object().get_string("title"sv);
            if (!title.has_value() || title->bytes().size() > maximum_text_length)
                return "Each button needs a title"_string;
            buttons.append(*title);
        }
        notification.buttons = move(buttons);
    }

    if (auto value = options.get("items"sv); value.has_value() && !value->is_null()) {
        if (!value->is_array() || value->as_array().size() > 1000)
            return "items must be an array"_string;
        Vector<ExtensionNotification::Item> items;
        for (auto const& item : value->as_array().values()) {
            if (!item.is_object())
                return "Each item must be an object"_string;
            auto title = item.as_object().get_string("title"sv);
            auto message = item.as_object().get_string("message"sv);
            if (!title.has_value() || !message.has_value() || title->bytes().size() > maximum_text_length || message->bytes().size() > maximum_text_length)
                return "Each item needs a title and a message"_string;
            items.append({ *title, *message });
        }
        notification.items = move(items);
    }

    if (auto value = options.get("progress"sv); value.has_value() && !value->is_null()) {
        auto progress = value->get_integer<i64>();
        if (!progress.has_value() || *progress < 0 || *progress > 100)
            return "The progress value should range from 0 to 100"_string;
        notification.progress = static_cast<i32>(*progress);
    }

    if (auto icon = TRY(image_option("iconUrl"sv)); icon.has_value())
        notification.icon = icon.release_value();
    if (auto image = TRY(image_option("imageUrl"sv)); image.has_value())
        notification.image = image.release_value();

    // What only some types have.
    if (!notification.items.is_empty() && notification.type != "list"sv)
        return "List items provided for notification type != list"_string;
    if (notification.progress.has_value() && notification.type != "progress"sv)
        return "Progress value provided for notification type != progress"_string;
    if (notification.type == "image"sv && !notification.image.has_value())
        return "An image notification needs an imageUrl"_string;
    return {};
}

NotificationsAPI::NotificationsAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    // For the extension's pages and background, with the "notifications" permission.
    auto add = [this](StringView name, Function<void(Context const&, JsonArray const&, Reply)> handler) {
        runtime().register_function(name, [this, handler = move(handler)](Context const& context, JsonArray const& arguments, Reply reply) {
            if (context.type == "content_script"sv)
                return reply("Content scripts can't use chrome.notifications"_string);
            auto const* extension = runtime().registry().find(context.extension_id);
            if (!extension || !extension->manifest().has_permission("notifications"sv))
                return reply("chrome.notifications needs the \"notifications\" permission"_string);
            handler(context, arguments, move(reply));
        });
    };

    add("notifications.create"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        // create(options) or create(notificationId, options).
        Optional<String> id;
        JsonValue options;
        if (arguments.size() == 1) {
            options = arguments.at(0);
        } else if (arguments.size() == 2) {
            auto const& given = arguments.at(0);
            if (given.is_string())
                id = given.as_string();
            else if (!given.is_null())
                return reply("The notification ID must be a string"_string);
            options = arguments.at(1);
        } else {
            return reply("Expected options"_string);
        }
        if (!options.is_object())
            return reply("The notification options must be an object"_string);
        if (id.has_value() && id->bytes().size() > 1024)
            return reply("The notification ID is too long"_string);
        // Without one (or with an empty one), the notification gets an ID of its own.
        if (!id.has_value() || id->is_empty())
            id = generate_random_uuid();

        auto const& object = options.as_object();
        for (auto key : { "type"sv, "iconUrl"sv, "title"sv, "message"sv }) {
            if (!object.get(key).has_value() || object.get(key)->is_null())
                return reply("Some of the required properties are missing: type, iconUrl, title and message."_string);
        }

        auto& notifications = m_notifications.ensure(context.extension_id);
        if (!notifications.contains(*id) && notifications.size() >= maximum_notifications_per_extension)
            return reply("Too many notifications"_string);

        ExtensionNotification notification;
        notification.extension_id = context.extension_id;
        notification.id = *id;
        notification.type = "basic"_string;
        if (auto result = apply_options(runtime(), context, notification, object); result.is_error())
            return reply(result.release_error());

        // An ID in use: the new notification replaces the old.
        notifications.set(*id, notification);
        if (m_delegate)
            m_delegate->show_notification(notification);
        reply(JsonValue { *id });
    });

    add("notifications.update"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (arguments.size() != 2 || !arguments.at(0).is_string() || !arguments.at(1).is_object())
            return reply("Expected a notification ID and options"_string);
        auto notifications = m_notifications.find(context.extension_id);
        if (notifications == m_notifications.end())
            return reply(JsonValue { false });
        auto existing = notifications->value.find(arguments.at(0).as_string());
        if (existing == notifications->value.end())
            return reply(JsonValue { false });

        auto updated = existing->value;
        if (auto result = apply_options(runtime(), context, updated, arguments.at(1).as_object()); result.is_error())
            return reply(result.release_error());
        existing->value = updated;
        if (m_delegate)
            m_delegate->show_notification(updated);
        reply(JsonValue { true });
    });

    add("notifications.clear"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (arguments.size() != 1 || !arguments.at(0).is_string())
            return reply("Expected a notification ID"_string);
        auto notifications = m_notifications.find(context.extension_id);
        if (notifications == m_notifications.end() || !notifications->value.remove(arguments.at(0).as_string()))
            return reply(JsonValue { false });
        auto id = arguments.at(0).as_string();
        if (m_delegate)
            m_delegate->close_notification(context.extension_id, id);
        reply(JsonValue { true });
        dispatch(context.extension_id, "notifications.onClosed"sv, JsonArray { Vector<JsonValue> { JsonValue { id }, JsonValue { false } } });
    });

    add("notifications.getAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        JsonObject all;
        if (auto notifications = m_notifications.find(context.extension_id); notifications != m_notifications.end()) {
            for (auto const& [id, notification] : notifications->value)
                all.set(id, true);
        }
        reply(JsonValue { move(all) });
    });

    add("notifications.getPermissionLevel"sv, [](Context const&, JsonArray const&, Reply reply) {
        reply(JsonValue { "granted"sv });
    });
}

void NotificationsAPI::dispatch(StringView extension_id, StringView event, JsonArray arguments)
{
    if (runtime().background_is_starting(extension_id)) {
        runtime().wait_for_background(extension_id, [this, extension_id = MUST(String::from_utf8(extension_id)), event = MUST(String::from_utf8(event)), arguments = move(arguments)]() mutable {
            runtime().dispatch_event(extension_id, event, move(arguments));
        });
        return;
    }
    runtime().dispatch_event(extension_id, event, move(arguments));
}

ExtensionNotification const* NotificationsAPI::find_showing(StringView extension_id, StringView id) const
{
    auto notifications = m_notifications.find(extension_id);
    if (notifications == m_notifications.end())
        return nullptr;
    auto notification = notifications->value.find(id);
    return notification == notifications->value.end() ? nullptr : &notification->value;
}

void NotificationsAPI::clicked(StringView extension_id, StringView id)
{
    if (!find_showing(extension_id, id))
        return;
    dispatch(extension_id, "notifications.onClicked"sv, JsonArray { Vector<JsonValue> { JsonValue { MUST(String::from_utf8(id)) } } });
}

void NotificationsAPI::button_clicked(StringView extension_id, StringView id, size_t button_index)
{
    auto const* notification = find_showing(extension_id, id);
    if (!notification || button_index >= notification->buttons.size())
        return;
    dispatch(extension_id, "notifications.onButtonClicked"sv, JsonArray { Vector<JsonValue> { JsonValue { MUST(String::from_utf8(id)) }, JsonValue { button_index } } });
}

void NotificationsAPI::closed(StringView extension_id, StringView id, bool by_user)
{
    auto notifications = m_notifications.find(extension_id);
    if (notifications == m_notifications.end() || !notifications->value.remove(id))
        return;
    dispatch(extension_id, "notifications.onClosed"sv, JsonArray { Vector<JsonValue> { JsonValue { MUST(String::from_utf8(id)) }, JsonValue { by_user } } });
}

Optional<ExtensionNotification const&> NotificationsAPI::find(StringView extension_id, StringView id) const
{
    auto const* notification = find_showing(extension_id, id);
    if (!notification)
        return {};
    return *notification;
}

Vector<ExtensionNotification const*> NotificationsAPI::notifications_of(StringView extension_id) const
{
    Vector<ExtensionNotification const*> result;
    if (auto notifications = m_notifications.find(extension_id); notifications != m_notifications.end()) {
        for (auto const& [id, notification] : notifications->value)
            result.append(&notification);
    }
    return result;
}

void NotificationsAPI::extensions_changed()
{
    // An extension that's gone (or lost the permission) takes its notifications with it.
    Vector<String> gone;
    for (auto const& [extension_id, notifications] : m_notifications) {
        auto const* extension = runtime().registry().find(extension_id);
        if (!extension || !extension->manifest().has_permission("notifications"sv))
            gone.append(extension_id);
    }
    for (auto const& extension_id : gone) {
        auto notifications = m_notifications.take(extension_id);
        if (!m_delegate || !notifications.has_value())
            continue;
        for (auto const& [id, notification] : *notifications)
            m_delegate->close_notification(extension_id, id);
    }
}

}
