#include "meton_game_binding.h"
#include "meton_game_support.generated.h"
#include "meton_game_lifecycle.generated.h"
#include "meton_js_runtime.h"
#include "scene/main/node.h"
#include "core/os/thread.h"
#include <atomic>
#include <cmath>
#include <vector>

namespace {
constexpr v8::ExternalPointerTypeTag HANDLE_TAG = 3;
enum Handle { CALLABLE = 1, SIGNAL = 2, SCOPE = 3, SUBSCRIPTION = 4, WAIT = 5 };
enum Operation { GET_SIGNAL, CREATE_CALLABLE, CONNECT, DISCONNECT, SCOPE_OF, SCOPE_CONNECT, SCOPE_WAIT, SUB_DISCONNECT, WAIT_CANCEL };
std::atomic<uint64_t> next_identity{1};
static_assert(Object::CONNECT_ONE_SHOT == MetonLifecycleSupport::SIGNAL_FLAGS, "固定信号 flags 与原生定义不符");
}

/** 原生连接保留 Callable 身份，目标和 VM 都只弱绑定，不延长 Node/引擎实例寿命。 */
class MetonGameCallable : public CallableCustom {
    std::weak_ptr<MetonGameBinding::Lifetime> lifetime;
    ObjectID object;
    uint32_t token;
    uint64_t identity = next_identity.fetch_add(1);
    static bool equal(const CallableCustom *p_a, const CallableCustom *p_b) {
        return static_cast<const MetonGameCallable *>(p_a)->identity == static_cast<const MetonGameCallable *>(p_b)->identity;
    }
    static bool less(const CallableCustom *p_a, const CallableCustom *p_b) {
        return static_cast<const MetonGameCallable *>(p_a)->identity < static_cast<const MetonGameCallable *>(p_b)->identity;
    }
public:
    MetonGameCallable(std::shared_ptr<MetonGameBinding::Lifetime> p_lifetime, ObjectID p_object, uint32_t p_token) :
            lifetime(p_lifetime), object(p_object), token(p_token) {}
    uint32_t hash() const override { return uint32_t(identity ^ (identity >> 32)); }
    String get_as_text() const override { return "MetonGameCallable"; }
    CompareEqualFunc get_compare_equal_func() const override { return equal; }
    CompareLessFunc get_compare_less_func() const override { return less; }
    ObjectID get_object() const override { return object; }
    int get_argument_count(bool &r_valid) const override { r_valid = true; return 0; }
    bool is_valid() const override {
        if (!Thread::is_main_thread()) return false;
        auto live = lifetime.lock();
        return live && live->binding && live->binding->callable_valid(token);
    }
    void call(const Variant **p_args, int p_count, Variant &r_result, Callable::CallError &r_error) const override {
        // Godot 对已失效弱目标的迟到信号不进入旧 VM；它不是新的执行请求。
        r_error.error = Callable::CallError::CALL_OK;
        if (!Thread::is_main_thread()) return;
        auto live = lifetime.lock();
        if (!live || !live->binding || !live->binding->callable_valid(token)) return;
        live->binding->runtime->dispatch_game_callback(token, "_meton_signal", p_args, p_count);
    }
};

bool MetonGameBinding::scope_valid(Stamp p_stamp, bool p_new_work) const {
    if (closed || revoked) return false;
    auto found = objects.find(p_stamp.owner);
    return found != objects.end() && found->second.in_tree && found->second.epoch == p_stamp.epoch &&
            (!p_new_work || !found->second.closing) && ObjectDB::get_instance(found->second.native);
}
bool MetonGameBinding::callable_valid(uint32_t p_token) const {
    if (closed || revoked) return false;
    auto found = callables.find(p_token);
    if (found == callables.end() || found->second.wrapper.IsEmpty()) return false;
    auto target = objects.find(found->second.target);
    if (target == objects.end()) return false;
    auto *native = Object::cast_to<Node>(ObjectDB::get_instance(target->second.native));
    return native && !native->is_queued_for_deletion() && (!found->second.scoped.owner || scope_valid(found->second.scoped, false));
}
v8::MaybeLocal<v8::Object> MetonGameBinding::handle(int p_kind, uint32_t p_token, uint64_t p_epoch) {
    v8::Local<v8::Object> object;
    if (!handles.at(p_kind).Get(isolate)->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()).ToLocal(&object)) return {};
    object->SetInternalField(0, v8::External::New(isolate, this, HANDLE_TAG));
    object->SetInternalField(1, v8::Int32::New(isolate, p_kind));
    object->SetInternalField(2, v8::Uint32::New(isolate, p_token));
    object->SetInternalField(3, v8::BigInt::NewFromUnsigned(isolate, p_epoch));
    object->SetInternalField(4, v8::Undefined(isolate));
    return object;
}
bool MetonGameBinding::unhandle(v8::Local<v8::Value> p_value, int p_kind, uint32_t &r_token, uint64_t &r_epoch) {
    if (!p_value->IsObject()) { error("invalid_argument", -1, "Expected native handle"); return false; }
    auto object = p_value.As<v8::Object>();
    if (object->InternalFieldCount() != 5 || !object->GetInternalField(0)->IsValue() || !object->GetInternalField(0).As<v8::Value>()->IsExternal() ||
            object->GetInternalField(0).As<v8::External>()->Value(HANDLE_TAG) != this ||
            !object->GetInternalField(1)->IsValue() || !object->GetInternalField(1).As<v8::Value>()->IsInt32() ||
            object->GetInternalField(1).As<v8::Int32>()->Value() != p_kind ||
            !object->GetInternalField(2)->IsValue() || !object->GetInternalField(2).As<v8::Value>()->IsUint32() ||
            !object->GetInternalField(3)->IsValue() || !object->GetInternalField(3).As<v8::Value>()->IsBigInt()) {
        error("invalid_argument", -1, "Native handle identity mismatch"); return false;
    }
    r_token = object->GetInternalField(2).As<v8::Uint32>()->Value();
    r_epoch = object->GetInternalField(3).As<v8::BigInt>()->Uint64Value();
    return true;
}
void MetonGameBinding::weak_callable(const v8::WeakCallbackInfo<CallableEntry> &p_info) {
    auto *entry = p_info.GetParameter();
    const uint32_t id = entry->id;
    auto *binding = entry->binding;
    entry->wrapper.Reset();
    binding->callables.erase(id);
}
v8::MaybeLocal<v8::Object> MetonGameBinding::create_callable(uint32_t p_target, v8::Local<v8::Function> p_method, Stamp p_scope) {
    if (p_method->IsAsyncFunction()) { error("invalid_argument", -1, "Callable requires a synchronous function"); return {}; }
    if (callables.size() >= MetonLifecycleSupport::CONNECTIONS) { error("budget_exceeded", -1, "Callable handle budget exceeded"); return {}; }
    const uint32_t id = next_callable++;
    v8::Local<v8::Object> wrapper;
    if (!handle(CALLABLE, id).ToLocal(&wrapper)) return {};
    wrapper->SetInternalField(4, p_method);
    auto &entry = callables[id];
    entry.binding = this; entry.id = id; entry.target = p_target; entry.scoped = p_scope;
    entry.native = Callable(memnew(MetonGameCallable(lifetime, objects.at(p_target).native, id)));
    entry.wrapper.Reset(isolate, wrapper);
    // 函数只藏在 JS 包装内，原生连接存在时才强保留整个包装；断开后 JS 环可由 GC 回收。
    entry.wrapper.SetWeak(&entry, weak_callable, v8::WeakCallbackType::kParameter);
    return wrapper;
}
void MetonGameBinding::release_capture(uint32_t p_callable) {
    for (const auto &entry : connections) if (entry.second.callable == p_callable) return;
    auto found = callables.find(p_callable);
    if (found != callables.end()) found->second.retained.Reset();
}
void MetonGameBinding::prune_connections() {
    prune_objects();
    prune_resources();
    std::vector<uint32_t> retired;
    for (const auto &entry : connections) {
        auto callable = callables.find(entry.second.callable);
        if (callable == callables.end() || !ObjectDB::get_instance(entry.second.signal.get_object_id()) || !entry.second.signal.is_connected(callable->second.native))
            retired.push_back(entry.first);
    }
    for (auto id : retired) retire_connection(id, "cancelled");
}
void MetonGameBinding::retire_connection(uint32_t p_connection, const char *p_reason) {
    auto found = connections.find(p_connection);
    if (found == connections.end()) return;
    auto &entry = found->second;
    const uint32_t target = entry.callable;
    auto callable = callables.find(target);
    if (callable != callables.end() && ObjectDB::get_instance(entry.signal.get_object_id()) && entry.signal.is_connected(callable->second.native))
        entry.signal.disconnect(callable->second.native);
    if (!entry.waiter.IsEmpty() && !revoked && !isolate->IsExecutionTerminating()) {
        auto ctx = isolate->GetCurrentContext();
        auto resolver = entry.waiter.Get(isolate);
        if (p_reason) {
            auto failure = v8::Exception::Error(text(p_reason)).As<v8::Object>();
            (void)failure->CreateDataProperty(ctx, text("code"), text(p_reason)).FromMaybe(false);
            (void)resolver->Reject(ctx, failure).FromMaybe(false);
        } else {
            auto values = v8::Array::New(isolate, 0);
            (void)values->SetIntegrityLevel(ctx, v8::IntegrityLevel::kFrozen).FromMaybe(false);
            (void)resolver->Resolve(ctx, values).FromMaybe(false);
        }
    }
    entry.waiter.Reset();
    connections.erase(found);
    release_capture(target);
}
void MetonGameBinding::cleanup_scope(uint32_t p_owner, bool p_destroyed) {
    std::vector<uint32_t> retiring;
    const auto &owner = objects.at(p_owner);
    for (const auto &entry : connections) {
        auto callable = callables.find(entry.second.callable);
        if (entry.second.scope.owner == p_owner || (p_destroyed &&
                (entry.second.signal.get_object_id() == owner.native || (callable != callables.end() && callable->second.target == p_owner))))
            retiring.push_back(entry.first);
    }
    for (uint32_t id : retiring) retire_connection(id, p_destroyed ? "object_invalid" : "scope_ended");
    if (p_destroyed) for (auto &entry : callables) if (entry.second.target == p_owner && !entry.second.wrapper.IsEmpty()) {
        // 原生弱目标已结束，不因外部仍拿着 Callable 而继续保留其用户闭包。
        entry.second.wrapper.Get(isolate)->SetInternalField(4, v8::Undefined(isolate));
        entry.second.retained.Reset();
    }
}
bool MetonGameBinding::call_signal(uint32_t p_token, const Variant **p_args, int p_count) {
    if (!callable_valid(p_token)) return true;
    if (p_count != 0) { error("invalid_argument", -1, "Only the admitted zero-argument signal is supported"); return false; }
    auto &callable = callables.at(p_token);
    auto &target = objects.at(callable.target);
    auto wrapper = callable.wrapper.Get(isolate);
    auto function = wrapper->GetInternalField(4).As<v8::Value>();
    std::vector<uint32_t> waits;
    for (const auto &entry : connections) if (entry.second.callable == p_token && !entry.second.waiter.IsEmpty()) waits.push_back(entry.first);
    if (!waits.empty()) {
        for (uint32_t id : waits) retire_connection(id);
        return true;
    }
    if (!function->IsFunction()) return true;
    Stamp previous = current;
    const uint32_t previous_phase = phase;
    current = callable.scoped.owner ? callable.scoped : Stamp{callable.target, target.epoch};
    phase = target.closing ? MetonGameSupport::P_CLEANUP : MetonGameSupport::P_SIGNAL;
    auto ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> returned;
    active_native_objects.push_back(target.native);
    bool ok = function.As<v8::Function>()->Call(ctx, target.wrapper.Get(isolate), 0, nullptr).ToLocal(&returned);
    if (ok && returned->IsObject()) {
        v8::Local<v8::Value> then;
        ok = returned.As<v8::Object>()->Get(ctx, text("then")).ToLocal(&then);
        if (ok && (returned->IsPromise() || then->IsFunction())) {
            error("invalid_argument", -1, "Synchronous signal listener returned a thenable"); ok = false;
        }
    }
    active_native_objects.pop_back();
    current = previous; phase = previous_phase;
    return ok;
}
void MetonGameBinding::handle_construct(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    self(p_args.GetIsolate())->error("invalid_argument", -1, "Native handle constructors are private");
}
void MetonGameBinding::signal_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int operation = p_args.DataV2().As<v8::Int32>()->Value();
    auto ctx = binding->isolate->GetCurrentContext();
    const bool teardown = operation == DISCONNECT || operation == SUB_DISCONNECT || operation == WAIT_CANCEL;
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Execution scope has ended"); return; }
    if (binding->phase == MetonGameSupport::P_MODULE || binding->phase == MetonGameSupport::P_CONSTRUCT ||
            (binding->phase == MetonGameSupport::P_CLEANUP && !teardown && operation != GET_SIGNAL && operation != SCOPE_OF)) {
        binding->error("phase_conflict", -1, "Signal operation is unavailable in this phase"); return;
    }
    if (!binding->charge_call(-1)) return;
    if (operation == GET_SIGNAL || operation == SCOPE_OF) {
        if (p_args.Length() != (operation == GET_SIGNAL ? 0 : 1)) { binding->error("invalid_argument", -1, "Argument count mismatch"); return; }
        auto receiver = operation == GET_SIGNAL ? v8::Local<v8::Value>(p_args.This()) : p_args[0];
        if (!receiver->IsObject() || !binding->node(receiver.As<v8::Object>(), -1)) return;
        const uint32_t token = receiver.As<v8::Object>()->GetInternalField(1).As<v8::Uint32>()->Value();
        auto &entry = binding->objects.at(token);
        if (operation == SCOPE_OF && !binding->scope_valid({token, entry.epoch}, true)) { binding->error("scope_ended", -1, "Node is outside an active tree entry"); return; }
        auto &stored = operation == GET_SIGNAL ? entry.signal : entry.scope;
        if (stored.IsEmpty()) {
            v8::Local<v8::Object> wrapper;
            if (!binding->handle(operation == GET_SIGNAL ? SIGNAL : SCOPE, token, operation == GET_SIGNAL ? 0 : entry.epoch).ToLocal(&wrapper)) return;
            stored.Reset(binding->isolate, wrapper);
        }
        p_args.GetReturnValue().Set(stored.Get(binding->isolate)); return;
    }
    if (operation == CREATE_CALLABLE) {
        if (p_args.Length() != 2 || !p_args[0]->IsObject() || !p_args[1]->IsFunction()) { binding->error("invalid_argument", -1, "Callable.create requires a node and function"); return; }
        if (!binding->node(p_args[0].As<v8::Object>(), -1)) return;
        const uint32_t target = p_args[0].As<v8::Object>()->GetInternalField(1).As<v8::Uint32>()->Value();
        v8::Local<v8::Object> callable;
        if (binding->create_callable(target, p_args[1].As<v8::Function>(), {}).ToLocal(&callable)) p_args.GetReturnValue().Set(callable);
        return;
    }
    uint32_t token; uint64_t epoch;
    const int kind = operation == CONNECT || operation == DISCONNECT ? SIGNAL :
        operation == SUB_DISCONNECT ? SUBSCRIPTION : operation == WAIT_CANCEL ? WAIT : SCOPE;
    if (!binding->unhandle(p_args.This(), kind, token, epoch)) return;
    if (operation == SUB_DISCONNECT || operation == WAIT_CANCEL) {
        if (p_args.Length()) { binding->error("invalid_argument", -1, "Unexpected arguments"); return; }
        binding->retire_connection(token, "cancelled"); return;
    }
    if (operation == SCOPE_CONNECT || operation == SCOPE_WAIT) {
        if (!binding->scope_valid({token, epoch}, true)) { binding->error("scope_ended", -1, "Tree entry has ended"); return; }
        if (p_args.Length() != (operation == SCOPE_CONNECT ? 2 : 1) || (operation == SCOPE_CONNECT && !p_args[1]->IsFunction())) {
            binding->error("invalid_argument", -1, "Invalid scope connection arguments"); return;
        }
    } else if (p_args.Length() < 1 || p_args.Length() > (operation == CONNECT ? 2 : 1)) {
        binding->error("invalid_argument", -1, "Invalid signal connection arguments"); return;
    }
    uint32_t source = token; uint64_t unused = 0; uint32_t callable_id;
    Stamp scope;
    v8::Local<v8::Object> callable_wrapper;
    v8::Local<v8::Promise::Resolver> resolver;
    if (kind == SCOPE) {
        scope = {token, epoch};
        if (!binding->unhandle(p_args[0], SIGNAL, source, unused)) return;
    }
    auto found = binding->objects.find(source);
    if (found == binding->objects.end() || !ObjectDB::get_instance(found->second.native)) { binding->error("object_invalid", -1, "Signal source lifetime has ended"); return; }
    Signal signal(found->second.native, "tree_exiting");
    if (kind == SCOPE) {
        v8::Local<v8::Function> listener;
        if (operation == SCOPE_CONNECT) listener = p_args[1].As<v8::Function>();
        else {
            // 等待回调不运行用户函数，原生信号只结算对应的 Promise。
            if (!v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &) {}).ToLocal(&listener) || !v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return;
            binding->stamp_promise(resolver->GetPromise(), scope);
        }
        if (!binding->create_callable(token, listener, scope).ToLocal(&callable_wrapper)) return;
        callable_id = callable_wrapper->GetInternalField(2).As<v8::Uint32>()->Value();
    } else {
        if (!binding->unhandle(p_args[0], CALLABLE, callable_id, unused)) return;
        if (!binding->callable_valid(callable_id)) { binding->error("object_invalid", -1, "Callable target lifetime has ended"); return; }
        callable_wrapper = p_args[0].As<v8::Object>();
    }
    auto &callable = binding->callables.at(callable_id);
    if (operation == DISCONNECT) {
        signal.disconnect(callable.native);
        std::vector<uint32_t> retired;
        for (const auto &entry : binding->connections) if (entry.second.callable == callable_id && entry.second.signal == signal) retired.push_back(entry.first);
        for (auto id : retired) binding->retire_connection(id, "cancelled");
        return;
    }
    uint32_t flags = operation == SCOPE_WAIT ? Object::CONNECT_ONE_SHOT : 0;
    if (operation == CONNECT && p_args.Length() == 2) {
        if (!p_args[1]->IsUint32() || (p_args[1].As<v8::Uint32>()->Value() & ~MetonLifecycleSupport::SIGNAL_FLAGS)) {
            binding->error("invalid_argument", -1, "Unsupported signal connection flags"); return;
        }
        flags = p_args[1].As<v8::Uint32>()->Value();
    }
    if (binding->connections.size() >= MetonLifecycleSupport::CONNECTIONS) { binding->error("budget_exceeded", -1, "Signal connection budget exceeded"); return; }
    // 返回原生 Error；作用域助手只有真实连接成功后才建立订阅，不伪造成功句柄。
    Error result = signal.connect(callable.native, flags);
    if (result != OK) {
        if (kind == SCOPE) binding->error("invalid_argument", -1, "Native scoped connection failed");
        else p_args.GetReturnValue().Set(int(result));
        return;
    }
    const uint32_t connection_id = binding->next_connection++;
    auto &connection = binding->connections[connection_id];
    connection.signal = signal; connection.callable = callable_id; connection.scope = scope;
    callable.retained.Reset(binding->isolate, callable_wrapper);
    if (operation == CONNECT) { p_args.GetReturnValue().Set(int(result)); return; }
    v8::Local<v8::Object> output;
    if (!binding->handle(operation == SCOPE_WAIT ? WAIT : SUBSCRIPTION, connection_id).ToLocal(&output)) {
        binding->retire_connection(connection_id, "cancelled"); return;
    }
    if (operation == SCOPE_WAIT) {
        connection.waiter.Reset(binding->isolate, resolver);
        if (!output->DefineOwnProperty(ctx, binding->text("result"), resolver->GetPromise(), v8::ReadOnly).FromMaybe(false)) {
            binding->retire_connection(connection_id, "cancelled"); return;
        }
    }
    p_args.GetReturnValue().Set(output);
}
bool MetonGameBinding::signal_exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module) {
    auto method = [&](int operation) { return v8::FunctionTemplate::New(isolate, signal_dispatch, v8::Int32::New(isolate, operation)); };
    for (const auto &definition : {std::pair<int, const char *>{CALLABLE, "Callable"}, {SIGNAL, "Signal"}, {SCOPE, "EngineScope"}, {SUBSCRIPTION, "Subscription"}, {WAIT, "SignalWait"}}) {
        auto type = v8::FunctionTemplate::New(isolate, handle_construct);
        type->SetClassName(text(definition.second));
        type->InstanceTemplate()->SetInternalFieldCount(5);
        if (definition.first == CALLABLE) type->Set(text("create"), method(CREATE_CALLABLE));
        else if (definition.first == SIGNAL) {
            type->PrototypeTemplate()->Set(text("connect"), method(CONNECT));
            type->PrototypeTemplate()->Set(text("disconnect"), method(DISCONNECT));
        } else if (definition.first == SCOPE) {
            type->PrototypeTemplate()->Set(text("connect"), method(SCOPE_CONNECT));
            type->PrototypeTemplate()->Set(text("wait"), method(SCOPE_WAIT));
        } else type->PrototypeTemplate()->Set(text(definition.first == WAIT ? "cancel" : "disconnect"), method(definition.first == WAIT ? WAIT_CANCEL : SUB_DISCONNECT));
        handles[definition.first].Reset(isolate, type);
        if (definition.first <= SIGNAL) {
            v8::Local<v8::Function> constructor;
            if (!type->GetFunction(p_context).ToLocal(&constructor) || !p_module->SetSyntheticModuleExport(isolate, text(definition.second), constructor).FromMaybe(false)) return false;
        }
    }
    // Node 构造器尚未实例化；此处添加继承的真实信号访问器。
    classes.at("Node").Get(isolate)->PrototypeTemplate()->SetAccessorProperty(text("tree_exiting"), method(GET_SIGNAL));
    return true;
}
v8::MaybeLocal<v8::Promise> MetonGameBinding::evaluate_helpers(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module) {
    auto *binding = self(v8::Isolate::GetCurrent());
    v8::Local<v8::Function> function;
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Function::New(p_context, signal_dispatch, v8::Int32::New(binding->isolate, SCOPE_OF)).ToLocal(&function) ||
            !p_module->SetSyntheticModuleExport(binding->isolate, binding->text("scopeOf"), function).FromMaybe(false) ||
            !v8::Function::New(p_context, scene_of).ToLocal(&function) ||
            !p_module->SetSyntheticModuleExport(binding->isolate, binding->text("sceneOf"), function).FromMaybe(false) ||
            !v8::Promise::Resolver::New(p_context).ToLocal(&resolver) ||
            !resolver->Resolve(p_context, v8::Undefined(binding->isolate)).FromMaybe(false)) return {};
    return resolver->GetPromise();
}
v8::Local<v8::Module> MetonGameBinding::helpers_module() {
    const std::array<v8::Local<v8::String>, 2> names = {text("scopeOf"), text("sceneOf")};
    return v8::Module::CreateSyntheticModule(isolate, text("@meton/sdk/game/engine"), names, evaluate_helpers);
}
