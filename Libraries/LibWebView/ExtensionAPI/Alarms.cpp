/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(AK_OS_MACOS)
#    include <CoreFoundation/CoreFoundation.h>
#endif

#include <AK/HashTable.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Utf8View.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionAPI/ExtensionAPI.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/Settings.h>

#if defined(AK_OS_MACOS)
// From CoreGraphics (linked through Carbon), declared here to keep its headers out.
extern "C" double CGEventSourceSecondsSinceLastEventType(i32 source_state, u32 event_type);
extern "C" CFDictionaryRef CGSessionCopyCurrentDictionary();
#endif

namespace WebView {

static AK::Duration s_minimum_alarm_delay = AK::Duration::from_seconds(30);
static AK::Duration s_idle_poll_interval = AK::Duration::from_seconds(5);

static Function<IdleState()>& idle_state_source_for_testing()
{
    static NeverDestroyed<Function<IdleState()>> source;
    return *source;
}

void set_minimum_alarm_delay_for_testing(AK::Duration delay)
{
    s_minimum_alarm_delay = delay;
}

void set_idle_state_source_for_testing(Function<IdleState()> source, AK::Duration poll_interval)
{
    idle_state_source_for_testing() = move(source);
    s_idle_poll_interval = poll_interval;
}

// How long since the user's last input, and whether the screen is locked.
static IdleState system_idle_state()
{
    if (auto& source = idle_state_source_for_testing())
        return source();

    IdleState state;
#if defined(AK_OS_MACOS)
    // kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType.
    auto seconds = CGEventSourceSecondsSinceLastEventType(0, ~0u);
    if (seconds > 0)
        state.idle_time = AK::Duration::from_milliseconds(static_cast<i64>(seconds * 1000));
    if (auto session = CGSessionCopyCurrentDictionary()) {
        auto locked = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
        state.screen_locked = locked && CFGetTypeID(locked) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(locked));
        CFRelease(session);
    }
#endif
    // FIXME: Elsewhere, ask the desktop (e.g. org.freedesktop.ScreenSaver); until then the user is always active.
    return state;
}

static constexpr i64 minimum_idle_threshold = 15;
static constexpr i64 maximum_idle_threshold = 4 * 60 * 60;
static constexpr i64 default_idle_threshold = 60;
static constexpr size_t maximum_alarms = 500;

static StringView idle_state_name(IdleState const& state, i64 threshold_in_seconds)
{
    if (state.screen_locked)
        return "locked"sv;
    if (state.idle_time >= AK::Duration::from_seconds(threshold_in_seconds))
        return "idle"sv;
    return "active"sv;
}

static double now_in_milliseconds()
{
    return static_cast<double>(UnixDateTime::now().nanoseconds_since_epoch()) / 1'000'000.0;
}

// chrome.alarms, chrome.idle and chrome.i18n's browser-side functions.
class AlarmsAPI final : public ExtensionAPI {
public:
    explicit AlarmsAPI(ExtensionRuntime&);

    virtual void context_destroyed(u64 context_id) override { update_idle_polling(context_id); }
    virtual void listeners_changed(u64, StringView event) override
    {
        if (event == "idle.onStateChanged"sv)
            update_idle_polling();
    }
    virtual void extensions_changed() override;
    virtual void shut_down() override;

private:
    struct Alarm {
        String name;
        double scheduled_time { 0 }; // Milliseconds since the epoch.
        Optional<double> period_in_minutes;

        JsonObject to_json() const;
    };

    using Context = ExtensionRuntime::Context;
    using Reply = ExtensionRuntime::Reply;

    Optional<String> refusal(Context const&, StringView permission) const;
    void register_alarms_functions();
    void register_idle_functions();
    void register_i18n_functions();

    Vector<Alarm>& alarms_of(StringView extension_id);
    ByteString alarms_path(StringView extension_id) const;
    void save_alarms(StringView extension_id);
    void fire_due_alarms();
    void schedule_next_alarm();

    void update_idle_polling(Optional<u64> leaving_context = {});
    void poll_idle_state();

    // Extension ID -> its alarms, in the order they were made; read from disk as an extension is first seen.
    HashMap<String, Vector<Alarm>> m_alarms;
    RefPtr<Core::Timer> m_alarm_timer;

    // Extension ID -> its idle.setDetectionInterval().
    HashMap<String, i64> m_idle_thresholds;
    // Extension ID -> the state it last heard, for those listening to idle.onStateChanged.
    HashMap<String, StringView> m_idle_states;
    RefPtr<Core::Timer> m_idle_timer;
};

NonnullOwnPtr<ExtensionAPI> create_alarms_api(ExtensionRuntime& runtime)
{
    return make<AlarmsAPI>(runtime);
}

AlarmsAPI::AlarmsAPI(ExtensionRuntime& extension_runtime)
    : ExtensionAPI(extension_runtime)
{
    register_alarms_functions();
    register_idle_functions();
    register_i18n_functions();
}

Optional<String> AlarmsAPI::refusal(Context const& context, StringView permission) const
{
    if (context.type == "content_script"sv)
        return MUST(String::formatted("chrome.{} isn't available to content scripts", permission));
    auto const* extension = runtime().registry().find(context.extension_id);
    if (!extension || !extension->manifest().has_permission(permission))
        return MUST(String::formatted("chrome.{} needs the \"{}\" permission", permission, permission));
    return {};
}

JsonObject AlarmsAPI::Alarm::to_json() const
{
    JsonObject alarm;
    alarm.set("name"sv, name);
    alarm.set("scheduledTime"sv, scheduled_time);
    if (period_in_minutes.has_value())
        alarm.set("periodInMinutes"sv, *period_in_minutes);
    return alarm;
}

ByteString AlarmsAPI::alarms_path(StringView extension_id) const
{
    return LexicalPath::join(*runtime().storage_directory(), extension_id, "alarms.json"sv).string();
}

Vector<AlarmsAPI::Alarm>& AlarmsAPI::alarms_of(StringView extension_id)
{
    return m_alarms.ensure(MUST(String::from_utf8(extension_id)), [&] {
        Vector<Alarm> alarms;
        if (!runtime().storage_directory().has_value())
            return alarms;
        auto file = Core::File::open(alarms_path(extension_id), Core::File::OpenMode::Read);
        if (file.is_error())
            return alarms;
        auto contents = file.value()->read_until_eof();
        if (contents.is_error())
            return alarms;
        auto json = JsonParser::parse(contents.value());
        if (json.is_error() || !json.value().is_array()) {
            dbgln("Extensions: Can't read {}'s alarms", extension_id);
            return alarms;
        }
        json.value().as_array().for_each([&](JsonValue const& value) {
            if (!value.is_object() || alarms.size() >= maximum_alarms)
                return;
            auto const& object = value.as_object();
            auto name = object.get_string("name"sv);
            auto scheduled_time = object.get_double_with_precision_loss("scheduledTime"sv);
            if (!name.has_value() || !scheduled_time.has_value())
                return;
            Optional<double> period;
            if (auto saved_period = object.get_double_with_precision_loss("periodInMinutes"sv); saved_period.has_value() && *saved_period > 0)
                period = *saved_period;
            alarms.remove_all_matching([&](Alarm const& alarm) { return alarm.name == *name; });
            alarms.append({ .name = *name, .scheduled_time = *scheduled_time, .period_in_minutes = period });
        });
        return alarms;
    });
}

void AlarmsAPI::save_alarms(StringView extension_id)
{
    if (!runtime().storage_directory().has_value())
        return;
    JsonArray saved;
    for (auto const& alarm : alarms_of(extension_id))
        saved.must_append(alarm.to_json());

    auto path = alarms_path(extension_id);
    if (auto result = Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes); result.is_error()) {
        dbgln("Extensions: Can't save {}'s alarms: {}", extension_id, result.error());
        return;
    }
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error()) {
        dbgln("Extensions: Can't save {}'s alarms: {}", extension_id, file.error());
        return;
    }
    auto serialized = saved.serialized();
    if (auto result = file.value()->write_until_depleted(serialized.bytes()); result.is_error())
        dbgln("Extensions: Can't save {}'s alarms: {}", extension_id, result.error());
}

// Fires what's due to the extension's background and pages; a background that's starting hears it once it can.
void AlarmsAPI::fire_due_alarms()
{
    auto now = now_in_milliseconds();
    for (auto& [extension_id, alarms] : m_alarms) {
        Vector<JsonObject> fired;
        alarms.remove_all_matching([&](Alarm& alarm) {
            if (alarm.scheduled_time > now)
                return false;
            fired.append(alarm.to_json());
            if (!alarm.period_in_minutes.has_value())
                return true;
            // A periodic alarm missed several times (as while the browser was closed) fires once.
            auto period = *alarm.period_in_minutes * 60'000;
            alarm.scheduled_time += period;
            if (alarm.scheduled_time <= now)
                alarm.scheduled_time = now + period;
            return false;
        });
        if (fired.is_empty())
            continue;
        save_alarms(extension_id);
        for (auto& alarm : fired) {
            auto dispatch = [this, extension_id, alarm = move(alarm)] {
                runtime().dispatch_event(extension_id, "alarms.onAlarm"sv, JsonArray { Vector<JsonValue> { alarm } });
            };
            if (runtime().background_is_starting(extension_id))
                runtime().wait_for_background(extension_id, move(dispatch));
            else
                dispatch();
        }
    }
    schedule_next_alarm();
}

void AlarmsAPI::schedule_next_alarm()
{
    Optional<double> next;
    for (auto const& [extension_id, alarms] : m_alarms) {
        for (auto const& alarm : alarms) {
            if (!next.has_value() || alarm.scheduled_time < *next)
                next = alarm.scheduled_time;
        }
    }
    if (!next.has_value()) {
        if (m_alarm_timer)
            m_alarm_timer->stop();
        return;
    }
    // Timers count in ints of milliseconds; a far-off alarm is looked at again in a day.
    auto delay = clamp(*next - now_in_milliseconds(), 0.0, 86'400'000.0);
    auto interval = AK::Duration::from_milliseconds(static_cast<i64>(ceil(delay)));
    if (!m_alarm_timer)
        m_alarm_timer = Core::Timer::create_single_shot(interval, [this] { fire_due_alarms(); });
    m_alarm_timer->restart(interval);
}

void AlarmsAPI::extensions_changed()
{
    // Removed extensions (and any that lost the permission) lose their alarms here; their saved alarms stay on disk.
    Vector<String> gone;
    for (auto const& [extension_id, alarms] : m_alarms) {
        auto const* extension = runtime().registry().find(extension_id);
        if (!extension || !extension->manifest().has_permission("alarms"sv))
            gone.append(extension_id);
    }
    for (auto const& extension_id : gone)
        m_alarms.remove(extension_id);

    // Alarms kept from before run again, and those missed meanwhile fire now.
    runtime().registry().for_each_extension([&](Extension const& extension) {
        if (extension.manifest().has_permission("alarms"sv))
            (void)alarms_of(extension.id());
    });
    fire_due_alarms();
    update_idle_polling();
}

void AlarmsAPI::shut_down()
{
    if (m_alarm_timer)
        m_alarm_timer->stop();
    if (m_idle_timer)
        m_idle_timer->stop();
}

static ErrorOr<Optional<double>, String> optional_number(JsonObject const& info, StringView key)
{
    auto value = info.get(key);
    if (!value.has_value())
        return Optional<double> {};
    if (!value->is_number())
        return MUST(String::formatted("alarms.create: {} must be a number", key));
    return value->get_double_with_precision_loss();
}

// An alarm's name: the first argument, "" if left out, nothing if it isn't a string.
static Optional<String> alarm_name(JsonArray const& arguments)
{
    if (arguments.is_empty() || arguments.at(0).is_null())
        return String {};
    if (!arguments.at(0).is_string())
        return {};
    return arguments.at(0).as_string();
}

static String bad_name_error(StringView function)
{
    return MUST(String::formatted("alarms.{}: the name must be a string", function));
}

void AlarmsAPI::register_alarms_functions()
{
    // alarms.create([name], alarmInfo)
    runtime().register_function("alarms.create"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (auto error = refusal(context, "alarms"sv); error.has_value())
            return reply(error.release_value());

        String name;
        JsonValue info;
        if (!arguments.is_empty() && arguments.at(0).is_object()) {
            info = arguments.at(0);
        } else {
            auto given_name = alarm_name(arguments);
            if (!given_name.has_value())
                return reply(bad_name_error("create"sv));
            name = given_name.release_value();
            if (arguments.size() > 1)
                info = arguments.at(1);
        }
        if (!info.is_object())
            return reply("alarms.create needs an alarmInfo object"_string);

        auto when = optional_number(info.as_object(), "when"sv);
        auto delay = optional_number(info.as_object(), "delayInMinutes"sv);
        auto period = optional_number(info.as_object(), "periodInMinutes"sv);
        for (auto* result : { &when, &delay, &period }) {
            if (result->is_error())
                return reply(result->release_error());
        }
        if (when.value().has_value() && delay.value().has_value())
            return reply("Cannot set both when and delayInMinutes."_string);
        if (!when.value().has_value() && !delay.value().has_value() && !period.value().has_value())
            return reply("Must set at least one of when, delayInMinutes, or periodInMinutes."_string);

        // Like a packed extension in Chrome, nothing waits less than the minimum (30 seconds).
        auto minimum = static_cast<double>(s_minimum_alarm_delay.to_milliseconds()) / 60'000.0;
        Optional<double> period_in_minutes;
        if (period.value().has_value())
            period_in_minutes = max(*period.value(), minimum);
        auto now = now_in_milliseconds();
        double scheduled_time = when.value().has_value()
            ? *when.value()
            : now + max(delay.value().value_or(period_in_minutes.value_or(0)), minimum) * 60'000;

        auto& alarms = alarms_of(context.extension_id);
        alarms.remove_all_matching([&](Alarm const& alarm) { return alarm.name == name; });
        if (alarms.size() >= maximum_alarms)
            return reply(MUST(String::formatted("An extension can't have more than {} alarms", maximum_alarms)));
        alarms.append({ .name = move(name), .scheduled_time = scheduled_time, .period_in_minutes = period_in_minutes });
        save_alarms(context.extension_id);
        schedule_next_alarm();
        reply(JsonValue {});
    });

    runtime().register_function("alarms.get"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (auto error = refusal(context, "alarms"sv); error.has_value())
            return reply(error.release_value());
        auto name = alarm_name(arguments);
        if (!name.has_value())
            return reply(bad_name_error("get"sv));
        for (auto const& alarm : alarms_of(context.extension_id)) {
            if (alarm.name == name.value())
                return reply(JsonValue { alarm.to_json() });
        }
        reply(JsonValue {});
    });

    runtime().register_function("alarms.getAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (auto error = refusal(context, "alarms"sv); error.has_value())
            return reply(error.release_value());
        JsonArray all;
        for (auto const& alarm : alarms_of(context.extension_id))
            all.must_append(alarm.to_json());
        reply(JsonValue { move(all) });
    });

    runtime().register_function("alarms.clear"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (auto error = refusal(context, "alarms"sv); error.has_value())
            return reply(error.release_value());
        auto name = alarm_name(arguments);
        if (!name.has_value())
            return reply(bad_name_error("clear"sv));
        bool cleared = alarms_of(context.extension_id).remove_all_matching([&](Alarm const& alarm) { return alarm.name == name.value(); });
        if (cleared) {
            save_alarms(context.extension_id);
            schedule_next_alarm();
        }
        reply(JsonValue { cleared });
    });

    runtime().register_function("alarms.clearAll"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (auto error = refusal(context, "alarms"sv); error.has_value())
            return reply(error.release_value());
        alarms_of(context.extension_id).clear();
        save_alarms(context.extension_id);
        schedule_next_alarm();
        reply(JsonValue { true });
    });
}

// A number of seconds, within what Chrome allows.
static ErrorOr<i64, String> idle_threshold(JsonArray const& arguments, StringView function)
{
    if (arguments.is_empty() || !arguments.at(0).is_number())
        return MUST(String::formatted("idle.{} needs a number of seconds", function));
    auto seconds = arguments.at(0).get_double_with_precision_loss().value();
    return static_cast<i64>(clamp(seconds, static_cast<double>(minimum_idle_threshold), static_cast<double>(maximum_idle_threshold)));
}

void AlarmsAPI::register_idle_functions()
{
    runtime().register_function("idle.queryState"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (auto error = refusal(context, "idle"sv); error.has_value())
            return reply(error.release_value());
        auto threshold = idle_threshold(arguments, "queryState"sv);
        if (threshold.is_error())
            return reply(threshold.release_error());
        reply(JsonValue { idle_state_name(system_idle_state(), threshold.value()) });
    });

    runtime().register_function("idle.setDetectionInterval"sv, [this](Context const& context, JsonArray const& arguments, Reply reply) {
        if (auto error = refusal(context, "idle"sv); error.has_value())
            return reply(error.release_value());
        auto threshold = idle_threshold(arguments, "setDetectionInterval"sv);
        if (threshold.is_error())
            return reply(threshold.release_error());
        m_idle_thresholds.set(context.extension_id, threshold.value());
        reply(JsonValue {});
    });

    // Only ChromeOS locks the screen by itself after a delay; elsewhere Chrome answers 0.
    runtime().register_function("idle.getAutoLockDelay"sv, [this](Context const& context, JsonArray const&, Reply reply) {
        if (auto error = refusal(context, "idle"sv); error.has_value())
            return reply(error.release_value());
        reply(JsonValue { 0 });
    });
}

// The system is asked how idle the user is only while some extension listens to idle.onStateChanged.
void AlarmsAPI::update_idle_polling(Optional<u64> leaving_context)
{
    HashTable<String> listening;
    runtime().for_each_context([&](Context const& context) {
        if (context.id == leaving_context || !context.has_listener("idle.onStateChanged"sv) || refusal(context, "idle"sv).has_value())
            return;
        listening.set(context.extension_id);
    });

    m_idle_states.remove_all_matching([&](auto const& extension_id, auto const&) { return !listening.contains(extension_id); });
    // Chrome starts from "active": a user idle already is told so at the first look.
    for (auto const& extension_id : listening)
        m_idle_states.ensure(extension_id, [] { return "active"sv; });

    if (m_idle_states.is_empty()) {
        if (m_idle_timer)
            m_idle_timer->stop();
        return;
    }
    if (!m_idle_timer)
        m_idle_timer = Core::Timer::create_repeating(s_idle_poll_interval, [this] { poll_idle_state(); });
    if (!m_idle_timer->is_active())
        m_idle_timer->start(s_idle_poll_interval);
}

void AlarmsAPI::poll_idle_state()
{
    auto state = system_idle_state();
    Vector<String> extension_ids;
    for (auto const& [extension_id, last_state] : m_idle_states)
        extension_ids.append(extension_id);
    for (auto const& extension_id : extension_ids) {
        auto name = idle_state_name(state, m_idle_thresholds.get(extension_id).value_or(default_idle_threshold));
        auto last = m_idle_states.find(extension_id);
        if (last == m_idle_states.end() || last->value == name)
            continue;
        last->value = name;
        runtime().dispatch_event(extension_id, "idle.onStateChanged"sv, JsonArray { Vector<JsonValue> { JsonValue { name } } });
    }
}

// i18n.detectLanguage's guess: the language of a script used by one language (or nearly), when it's the text's main
// script. Text in Latin, Cyrillic and other shared scripts isn't guessed at.
static Optional<StringView> guess_language(StringView text)
{
    struct Script {
        StringView language; // Empty for scripts shared by many languages.
        u32 first;
        u32 last;
    };
    static constexpr Script scripts[] = {
        { "ja"sv, 0x3040, 0x30FF }, // Hiragana and katakana
        { "ja"sv, 0x31F0, 0x31FF },
        { "zh"sv, 0x3400, 0x4DBF }, // Han
        { "zh"sv, 0x4E00, 0x9FFF },
        { "ko"sv, 0x1100, 0x11FF }, // Hangul
        { "ko"sv, 0x3130, 0x318F },
        { "ko"sv, 0xAC00, 0xD7AF },
        { "th"sv, 0x0E00, 0x0E7F },
        { "el"sv, 0x0370, 0x03FF },
        { "he"sv, 0x0590, 0x05FF },
        { "ar"sv, 0x0600, 0x06FF },
        { "hy"sv, 0x0530, 0x058F },
        { "ka"sv, 0x10A0, 0x10FF },
        { "hi"sv, 0x0900, 0x097F },
        { {}, 'A', 'Z' }, // Latin
        { {}, 'a', 'z' },
        { {}, 0x00C0, 0x024F },
        { {}, 0x0400, 0x04FF }, // Cyrillic
    };

    OrderedHashMap<StringView, size_t> letters;
    for (auto code_point : Utf8View { text }) {
        for (auto const& script : scripts) {
            if (code_point >= script.first && code_point <= script.last) {
                ++letters.ensure(script.language, [] { return 0; });
                break;
            }
        }
    }
    // Japanese writes kanji alongside its kana.
    if (letters.contains("ja"sv)) {
        if (auto han = letters.take("zh"sv); han.has_value())
            letters.find("ja"sv)->value += *han;
    }
    Optional<StringView> most;
    size_t most_letters = 0;
    for (auto const& [language, count] : letters) {
        if (count > most_letters) {
            most = language;
            most_letters = count;
        }
    }
    if (!most.has_value() || most->is_empty())
        return {};
    return most;
}

void AlarmsAPI::register_i18n_functions()
{
    // i18n.getAcceptLanguages: the languages the browser asks websites for.
    runtime().register_function("i18n.getAcceptLanguages"sv, [](Context const&, JsonArray const&, Reply reply) {
        JsonArray languages;
        for (auto const& language : Application::settings().languages())
            languages.must_append(language);
        if (languages.is_empty())
            languages.must_append("en-US"_string);
        reply(JsonValue { move(languages) });
    });

    // i18n.detectLanguage(text): a best guess from the text's script, never claimed to be reliable.
    runtime().register_function("i18n.detectLanguage"sv, [](Context const&, JsonArray const& arguments, Reply reply) {
        if (arguments.is_empty() || !arguments.at(0).is_string())
            return reply("i18n.detectLanguage needs a string"_string);
        JsonArray languages;
        if (auto language = guess_language(arguments.at(0).as_string()); language.has_value()) {
            JsonObject detected;
            detected.set("language"sv, *language);
            detected.set("percentage"sv, 100);
            languages.must_append(move(detected));
        }
        JsonObject result;
        result.set("isReliable"sv, false);
        result.set("languages"sv, move(languages));
        reply(JsonValue { move(result) });
    });
}

}
