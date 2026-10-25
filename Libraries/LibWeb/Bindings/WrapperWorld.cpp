/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(WrapperWorld);

static void verify_cache_entry(WrapperWorld const& world, Wrappable const& wrappable, JS::HostObject const& wrapper)
{
    VERIFY(wrapper.shape().realm().host_defined());
    VERIFY(&host_defined_wrapper_world(wrapper.shape().realm()) == &world);
    VERIFY(wrappable_impl_from(&wrapper) == &wrappable);
}

WrapperWorld::WrapperWorld(Type type)
    : m_type(type)
{
}

WrapperWorld::~WrapperWorld() = default;

void WrapperWorld::finalize()
{
    Base::finalize();
    VERIFY(is_main_world() || is_detached() || m_preserved_wrappables.is_empty() || heap().is_collecting_everything());
}

void WrapperWorld::detach()
{
    VERIFY(!is_main_world());
    if (m_detached)
        return;
    m_detached = true;
    for (auto& wrappable : m_preserved_wrappables)
        wrappable.clear_preserved_wrappers(*this);
    m_preserved_wrappables.clear();
    m_wrappers.clear();
    m_rust_wrappers.clear();
}

void WrapperWorld::register_preserved_wrappable(GCAllocatedWrappable& wrappable)
{
    VERIFY(!is_main_world());
    VERIFY(!is_detached());
    m_preserved_wrappables.set(wrappable);
}

GC::Ptr<JS::HostObject> WrapperWorld::wrapper_for(Wrappable const& wrappable, JS::Realm& realm) const
{
    if (is_main_world()) {
        if (auto wrapper = wrappable.cached_main_world_wrapper(*this))
            return wrapper;
        return nullptr;
    }

    if (auto* wrapper = m_wrappers.get(wrappable)) {
        VERIFY(&wrapper->shape().realm() == &realm);
        return const_cast<JS::HostObject*>(wrapper);
    }
    return nullptr;
}

void WrapperWorld::set_wrapper(Wrappable& wrappable, JS::HostObject& wrapper)
{
    VERIFY(!is_detached());
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.set_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        VERIFY(&existing->shape().realm() == &wrapper.shape().realm());
        return;
    }
    m_wrappers.set(wrappable, wrapper);
}

void WrapperWorld::clear_wrapper(Wrappable& wrappable, JS::HostObject const& wrapper)
{
    verify_cache_entry(*this, wrappable, wrapper);

    if (is_main_world()) {
        wrappable.clear_cached_main_world_wrapper(wrapper);
        return;
    }

    if (auto* existing = m_wrappers.get(wrappable)) {
        VERIFY(existing == &wrapper);
        m_wrappers.remove(wrappable);
    }
}

GC::Ptr<JS::HostObject> WrapperWorld::rust_wrapper_for(RustInterfaceCell const& cell, JS::Realm& realm) const
{
    VERIFY(!is_main_world());
    auto* wrapper = m_rust_wrappers.get(cell);
    if (!wrapper)
        return nullptr;
    VERIFY(&wrapper->shape().realm() == &realm);
    return const_cast<JS::HostObject*>(wrapper);
}

void WrapperWorld::set_rust_wrapper(RustInterfaceCell& cell, JS::HostObject& wrapper)
{
    VERIFY(!is_main_world());
    VERIFY(!is_detached());
    VERIFY(wrapper.shape().realm().host_defined());
    VERIFY(&host_defined_wrapper_world(wrapper.shape().realm()) == this);
    VERIFY(JS::host_data_of(wrapper).ptr() == GC::as_cell(&cell));
    // NB: The entry of a wrapper that was collected stays in the map until the map prunes it. get() does not return
    //     the wrapper of such an entry, and set() replaces it.
    VERIFY(!m_rust_wrappers.get(cell));
    m_rust_wrappers.set(cell, wrapper);
}

WrapperWorld& host_defined_wrapper_world(JS::Realm& realm)
{
    return *host_defined_of(realm).wrapper_world;
}

WrapperWorld const& host_defined_wrapper_world(JS::Realm const& realm)
{
    return *host_defined_of(realm).wrapper_world;
}

static bool belongs_to_world(JS::Object const& object, JS::Realm const& realm, WrapperWorld const& world)
{
    auto const& object_realm = object.shape().realm();
    return &object_realm == &realm || !object_realm.host_defined() || &host_defined_wrapper_world(object_realm) == &world;
}

namespace {

// Copies a value of another world into this one. Platform objects become this world's wrappers of them, a WindowProxy
// this world's, a promise this world's promise of its result; arrays and plain objects (what the engine makes for
// script, and what script makes most) are copied member by member, and anything else is structured-cloned. What can't
// be copied (a function, say) makes the whole copy fail, as a structured clone does.
class WorldCopier {
public:
    WorldCopier(JS::Realm& realm, WrapperWorld& world)
        : m_realm(realm)
        , m_world(world)
    {
    }

    Optional<JS::Value> copy(JS::Value value, size_t depth = 0)
    {
        if (!value.is_object())
            return value;
        auto& object = value.as_object();
        if (belongs_to_world(object, m_realm, m_world))
            return value;
        if (depth > max_depth)
            return {};
        for (size_t index = 0; index < m_sources.size(); ++index) {
            if (m_sources[index] == &object)
                return m_copies[index];
        }

        if (auto* window_proxy = HTML::WindowProxy::from_object(object)) {
            auto browsing_context = window_proxy->associated_browsing_context();
            if (!browsing_context)
                return JS::js_null();
            if (auto* proxy = browsing_context->window_proxy_for(m_world, m_realm))
                return JS::Value { &proxy->object() };
            return JS::js_null();
        }
        if (auto* wrappable = wrappable_impl_from(&object))
            return JS::Value { wrap(m_world, m_realm, GC::Ref { *wrappable }) };
        if (is<JS::Promise>(object)) {
            // NB: A promise met inside a value isn't one this world's script asked for: its
            //     rejection isn't this world's to report.
            auto copy = promise_for_world(m_realm, object);
            if (copy.ptr() != &object)
                as<JS::Promise>(*copy).set_is_handled();
            return JS::Value { copy };
        }

        if (is<JS::Array>(object))
            return copy_array(object, depth);
        if (is_plain_object(object)) {
            if (auto result = copy_plain_object(object, depth); result.has_value())
                return result;
        }
        return structured_clone(value);
    }

private:
    static constexpr size_t max_depth = 64;
    // NB: A long (and likely sparse) array is left to the structured clone, which copies only what's there.
    static constexpr size_t max_elementwise_length = 65536;

    static bool is_plain_object(JS::Object const& object)
    {
        if (object.is_function() || object.is_platform_object() || object.class_name() != "Object"sv)
            return false;
        auto const* prototype = object.prototype();
        return !prototype || prototype == object.shape().realm().intrinsics().object_prototype().ptr();
    }

    void remember(JS::Object& source, JS::Object& copy)
    {
        m_sources.append(&source);
        m_copies.append(&copy);
    }

    Optional<JS::Value> copy_array(JS::Object& object, size_t depth)
    {
        auto length = JS::length_of_array_like(m_realm.vm(), object);
        if (length.is_error())
            return {};
        if (length.value() > max_elementwise_length)
            return structured_clone(&object);
        auto array = MUST(JS::Array::create(m_realm, 0));
        remember(object, array);
        for (size_t index = 0; index < length.value(); ++index) {
            auto element = object.get(JS::PropertyKey { index });
            if (element.is_error())
                return {};
            auto element_copy = copy(element.value(), depth + 1);
            if (!element_copy.has_value())
                return {};
            MUST(array->create_data_property_or_throw(JS::PropertyKey { index }, *element_copy));
        }
        return JS::Value { array };
    }

    // Only an object with nothing but data properties is copied here; what has getters is the structured clone's.
    Optional<JS::Value> copy_plain_object(JS::Object& object, size_t depth)
    {
        auto keys = object.internal_own_property_keys();
        if (keys.is_error())
            return {};
        Vector<JS::PropertyKey> data_keys;
        for (auto key_value : keys.value()) {
            if (!key_value.is_string())
                continue;
            auto key = MUST(JS::PropertyKey::from_value(m_realm.vm(), key_value));
            auto descriptor = object.internal_get_own_property(key);
            if (descriptor.is_error() || !descriptor.value().has_value())
                continue;
            if (!descriptor.value()->is_data_descriptor())
                return {};
            if (descriptor.value()->enumerable.value_or(false))
                data_keys.append(key);
        }

        auto copy_object = JS::Object::create(m_realm, m_realm.intrinsics().object_prototype());
        remember(object, copy_object);
        for (auto const& key : data_keys) {
            auto member = object.get(key);
            if (member.is_error())
                return {};
            auto member_copy = copy(member.value(), depth + 1);
            if (!member_copy.has_value())
                return {};
            MUST(copy_object->create_data_property_or_throw(key, *member_copy));
        }
        return JS::Value { copy_object };
    }

    // NB: Serializing reads the value's properties, which may run its world's getters; those only ever see their own
    //     world's objects.
    Optional<JS::Value> structured_clone(JS::Value value)
    {
        auto& vm = m_realm.vm();
        auto serialized = HTML::structured_serialize(vm, value);
        if (serialized.is_error())
            return {};
        auto deserialized = HTML::structured_deserialize(vm, serialized.value(), m_realm);
        if (deserialized.is_error())
            return {};
        return deserialized.release_value();
    }

    JS::Realm& m_realm;
    WrapperWorld& m_world;
    Vector<JS::Object*> m_sources;
    GC::RootVector<JS::Value> m_copies;
};

}

// Elytra: see the header.
JS::Value value_for_world(JS::Realm& realm, JS::Value value)
{
    if (!value.is_object() || !realm.host_defined())
        return value;
    auto& world = host_defined_wrapper_world(realm);
    if (belongs_to_world(value.as_object(), realm, world))
        return value;
    // NB: A promise handed to script as it is (one an operation returns) is this world's to report, should it reject.
    if (is<JS::Promise>(value.as_object()))
        return promise_for_world(realm, value.as_object());
    WorldCopier copier { realm, world };
    return copier.copy(value).value_or(JS::js_null());
}

// Elytra: see the header.
GC::Ref<JS::Object> promise_for_world(JS::Realm& realm, JS::Object& promise)
{
    if (!realm.host_defined() || belongs_to_world(promise, realm, host_defined_wrapper_world(realm)) || !is<JS::Promise>(promise))
        return promise;

    // NB: The reactions are the engine's own, not the other world's then(), which may be anything.
    auto& source = as<JS::Promise>(promise);
    auto copy = WebIDL::create_promise(realm);
    auto target_realm = GC::Ref { realm };
    auto on_fulfilled = JS::NativeFunction::create(realm, [target_realm, copy](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        WebIDL::resolve_promise(target_realm, copy, value_for_world(target_realm, vm.argument(0)));
        return JS::js_undefined();
    }, 1);
    auto on_rejected = JS::NativeFunction::create(realm, [target_realm, copy](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        WebIDL::reject_promise(target_realm, copy, value_for_world(target_realm, vm.argument(0)));
        return JS::js_undefined();
    }, 1);
    // NB: A promise that something already handles (PromiseRejectionEvent.promise, say) isn't for this world to report.
    if (source.is_handled())
        as<JS::Promise>(*copy->promise()).set_is_handled();
    source.perform_then(on_fulfilled, on_rejected, nullptr);
    return *copy->promise();
}

// Elytra: see the header.
JS::Completion completion_for_world(JS::Realm& realm, JS::Completion completion)
{
    if (!completion.is_error())
        return completion;
    auto value = completion.release_value();
    return JS::throw_completion(value_for_world(realm, value));
}

WrapperWorld* current_non_main_world()
{
    auto& vm = main_thread_vm();
    if (vm.execution_context_stack().is_empty())
        return nullptr;
    auto* realm = vm.current_realm();
    if (!realm || !realm->host_defined())
        return nullptr;
    auto& world = host_defined_wrapper_world(*realm);
    return world.is_main_world() ? nullptr : &world;
}

}
