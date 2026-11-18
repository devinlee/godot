#include "meton_js_runtime.h"
#ifdef METON_JS_GAME_ENABLED
#include "meton_game_binding.h"
#endif
#include "meton_native_memory.h"
#ifdef METON_JS_GAME_ENABLED
#include "meton_game_memory.generated.h"
#endif
#ifdef METON_JS_GAME_ENABLED
#include "meton_game_lifecycle.generated.h"
#endif
#ifdef METON_JS_TASK_ENABLED
#include "meton_task_bridge.h"
#endif
#ifdef METON_JS_TASK_ENABLED
#include "meton_task_data.generated.h"
#endif
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#ifdef METON_JS_GAME_ENABLED
#include "scene/main/node.h"
#endif
#include <libplatform/libplatform.h>
#include <chrono>
#include <cstdlib>

namespace {
std::unique_ptr<v8::Platform> platform;
std::atomic<unsigned> live_isolates{0};
constexpr size_t HEAP_BYTES = 64 * 1024 * 1024;
constexpr size_t BUFFER_BYTES = 16 * 1024 * 1024;
constexpr int JSON_BYTES = 256 * 1024;
}

/** ArrayBuffer 外部分配独立计数，不能绕过堆预算无限申请。 */
class MetonJSRuntime::BoundedAllocator : public v8::ArrayBuffer::Allocator {
    std::mutex mutex;
    size_t used = 0;
    std::atomic<bool> &limited;
public:
    explicit BoundedAllocator(std::atomic<bool> &p_limited) : limited(p_limited) {}
    void *allocate(size_t length, bool zero) {
        std::lock_guard<std::mutex> lock(mutex);
        if (length > BUFFER_BYTES - used) {
            limited.store(true);
            return nullptr;
        }
        void *result = zero ? std::calloc(length ? length : 1, 1) : std::malloc(length ? length : 1);
        if (result) used += length;
        else limited.store(true);
        return result;
    }
    void *Allocate(size_t length) override { return allocate(length, true); }
    void *AllocateUninitialized(size_t length) override { return allocate(length, false); }
    void Free(void *data, size_t length) override {
        std::lock_guard<std::mutex> lock(mutex);
        used -= length;
        std::free(data);
    }
};

MetonJSRuntime::MetonJSRuntime() = default;

void MetonJSRuntime::initialize_engine() {
    platform = v8::platform::NewDefaultPlatform(1);
    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();
}
void MetonJSRuntime::shutdown_engine() {
    ERR_FAIL_COND_MSG(live_isolates.load() != 0, "Meton V8 仍有未释放实例");
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    platform.reset();
}
void MetonJSRuntime::_bind_methods() {
#ifdef METON_JS_GAME_ENABLED
    ClassDB::bind_method(D_METHOD("open_game", "sources", "entry", "identity", "actions", "budget_ms"), &MetonJSRuntime::open_game, DEFVAL(500));
    ClassDB::bind_method(D_METHOD("adopt_game_tree", "root", "owned"), &MetonJSRuntime::adopt_game_tree, DEFVAL(true));
    ClassDB::bind_method(D_METHOD("claim_game_fields", "node", "fields", "owner", "phases"), &MetonJSRuntime::claim_game_fields);
    ClassDB::bind_method(D_METHOD("configure_game_world", "node"), &MetonJSRuntime::configure_game_world);
    ClassDB::bind_method(D_METHOD("configure_game_ui", "node"), &MetonJSRuntime::configure_game_ui);
    ClassDB::bind_method(D_METHOD("release_game_fields", "node", "owner"), &MetonJSRuntime::release_game_fields);
    ClassDB::bind_method(D_METHOD("install_game_content", "json", "digest", "version"), &MetonJSRuntime::install_game_content);
    ClassDB::bind_method(D_METHOD("instantiate_game_scene", "path"), &MetonJSRuntime::instantiate_game_scene);
    ClassDB::bind_method(D_METHOD("get_game_content"), &MetonJSRuntime::get_game_content);
    ClassDB::bind_method(D_METHOD("collect_game_garbage"), &MetonJSRuntime::collect_game_garbage);
    ClassDB::bind_method(D_METHOD("drain_game_textures"), &MetonJSRuntime::drain_game_textures);
    ClassDB::bind_method(D_METHOD("invoke_game", "export_name", "input_json", "generation", "phase", "budget_ms"), &MetonJSRuntime::invoke_game, DEFVAL(100));
    ClassDB::bind_method(D_METHOD("get_game_support"), &MetonJSRuntime::get_game_support);
    ClassDB::bind_method(D_METHOD("get_game_objects"), &MetonJSRuntime::get_game_objects);
    ClassDB::bind_method(D_METHOD("get_game_lifecycle"), &MetonJSRuntime::get_game_lifecycle);
    ClassDB::bind_method(D_METHOD("_game_checkpoint", "generation"), &MetonJSRuntime::game_checkpoint);
#endif
#ifdef METON_JS_TASK_ENABLED
    ClassDB::bind_method(D_METHOD("open_task", "sources", "entry", "validator", "validator_digest"), &MetonJSRuntime::open_task);
    ClassDB::bind_method(D_METHOD("invoke_task", "export_name", "input_json", "generation"), &MetonJSRuntime::invoke_task);
    ClassDB::bind_method(D_METHOD("checkpoint_task", "generation", "execution"), &MetonJSRuntime::checkpoint_task);
    ClassDB::bind_method(D_METHOD("request_task_cancel", "generation", "execution"), &MetonJSRuntime::request_task_cancel);
#endif
    ClassDB::bind_method(D_METHOD("request_cancel", "generation"), &MetonJSRuntime::request_cancel, DEFVAL(-1));
    ClassDB::bind_method(D_METHOD("close"), &MetonJSRuntime::close);
    ClassDB::bind_method(D_METHOD("get_info"), &MetonJSRuntime::get_info);
}
Dictionary MetonJSRuntime::response(const String &p_code, const String &p_detail) const {
    Dictionary result;
    result["ok"] = p_code == "OK";
    result["code"] = p_code;
    result["detail"] = p_detail.substr(0, 8192);
    result["generation"] = int64_t(generation);
    result["ready"] = ready;
    return result;
}
Dictionary MetonJSRuntime::wrong_thread() const {
    Dictionary result;
    result["ok"] = false;
    result["code"] = "WRONG_THREAD";
    return result;
}
String MetonJSRuntime::failure_code() const {
    if (cancelled.load()) return "CANCELLED";
    if (timed_out.load()) return "TIMEOUT";
    if (resource_limited.load()) return "RESOURCE_LIMIT";
    if (queue_limited.load()) return "EVENT_QUEUE_LIMIT";
    if (jobs_limited.load()) return "MICROTASK_LIMIT";
    return "JS_ERROR";
}
void MetonJSRuntime::begin_budget(int p_budget_ms, int p_ceiling_ms) {
    timed_out.store(false);
    jobs_limited.store(false);
    job_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CLAMP(p_budget_ms, 1, p_ceiling_ms));
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        executing = true;
    }
    // 引擎所属线程可能困在 JS/微任务中；看门线程不依赖它继续处理事件。
    watchdog = std::thread([this, deadline]() {
        std::unique_lock<std::mutex> lock(execution_mutex);
        if (!budget_changed.wait_until(lock, deadline, [this]() { return !executing; })) {
            timed_out.store(true);
            isolate->TerminateExecution();
        }
    });
}
void MetonJSRuntime::end_budget() {
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        executing = false;
    }
    budget_changed.notify_all();
    if (watchdog.joinable()) watchdog.join();
}
#ifdef METON_JS_TASK_ENABLED
void MetonJSRuntime::begin_task_budget() {
    // 作业数是每检查点限额；绝对运行截止跨检查点保持，不能通过频繁轮询延长。
    job_count = 0;
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        executing = true;
        if (cancelled.load()) isolate->TerminateExecution();
    }
    watchdog = std::thread([this, deadline = task_deadline]() {
        std::unique_lock<std::mutex> lock(execution_mutex);
        if (!budget_changed.wait_until(lock, deadline, [this]() { return !executing; })) {
            timed_out.store(true);
            isolate->TerminateExecution();
        }
    });
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::request_task_cancel(int64_t p_generation, int64_t p_execution) {
    std::lock_guard<std::mutex> lock(execution_mutex);
    const String code = p_generation < 0 || uint64_t(p_generation) != generation ? "STALE_GENERATION" :
            p_execution <= 0 || uint64_t(p_execution) != task_execution ? "STALE_EXECUTION" :
            !task_active ? "TASK_ENDED" : "OK";
    Dictionary result;
    result["ok"] = code == "OK";
    result["code"] = code;
    // 控制口回显请求身份，不把当前执行身份泄漏成原请求的结局。
    result["generation"] = p_generation;
    result["execution"] = p_execution;
    if (code != "OK") return result;
    cancelled.store(true);
    if (executing && isolate) isolate->TerminateExecution();
    return result;
}
#endif
Dictionary MetonJSRuntime::request_cancel(int64_t p_generation) {
    std::lock_guard<std::mutex> lock(execution_mutex);
    Dictionary result;
    const bool missing = p_generation == -1 && std::this_thread::get_id() != owner;
    const bool stale = p_generation != -1 && (p_generation < 0 || uint64_t(p_generation) != generation);
    result["ok"] = !missing && !stale;
    result["code"] = missing ? "GENERATION_REQUIRED" : stale ? "STALE_GENERATION" : "OK";
    result["generation"] = int64_t(generation);
    if (missing || stale) return result;
    // 同线程可以省略代次；异步调用必须绑定原实例，不能取消随后重开的实例。
    cancelled.store(true);
    if (executing && isolate) isolate->TerminateExecution();
    return result;
}
void MetonJSRuntime::release_runtime() {
    MetonNativeMemoryScope native_scope(native_memory);
    end_budget();
    std::lock_guard<std::mutex> lock(execution_mutex);
    ready = false;
    // 回调栈已经退出并停止看门线程；先撤销对象绑定，再释放仍存放内部标记的 VM。
#ifdef METON_JS_GAME_ENABLED
    if (game_binding) {
        game_binding->release();
        game_cleanup = game_binding->release_state();
        game_cleanup["generation"] = int64_t(generation);
        game_binding.reset();
    }
#endif
    unhandled_rejections.clear();
#ifdef METON_JS_TASK_ENABLED
    task_active = false;
    task_promise.Reset();
    // 私有 Context 的持久句柄须早于 Isolate 释放，不能让其析构访问已销毁 VM。
    task_bridge.reset();
#endif
    if (isolate) {
        // 所有执行函数已退出 HandleScope；这里不能在活动 Context 中 Dispose。
        module_namespace.Reset();
        context.Reset();
        modules.clear();
        isolate->Dispose();
        isolate = nullptr;
        allocator.reset();
        --live_isolates;
        ++generation;
    }
    sources.clear();
    checkpoint_queued = false;
}
v8::Local<v8::String> MetonJSRuntime::text(const String &p_value) {
    CharString value = p_value.utf8();
    return v8::String::NewFromUtf8(isolate, value.get_data(), v8::NewStringType::kNormal, value.length()).ToLocalChecked();
}
String MetonJSRuntime::from_text(v8::Local<v8::Value> p_value) {
    v8::String::Utf8Value value(isolate, p_value);
    return *value ? String::utf8(*value, value.length()) : String("JavaScript exception");
}
String MetonJSRuntime::exception_text(v8::TryCatch &p_caught) {
    if (isolate->IsExecutionTerminating()) return "JavaScript execution interrupted";
    // 隔离任务的异常也可能是 Proxy 或带 getter 的对象，不为诊断再次执行外来代码。
#ifdef METON_JS_TASK_ENABLED
    if (task_bridge) return "Task execution failed";
#endif
    String result = p_caught.HasCaught() ? from_text(p_caught.Exception()) : String("JavaScript operation failed");
    v8::Local<v8::Value> stack;
    if (!context.IsEmpty() && p_caught.StackTrace(context.Get(isolate)).ToLocal(&stack) && stack->IsString()) {
        result += "\n" + from_text(stack);
    }
    return result.substr(0, 8192);
}
bool MetonJSRuntime::valid_name(const String &p_name) {
    if (p_name == "godot" || p_name.begins_with("godot.") || p_name == "@meton/sdk/game/engine" || p_name.begins_with("meton.internal.")) return false;
    if (p_name.is_empty() || p_name.length() > 256 || p_name.begins_with("/") || p_name.contains(":") || p_name.contains("\\")) return false;
    for (int i = 0; i < p_name.length(); ++i) if (p_name[i] < 32) return false;
    for (const String &part : p_name.split("/")) if (part.is_empty() || part == "." || part == "..") return false;
    return true;
}
String MetonJSRuntime::resolve_name(const String &p_name, const String &p_parent) {
#ifdef METON_JS_GAME_ENABLED
    // 原生内部模块只认本次 open_game 的宿主入口，项目模块不能借重导出进入。
    if (p_name == "meton.internal.world" || p_name == "meton.internal.ui") return game_binding && p_parent == game_host_entry ? p_name : String();
    if (game_binding && (p_name == "godot" || p_name == "@meton/sdk/game/engine")) return p_name;
#endif
    String name = p_name;
    if (name.begins_with("./")) {
        String directory = p_parent.get_base_dir();
        name = (directory.is_empty() ? String() : directory + "/") + name.substr(2);
    }
#ifdef METON_JS_GAME_ENABLED
    if (game_binding && name == game_host_entry && p_parent != game_host_entry) return String();
#endif
    return valid_name(name) && sources.has(name) ? name : String();
}
v8::MaybeLocal<v8::Module> MetonJSRuntime::load_module(const String &p_name) {
#ifdef METON_JS_GAME_ENABLED
    if (game_binding && (p_name == "godot" || p_name == "@meton/sdk/game/engine" || p_name == "meton.internal.world" || p_name == "meton.internal.ui")) {
        auto found = modules.find(p_name);
        if (found != modules.end()) return found->second.module.Get(isolate);
        auto module = p_name == "godot" ? game_binding->module() : p_name == "meton.internal.world" ? game_binding->world_module() : p_name == "meton.internal.ui" ? game_binding->ui_module() : game_binding->helpers_module();
        modules[p_name].module.Reset(isolate, module);
        return module;
    }
#endif
    if (!valid_name(p_name) || !sources.has(p_name)) {
        isolate->ThrowException(v8::Exception::Error(text("Module not allowed: " + p_name)));
        return {};
    }
    auto found = modules.find(p_name);
    if (found != modules.end()) return found->second.module.Get(isolate);
    v8::ScriptOrigin origin(text(p_name), 0, 0, false, -1, {}, false, false, true);
    v8::ScriptCompiler::Source source(text(sources[p_name]), origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module)) return {};
    modules[p_name].module.Reset(isolate, module);
    return module;
}
v8::MaybeLocal<v8::Module> MetonJSRuntime::resolve_module(v8::Local<v8::Context> ctx, v8::Local<v8::String> specifier, v8::Local<v8::FixedArray> attributes, v8::Local<v8::Module> referrer) {
    auto *self = static_cast<MetonJSRuntime *>(v8::Isolate::GetCurrent()->GetData(0));
    String parent;
    for (auto &entry : self->modules) if (entry.second.module.Get(self->isolate) == referrer) parent = entry.first;
    String name = attributes->Length() == 0 ? self->resolve_name(self->from_text(specifier), parent) : String();
    if (name.is_empty()) {
        self->isolate->ThrowException(v8::Exception::Error(self->text("Module not allowed: " + self->from_text(specifier))));
        return {};
    }
    return self->load_module(name);
}
void MetonJSRuntime::module_value(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    p_args.GetReturnValue().Set(p_args.DataV2().As<v8::Value>());
}
v8::MaybeLocal<v8::Promise> MetonJSRuntime::import_module(v8::Local<v8::Context> ctx, v8::Local<v8::Data>, v8::Local<v8::Value> resource, v8::Local<v8::String> specifier, v8::Local<v8::FixedArray> attributes) {
    auto *self = static_cast<MetonJSRuntime *>(v8::Isolate::GetCurrent()->GetData(0));
    v8::TryCatch caught(self->isolate);
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return {};
    String name = attributes->Length() == 0 ? self->resolve_name(self->from_text(specifier), self->from_text(resource)) : String();
    // 安装端口只在宿主静态装配时交出，不能经动态导入重新取得。
    if (name == "meton.internal.world" || name == "meton.internal.ui") name = String();
    v8::Local<v8::Module> module;
    if (name.is_empty()) self->isolate->ThrowException(v8::Exception::Error(self->text("Module not allowed: " + self->from_text(specifier))));
    if (!name.is_empty() && self->load_module(name).ToLocal(&module)) {
        auto &entry = self->modules[name];
        if (module->GetStatus() == v8::Module::kUninstantiated && !module->InstantiateModule(ctx, resolve_module).FromMaybe(false)) {
            // 下方统一把实际异常交回被拒 Promise。
        } else if (module->GetStatus() == v8::Module::kErrored) {
            self->isolate->ThrowException(module->GetException());
        } else {
            if (module->GetStatus() == v8::Module::kInstantiated) {
                v8::Local<v8::Promise> evaluated;
                if (module->Evaluate(ctx).ToLocal(&evaluated)) entry.evaluation.Reset(self->isolate, evaluated);
            }
            if (!caught.HasCaught() && !self->isolate->IsExecutionTerminating()) {
                if (!entry.evaluation.IsEmpty()) {
                    v8::Local<v8::Function> fulfilled;
                    if (!v8::Function::New(ctx, module_value, module->GetModuleNamespace()).ToLocal(&fulfilled)) return {};
                    return entry.evaluation.Get(self->isolate)->Then(ctx, fulfilled);
                }
                if (module->GetStatus() == v8::Module::kEvaluated && !module->IsGraphAsync()) {
                    if (resolver->Resolve(ctx, module->GetModuleNamespace()).IsNothing()) return {};
                    return resolver->GetPromise();
                }
                self->isolate->ThrowException(v8::Exception::Error(self->text("Unresolved asynchronous module is unsupported in JR1")));
            }
        }
    }
    if (self->isolate->IsExecutionTerminating()) return {};
    v8::Local<v8::Value> error = caught.HasCaught() ? caught.Exception() : v8::Exception::Error(self->text("Module evaluation failed"));
    caught.Reset();
    if (resolver->Reject(ctx, error).IsNothing()) return {};
    return resolver->GetPromise();
}
void MetonJSRuntime::promise_hook(v8::PromiseHookType p_type, v8::Local<v8::Promise> p_promise, v8::Local<v8::Value> p_parent) {
    auto *isolate = v8::Isolate::GetCurrent();
    auto *self = static_cast<MetonJSRuntime *>(isolate->GetData(0));
#ifdef METON_JS_GAME_ENABLED
    if (self->game_binding && !self->game_binding->promise_event(p_type, p_promise, p_parent)) {
        self->queue_limited.store(true); isolate->TerminateExecution(); return;
    }
    if (self->game_binding && self->game_binding->reactions.size() + self->unhandled_rejections.size() > MetonLifecycleSupport::EVENTS_QUEUED) {
        self->queue_limited.store(true); isolate->TerminateExecution(); return;
    }
#endif
    if (p_type == v8::PromiseHookType::kBefore && ++self->job_count > self->microtask_limit()) {
        self->jobs_limited.store(true);
        isolate->TerminateExecution();
    }
}
void MetonJSRuntime::promise_rejected(v8::PromiseRejectMessage p_message) {
    auto *isolate = v8::Isolate::GetCurrent();
    auto *self = static_cast<MetonJSRuntime *>(isolate->GetData(0));
#ifdef METON_JS_GAME_ENABLED
    if (!self->game_binding) return;
#endif
#ifdef METON_JS_TASK_ENABLED
    if (!self->task_bridge) return;
#endif
    auto promise = p_message.GetPromise();
    if (p_message.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
        for (auto entry = self->unhandled_rejections.begin(); entry != self->unhandled_rejections.end(); ++entry) {
            if (entry->promise.Get(isolate) == promise) { self->unhandled_rejections.erase(entry); break; }
        }
    } else if (p_message.GetEvent() == v8::kPromiseRejectWithNoHandler) {
#ifdef METON_JS_GAME_ENABLED
        const size_t reactions = self->game_binding ? self->game_binding->reactions.size() : 0;
        const unsigned limit = MetonLifecycleSupport::EVENTS_QUEUED;
#endif
#ifdef METON_JS_TASK_ENABLED
        const size_t reactions = 0;
        const unsigned limit = MetonTaskSupport::EVENTS_QUEUED;
#endif
        if (self->unhandled_rejections.size() + reactions >= limit) {
            self->queue_limited.store(true); isolate->TerminateExecution(); return;
        }
        Rejection entry;
        entry.promise.Reset(isolate, promise);
        entry.reason.Reset(isolate, p_message.GetValue());
        self->unhandled_rejections.push_back(std::move(entry));
    }
}
#ifdef METON_JS_GAME_ENABLED
String MetonJSRuntime::unhandled_game_error() {
    if (unhandled_rejections.empty()) return {};
    auto reason = unhandled_rejections.front().reason.Get(isolate);
    String detail = from_text(reason);
    v8::Local<v8::Value> stack;
    // 错误对象的格式化也属于用户执行，当前安全点的同一预算覆盖它。
    if (reason->IsObject() && reason.As<v8::Object>()->Get(context.Get(isolate), text("stack")).ToLocal(&stack) && stack->IsString()) detail += "\n" + from_text(stack);
    return detail.substr(0, 8192);
}
#endif
size_t MetonJSRuntime::heap_limit(void *p_data, size_t p_current, size_t) {
    auto *self = static_cast<MetonJSRuntime *>(p_data);
    self->resource_limited.store(true);
    self->isolate->TerminateExecution();
    // V8 需要退出执行的临时余量；这是堆终止阈值，不宣称整个进程只有 64 MiB。
    return p_current + 16 * 1024 * 1024;
}
v8::MaybeLocal<v8::Value> MetonJSRuntime::resolved(v8::Local<v8::Value> p_value) {
    isolate->PerformMicrotaskCheckpoint();
    if (isolate->IsExecutionTerminating()) return {};
    if (!p_value->IsPromise()) return p_value;
    auto promise = p_value.As<v8::Promise>();
    promise->MarkAsHandled();
    if (promise->State() == v8::Promise::kFulfilled) return promise->Result();
    isolate->ThrowException(promise->State() == v8::Promise::kRejected ? promise->Result() : v8::Exception::Error(text("Unresolved asynchronous result is unsupported in JR1")));
    return {};
}
Dictionary MetonJSRuntime::open_in_scope(const String &p_entry) {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    v8::TryCatch caught(isolate);
    auto ctx = v8::Context::New(isolate);
    if (ctx.IsEmpty()) return response(failure_code(), "Cannot create V8 context");
    context.Reset(isolate, ctx);
    v8::Context::Scope context_scope(ctx);
    ctx->AllowCodeGenerationFromStrings(false);
#ifdef METON_JS_TASK_ENABLED
    if (task_bridge && !task_bridge->initialize(isolate, ctx)) return response(failure_code(), "Task data material initialization failed");
#endif
    v8::Local<v8::Module> module;
    if (!load_module(p_entry).ToLocal(&module) || !module->InstantiateModule(ctx, resolve_module).FromMaybe(false)) return response(failure_code(), exception_text(caught));
    v8::Local<v8::Promise> evaluated;
    v8::Local<v8::Value> value;
    if (!module->Evaluate(ctx).ToLocal(&evaluated) || !resolved(evaluated).ToLocal(&value)) return response(failure_code(), exception_text(caught));
#ifdef METON_JS_GAME_ENABLED
    if (game_binding && !unhandled_rejections.empty()) return response("JS_ERROR", unhandled_game_error());
#endif
#ifdef METON_JS_TASK_ENABLED
    if (task_bridge && !unhandled_rejections.empty()) return response("JS_ERROR", "Task module has unhandled rejection");
#endif
    modules[p_entry].evaluation.Reset(isolate, evaluated);
    module_namespace.Reset(isolate, module->GetModuleNamespace().As<v8::Object>());
    return response("OK");
}
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::invoke_in_scope(const String &p_export, const String &p_json) {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto ctx = context.Get(isolate);
    v8::Context::Scope context_scope(ctx);
    v8::TryCatch caught(isolate);
    // 只在一次外层调用开始时核对原生连接；信号发射中 ONE_SHOT 会先断开再调用，不能提早按取消结算。
    if (game_binding) game_binding->prune_connections();
    v8::Local<v8::Value> input, function, output;
    if (!v8::JSON::Parse(ctx, text(p_json)).ToLocal(&input)) return response("INVALID_INPUT", exception_text(caught));
    if (!module_namespace.Get(isolate)->Get(ctx, text(p_export)).ToLocal(&function)) return response(failure_code(), exception_text(caught));
    if (!function->IsFunction()) return response("NO_EXPORT", "Export is not callable");
    if (!function.As<v8::Function>()->Call(ctx, v8::Undefined(isolate), 1, &input).ToLocal(&output)) return response(failure_code(), exception_text(caught));
    // await 后的续体不是仍在原物理回调中；进入微任务前先回到安全点。
    if (game_binding) game_binding->set_phase("safe_point");
    if (!resolved(output).ToLocal(&output)) return response(failure_code(), exception_text(caught));
    if (output->IsUndefined() || output->IsFunction() || output->IsSymbol()) return response("INVALID_OUTPUT", "Export returned no JSON value");
    v8::Local<v8::String> encoded;
    if (!v8::JSON::Stringify(ctx, output).ToLocal(&encoded)) return response(failure_code(), exception_text(caught));
    // getter/toJSON 也是用户执行；同一预算覆盖它和它排出的微任务。
    isolate->PerformMicrotaskCheckpoint();
    if (isolate->IsExecutionTerminating()) return response(failure_code(), "Output conversion interrupted");
    if (game_binding && !unhandled_rejections.empty()) return response("JS_ERROR", unhandled_game_error());
    if (encoded->Utf8Length(isolate) > JSON_BYTES) return response("INVALID_OUTPUT", "Output limit exceeded");
    Dictionary result = response("OK");
    result["json"] = from_text(encoded);
    return result;
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::invoke_task_in_scope(const String &p_export, const String &p_json) {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto ctx = context.Get(isolate);
    v8::Context::Scope context_scope(ctx);
    v8::TryCatch caught(isolate);
    String reason;
    v8::Local<v8::Value> input, function, output;
    if (!task_bridge->input(ctx, p_json, reason).ToLocal(&input)) return response("INVALID_INPUT", reason);
    if (!module_namespace.Get(isolate)->Get(ctx, text(p_export)).ToLocal(&function)) return response(failure_code(), "Task export lookup failed");
    if (!function->IsFunction()) return response("NO_EXPORT", "Export is not callable");
    if (!function.As<v8::Function>()->Call(ctx, v8::Undefined(isolate), 1, &input).ToLocal(&output)) return response(failure_code(), "Task execution failed");
    return task_result_in_scope(output);
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::task_result_in_scope(v8::Local<v8::Value> p_output) {
    // 只识别原生 Promise，不读取 then/getter，也不经项目 Promise.prototype.then。
    if (p_output->IsPromise()) {
        const auto promise = p_output.As<v8::Promise>();
        promise->MarkAsHandled();
        // MarkAsHandled 不保证触发旧拒绝通知的撤销，返回的 Promise 由本宿主实际观察。
        for (auto entry = unhandled_rejections.begin(); entry != unhandled_rejections.end(); ++entry) {
            if (entry->promise.Get(isolate) == promise) { unhandled_rejections.erase(entry); break; }
        }
    }
    isolate->PerformMicrotaskCheckpoint();
    if (isolate->IsExecutionTerminating()) return response(failure_code(), "Task checkpoint interrupted");
    if (!unhandled_rejections.empty()) return response("JS_ERROR", "Task has unhandled rejection");
    if (p_output->IsPromise()) {
        const auto promise = p_output.As<v8::Promise>();
        if (promise->State() == v8::Promise::kPending) {
            task_promise.Reset(isolate, promise);
            Dictionary result = response("OK");
            result["state"] = "pending";
            return result;
        }
        if (promise->State() == v8::Promise::kRejected) return response("JS_ERROR", "Task Promise rejected");
        p_output = promise->Result();
    }
    String json, reason;
    if (!task_bridge->check(p_output, true, json, reason)) return response("INVALID_OUTPUT", reason);
    if (isolate->IsExecutionTerminating()) return response(failure_code(), "Task data conversion interrupted");
    Dictionary result = response("OK");
    result["state"] = "completed";
    result["json"] = json;
    return result;
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::checkpoint_task_in_scope() {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    v8::Context::Scope context_scope(context.Get(isolate));
    v8::TryCatch caught(isolate);
    return task_result_in_scope(task_promise.Get(isolate));
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::finish_task(Dictionary p_result) {
    end_budget();
    const uint64_t original_generation = generation;
    const uint64_t original_execution = task_execution;
    {
        // 取消先入锁便不能发布完成；终局先入锁则迟到取消只能读到 TASK_ENDED。
        std::lock_guard<std::mutex> lock(execution_mutex);
        if (std::chrono::steady_clock::now() >= task_deadline) timed_out.store(true);
        if (cancelled.load() || timed_out.load() || resource_limited.load() || jobs_limited.load() || queue_limited.load()) {
            p_result = response(failure_code(), "Task budget or cancellation ended this execution");
        }
        const String code = p_result["code"];
        const bool pending = code == "OK" && String(p_result.get("state", "")) == "pending";
        if (!pending) {
            task_active = false;
            task_promise.Reset();
        }
    }
    const String code = p_result["code"];
    if (code != "OK" && code != "INVALID_INPUT" && code != "NO_EXPORT") release_runtime();
    p_result["execution"] = int64_t(original_execution);
    p_result["request_generation"] = int64_t(original_generation);
    p_result["generation"] = int64_t(generation);
    p_result["ready"] = ready;
    return p_result;
}
#endif
Dictionary MetonJSRuntime::finish(Dictionary p_result, bool p_opening) {
    end_budget();
#ifdef METON_JS_GAME_ENABLED
    if (game_faulted) p_result = last_game_fault;
#endif
    if (cancelled.load() || timed_out.load() || resource_limited.load() || jobs_limited.load() || queue_limited.load()) p_result = response(failure_code(), "Execution budget or cancellation ended this generation");
    String code = p_result["code"];
    if (code != "OK" && (p_opening || (code != "INVALID_INPUT" && code != "NO_EXPORT"))) {
        String detail = p_result["detail"];
        release_runtime();
        return response(code, detail);
    }
    if (p_opening && code == "OK") ready = true;
    p_result["ready"] = ready;
    return p_result;
}
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::open_task(const Dictionary &p_sources, const String &p_entry, const String &p_validator, const String &p_validator_digest) {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    if (p_validator.utf8().length() > MetonTaskSupport::SOURCE_CLOSURE_BYTES || p_validator.sha256_text() != p_validator_digest) return response("INVALID_TASK_MATERIAL", "Fixed validator identity mismatch");
    return open_internal(p_sources, p_entry, MetonTaskSupport::MODULE_EVALUATION_MS, std::make_unique<MetonTaskBridge>(p_validator));
}
#endif
Dictionary MetonJSRuntime::open_internal(const Dictionary &p_sources, const String &p_entry, int p_budget_ms, std::unique_ptr<RuntimeBinding> p_binding) {
    if (std::this_thread::get_id() != owner) return wrong_thread();
#ifdef METON_JS_TASK_ENABLED
    if (executing || task_active) return response("EXECUTION_BUSY", "Cannot replace a VM with an active execution");
#endif
#ifdef METON_JS_GAME_ENABLED
    if (executing) return response("EXECUTION_BUSY", "Cannot replace a VM while its native callback is executing");
#endif
    release_runtime();
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        cancelled.store(false);
    }
    resource_limited.store(false);
    queue_limited.store(false);
#ifdef METON_JS_TASK_ENABLED
    const unsigned file_limit = MetonTaskSupport::SOURCE_FILES;
    const uint64_t source_limit = MetonTaskSupport::SOURCE_CLOSURE_BYTES;
    uint64_t total = p_binding->material_bytes();
#endif
#ifdef METON_JS_GAME_ENABLED
    const unsigned file_limit = 32;
    const uint64_t source_limit = 1024 * 1024;
    uint64_t total = 0;
#endif
    if (p_sources.size() > file_limit || !p_sources.has(p_entry) || !valid_name(p_entry)) return response("INVALID_MODULES", "Entry missing or module count/name invalid");
    for (const Variant &key : p_sources.get_key_list()) {
        if (key.get_type() != Variant::STRING || p_sources[key].get_type() != Variant::STRING || !valid_name(key)) return response("INVALID_MODULES", "Module names and sources must be valid strings");
        const unsigned bytes = String(p_sources[key]).utf8().length();
        total += bytes;
        if (total > source_limit) return response("INVALID_MODULES", "Source limit exceeded");
    }
    sources = p_sources.duplicate();
#ifdef METON_JS_GAME_ENABLED
    game_host_entry = p_entry;
#endif
    allocator = std::make_unique<BoundedAllocator>(resource_limited);
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    params.constraints.ConfigureDefaultsFromHeapSize(0, HEAP_BYTES);
    uintptr_t stack = reinterpret_cast<uintptr_t>(&params);
    params.constraints.set_stack_limit(reinterpret_cast<uint32_t *>(stack - 256 * 1024));
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        isolate = v8::Isolate::New(params);
        if (isolate) ++generation;
    }
    if (!isolate) { allocator.reset(); return response("ALLOCATION_FAILED", "Cannot create V8 Isolate"); }
    ++live_isolates;
    isolate->SetData(0, this);
#ifdef METON_JS_GAME_ENABLED
    game_binding = std::move(p_binding);
    game_binding->initialize(isolate, this);
#endif
#ifdef METON_JS_TASK_ENABLED
    task_bridge = std::move(p_binding);
#endif
    game_faulted = false;
    last_game_fault.clear();
    isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    isolate->SetAllowAtomicsWait(false);
#ifdef METON_JS_TASK_ENABLED
    isolate->SetAllowWasmCodeGenerationCallback([](v8::Local<v8::Context>, v8::Local<v8::String>) { return false; });
#endif
    isolate->SetHostImportModuleDynamicallyCallback(import_module);
    isolate->SetPromiseHook(promise_hook);
    isolate->SetPromiseRejectCallback(promise_rejected);
    isolate->AddNearHeapLimitCallback(heap_limit, this);
#ifdef METON_JS_TASK_ENABLED
    begin_budget(p_budget_ms, MetonTaskSupport::MODULE_EVALUATION_MS);
#endif
#ifdef METON_JS_GAME_ENABLED
    begin_budget(p_budget_ms);
#endif
    return finish(open_in_scope(p_entry), true);
}
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::invoke_task(const String &p_export, const String &p_input_json, int64_t p_generation) {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    if (p_generation < 0 || uint64_t(p_generation) != generation) return response("STALE_GENERATION", "This task belongs to an expired runtime");
    if (!task_bridge) return response("TASK_ENDPOINT_REQUIRED", "No isolated task is active");
    if (executing || task_active) return response("EXECUTION_BUSY", "Only one task may execute in this VM");
    if (!ready) return response("CLOSED", "Task runtime is closed");
    if (cancelled.load()) { release_runtime(); return response("CANCELLED", "Cancelled before execution"); }
    if (p_input_json.utf8().length() > MetonTaskSupport::DTO_INPUT_BYTES || p_export.length() > 256) return response("INVALID_INPUT", "Input limit exceeded");
    {
        std::lock_guard<std::mutex> lock(execution_mutex);
        if (task_execution >= INT64_MAX) return response("RESOURCE_LIMIT", "Task execution identity exhausted");
        ++task_execution;
        task_active = true;
        task_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(MetonTaskSupport::TASK_DURATION_MS);
        timed_out.store(false);
        jobs_limited.store(false);
    }
    begin_task_budget();
    return finish_task(invoke_task_in_scope(p_export, p_input_json));
}
#endif
#ifdef METON_JS_TASK_ENABLED
Dictionary MetonJSRuntime::checkpoint_task(int64_t p_generation, int64_t p_execution) {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    if (p_generation < 0 || uint64_t(p_generation) != generation) return response("STALE_GENERATION", "This task belongs to an expired runtime");
    if (p_execution <= 0 || uint64_t(p_execution) != task_execution) return response("STALE_EXECUTION", "This checkpoint belongs to another execution");
    if (executing) return response("EXECUTION_BUSY", "Task checkpoint cannot reenter JavaScript");
    if (!task_active) return response("TASK_ENDED", "This execution has already ended");
    if (cancelled.load() || std::chrono::steady_clock::now() >= task_deadline) return finish_task(response("OK"));
    begin_task_budget();
    return finish_task(checkpoint_task_in_scope());
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::invoke_internal(const String &p_export, const String &p_input_json, int64_t p_generation, int p_budget_ms) {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    if (executing) return response("EXECUTION_BUSY", "Host exports cannot reenter the active VM");
    if (p_generation < 0 || uint64_t(p_generation) != generation) return response("STALE_GENERATION", "This request belongs to an expired runtime");
    if (!ready) return response("CLOSED", "Runtime is closed");
    if (cancelled.load()) { release_runtime(); return response("CANCELLED", "Cancelled before execution"); }
    if (p_input_json.utf8().length() > JSON_BYTES || p_export.length() > 256) return response("INVALID_INPUT", "Input limit exceeded");
    begin_budget(p_budget_ms);
    return finish(invoke_in_scope(p_export, p_input_json), false);
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::open_game(const Dictionary &p_sources, const String &p_entry, const Dictionary &p_identity, const PackedStringArray &p_actions, int p_budget_ms) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!native_memory) native_memory = new MetonNativeMemory(MetonMemorySupport::NATIVE_BYTES);
    MetonNativeMemoryScope native_scope(native_memory);
    if (!MetonGameBinding::identity_matches(p_identity)) return response("BINDING_MISMATCH", "Actual engine/binding/support identity mismatch");
    if (!MetonGameBinding::actions_valid(p_actions)) return response("PERMISSION_DENIED", "Input declaration is not admitted");
    if (p_sources.has("godot") || p_sources.has("godot.annotations") || p_sources.has("@meton/sdk/game/engine") || p_sources.has("meton.internal.world") || p_sources.has("meton.internal.ui")) return response("INVALID_MODULES", "Cannot replace a native module");
    return open_internal(p_sources, p_entry, p_budget_ms, std::make_unique<MetonGameBinding>(p_actions));
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::invoke_game(const String &p_export, const String &p_input_json, int64_t p_generation, const String &p_phase, int p_budget_ms) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    MetonNativeMemoryScope native_scope(native_memory);
    if (p_generation < 0 || uint64_t(p_generation) != generation) return response("STALE_GENERATION", "This game generation has ended");
    if (!game_binding || !ready) return response("CLOSED", "Game runtime is closed");
    if (!game_binding->set_phase(p_phase, true)) return response("PHASE_CONFLICT", "Unknown native invocation phase");
    return invoke_internal(p_export, p_input_json, p_generation, p_budget_ms);
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::get_game_support() const {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    return MetonGameBinding::support();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Array MetonJSRuntime::get_game_objects() const {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread() || !game_binding) return Array();
    return game_binding->native_objects();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::adopt_game_tree(Node *p_root, bool p_owned) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !isolate || !game_binding || game_faulted || executing) return response("CLOSED", "Game runtime is closed");
    v8::Isolate::Scope isolate_scope(isolate); v8::HandleScope handles(isolate);
    v8::Context::Scope context_scope(context.Get(isolate));
    return game_binding->adopt_tree(p_root, p_owned) ? response("OK") : response("PERMISSION_DENIED", "Native tree is not admissible for this game");
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::claim_game_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !game_binding || executing || game_faulted) return response("CLOSED", "Game is not at an ownership handoff boundary");
    return game_binding->claim_fields(p_node, p_fields, p_owner, p_phases) ? response("OK") : response("PERMISSION_DENIED", "Field owner or phase declaration was refused");
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::configure_game_world(Node *p_node) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !game_binding || executing || game_faulted) return response("CLOSED", "Game is not at a world handoff boundary");
    return game_binding->configure_world(p_node) ? response("OK") : response("PERMISSION_DENIED", "World physics ownership was refused");
}
#endif
/** 受信装配在脚本不执行时交接原父节点；之后不允许同代换绑。 */
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::configure_game_ui(Node *p_node) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !game_binding || executing || game_faulted) return response("CLOSED", "Game is not at a UI handoff boundary");
    MetonNativeMemoryScope native_scope(native_memory);
    return game_binding->configure_ui(p_node) ? response("OK") : response("PERMISSION_DENIED", "UI parent ownership was refused");
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::release_game_fields(Node *p_node, const String &p_owner) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !game_binding || executing || game_faulted) return response("CLOSED", "Game is not at an ownership handoff boundary");
    return game_binding->release_fields(p_node, p_owner) ? response("OK") : response("PERMISSION_DENIED", "Field owner release was refused");
}
#endif
#ifdef METON_JS_GAME_ENABLED
void MetonJSRuntime::reject_game_effect(const String &p_detail) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread() || !game_binding || game_faulted) return;
    last_game_fault = response("PERMISSION_DENIED", p_detail);
    last_game_fault["effect"] = "deferred_delete";
    last_game_fault["committed"] = false;
    game_faulted = true; ready = false; game_binding->revoke();
    last_game_fault["ready"] = false;
    queue_game_checkpoint();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::install_game_content(const String &p_json, const String &p_digest, const String &p_version) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    MetonNativeMemoryScope native_scope(native_memory);
    if (!ready || !game_binding || executing) return response("CLOSED", "Game is not at a content admission boundary");
    return game_binding->install_content(p_json, p_digest, p_version) ? response("OK") : response("CONTENT_REJECTED", "Fixed content identity or closure mismatch");
}
#endif
#ifdef METON_JS_GAME_ENABLED
Node *MetonJSRuntime::instantiate_game_scene(const String &p_path) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread() || !ready || !isolate || !game_binding || executing) return nullptr;
    MetonNativeMemoryScope native_scope(native_memory);
    v8::Isolate::Scope isolate_scope(isolate); v8::HandleScope handles(isolate);
    v8::Context::Scope context_scope(context.Get(isolate));
    return game_binding->instantiate_content_scene(p_path);
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::get_game_content() const {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    return game_binding ? game_binding->content_state() : Dictionary();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::drain_game_textures() {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    if (!ready || !game_binding || executing) return response("CLOSED", "Game is not at a GPU cleanup boundary");
    return game_binding->drain_content_textures();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::collect_game_garbage() {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread()) return wrong_thread();
    MetonNativeMemoryScope native_scope(native_memory);
    if (!ready || !isolate || !game_binding || executing) return response("CLOSED", "Game is not at a GC safe point");
    v8::Isolate::Scope isolate_scope(isolate); v8::HandleScope handles(isolate);
    v8::Context::Scope context_scope(context.Get(isolate));
    isolate->LowMemoryNotification();
    game_binding->prune_objects();
    game_binding->prune_resources();
    return response("OK");
}
#endif
#ifdef METON_JS_GAME_ENABLED
void MetonJSRuntime::queue_game_checkpoint() {
    if (checkpoint_queued) return;
    checkpoint_queued = true;
    // MessageQueue 在当前 Godot 通知/信号栈退出后执行，代次防止旧队列作用到重建后的 VM。
    call_deferred("_game_checkpoint", int64_t(generation));
}
#endif
#ifdef METON_JS_GAME_ENABLED
bool MetonJSRuntime::dispatch_game_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count) {
    if (std::this_thread::get_id() != owner || !Thread::is_main_thread() || !ready || !isolate || !game_binding || game_faulted) return false;
    MetonNativeMemoryScope native_scope(native_memory);
    const bool outer = !executing;
    if (outer) begin_budget(MetonLifecycleSupport::CALLBACK_MS);
    bool ok;
    {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handles(isolate);
        auto ctx = context.Get(isolate);
        v8::Context::Scope context_scope(ctx);
        v8::TryCatch caught(isolate);
        ok = p_method == StringName("_meton_signal") ? game_binding->call_signal(p_token, p_args, p_count) :
            p_method == StringName("_meton_validate") ? game_binding->validate_instance(p_token) : game_binding->call_callback(p_token, p_method, p_args, p_count);
        if (!ok || isolate->IsExecutionTerminating()) {
            last_game_fault = response(failure_code(), exception_text(caught));
            last_game_fault["callback"] = String(p_method);
            last_game_fault["token"] = int64_t(p_token);
            game_faulted = true; ready = false; game_binding->revoke();
            last_game_fault["ready"] = false;
        }
    }
    if (outer) end_budget();
    if (cancelled.load() || timed_out.load() || resource_limited.load() || jobs_limited.load() || queue_limited.load()) {
        last_game_fault = response(failure_code(), "Execution budget or cancellation ended this callback");
        last_game_fault["callback"] = String(p_method);
        game_faulted = true; ready = false; game_binding->revoke();
        last_game_fault["ready"] = false;
    }
    queue_game_checkpoint();
    return ok;
}
#endif
#ifdef METON_JS_GAME_ENABLED
void MetonJSRuntime::game_checkpoint(int64_t p_generation) {
    if (std::this_thread::get_id() != owner || p_generation < 0 || uint64_t(p_generation) != generation) return;
    MetonNativeMemoryScope native_scope(native_memory);
    checkpoint_queued = false;
    if (!isolate || !game_binding) return;
    // 不允许在同步信号仍嵌套于调用栈时排空微任务或释放 VM。
    if (executing) { queue_game_checkpoint(); return; }
    if (game_faulted || cancelled.load()) { release_runtime(); return; }
    begin_budget(MetonLifecycleSupport::CALLBACK_MS);
    {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handles(isolate);
        auto ctx = context.Get(isolate);
        v8::Context::Scope context_scope(ctx);
        v8::TryCatch caught(isolate);
        game_binding->set_phase("safe_point");
        game_binding->prune_connections();
        isolate->PerformMicrotaskCheckpoint();
        if (caught.HasCaught() || isolate->IsExecutionTerminating() || !unhandled_rejections.empty()) {
            last_game_fault = response(failure_code(), !isolate->IsExecutionTerminating() && !unhandled_rejections.empty() ? unhandled_game_error() : exception_text(caught));
            last_game_fault["callback"] = "microtasks";
            game_faulted = true; ready = false; game_binding->revoke();
            last_game_fault["ready"] = false;
        }
    }
    end_budget();
    if (cancelled.load() || timed_out.load() || resource_limited.load() || jobs_limited.load() || queue_limited.load()) {
        last_game_fault = response(failure_code(), "Microtask checkpoint budget or cancellation ended this generation");
        last_game_fault["callback"] = "microtasks";
        game_faulted = true; ready = false; game_binding->revoke();
        last_game_fault["ready"] = false;
    }
    if (game_faulted) release_runtime();
}
#endif
#ifdef METON_JS_GAME_ENABLED
void MetonJSRuntime::cleanup_game_scope(uint32_t p_token, bool p_destroyed) {
    MetonNativeMemoryScope native_scope(native_memory);
    if (!isolate || !game_binding) return;
    const bool outer = !executing;
    if (outer) begin_budget(MetonLifecycleSupport::CALLBACK_MS);
    {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handles(isolate);
        auto ctx = context.Get(isolate);
        v8::Context::Scope context_scope(ctx);
        game_binding->cleanup_scope(p_token, p_destroyed);
    }
    if (outer) end_budget();
    queue_game_checkpoint();
}
#endif
#ifdef METON_JS_GAME_ENABLED
Dictionary MetonJSRuntime::get_game_lifecycle() const {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    Dictionary result = game_binding ? game_binding->lifecycle_state() : Dictionary();
    result["last_fault"] = last_game_fault;
    result["checkpoint_queued"] = checkpoint_queued;
    result["live_isolate"] = isolate != nullptr;
    result["generation"] = int64_t(generation);
    return result;
}
#endif
Dictionary MetonJSRuntime::close() {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    if (executing) {
        request_cancel(int64_t(generation));
#ifdef METON_JS_GAME_ENABLED
        if (game_binding) { game_binding->revoke(); queue_game_checkpoint(); }
#endif
        return response("STOP_PENDING", "Release awaits the active native callback stack");
    }
    release_runtime();
    return response("OK");
}
Dictionary MetonJSRuntime::get_info() const {
    if (std::this_thread::get_id() != owner) return wrong_thread();
    Dictionary result = response("OK");
    result["engine"] = "V8";
    if (native_memory) {
        const auto usage = native_memory->state();
        Dictionary memory;
        memory["bytes"] = int64_t(usage.bytes); memory["peak"] = int64_t(usage.peak);
        memory["blocks"] = int64_t(usage.blocks); memory["allocations"] = int64_t(usage.allocations);
        memory["limit"] = int64_t(usage.limit); memory["exhaustion_exit_code"] = 197;
        memory["scope"] = "Godot_allocator_and_builtin_PNG_native_allocations";
        result["native_memory"] = memory;
    }
    result["version"] = v8::V8::GetVersion();
    result["modules"] = sources.size();
    result["platform"] = OS::get_singleton()->get_name();
    result["live_isolates"] = int(live_isolates.load());
#ifdef METON_JS_GAME_ENABLED
    result["game_cleanup"] = game_cleanup.duplicate(true);
#endif
    result["heap_threshold_bytes"] = int64_t(HEAP_BYTES);
    result["array_buffer_limit_bytes"] = int64_t(BUFFER_BYTES);
    result["microtask_limit"] = microtask_limit();
    result["binding_digest"] = METON_GW3_BINDING_DIGEST;
#ifdef METON_JS_GAME_ENABLED
    result["runtime_role"] = "game";
#endif
#ifdef METON_JS_TASK_ENABLED
    result["runtime_role"] = "isolated-task";
#endif
#ifdef METON_JS_TASK_ENABLED
    result["isolated_task"] = task_bridge != nullptr;
    result["task_pending"] = task_active && !task_promise.IsEmpty();
    result["task_execution"] = int64_t(task_execution);
    if (task_bridge) {
        result["task_data_digest"] = MetonTaskSupport::DATA_DIGEST;
        result["task_duration_ms"] = MetonTaskSupport::TASK_DURATION_MS;
        result["task_module_ms"] = MetonTaskSupport::MODULE_EVALUATION_MS;
    }
#endif
    return result;
}
MetonJSRuntime::~MetonJSRuntime() { release_runtime(); if (native_memory) native_memory->release(); }

unsigned MetonJSRuntime::microtask_limit() const {
#ifdef METON_JS_GAME_ENABLED
    return game_binding ? MetonLifecycleSupport::MICROTASKS : 256;
#else
    return MetonTaskSupport::MICROTASKS_PER_CHECKPOINT;
#endif
}
