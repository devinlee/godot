#pragma once
#include "meton_js_role.h"
#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
// V8 头内 friend 的名字查找不能误取 Godot 全局 TypedArray 模板。
namespace v8 { class TypedArray; }
#include <v8.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class MetonGameBinding;
class MetonNativeMemory;
class MetonTaskBridge;
class Node;

/** 共享的 V8 生命周期；构建角色决定游戏绑定或隔离任务控制入口。 */
class MetonJSRuntime : public RefCounted {
    GDCLASS(MetonJSRuntime, RefCounted);
    class BoundedAllocator;
    struct ModuleEntry {
        v8::Global<v8::Module> module;
        v8::Global<v8::Promise> evaluation;
    };
    v8::Isolate *isolate = nullptr;
    std::unique_ptr<BoundedAllocator> allocator;
#ifdef METON_JS_GAME_ENABLED
    using RuntimeBinding = MetonGameBinding;
    std::unique_ptr<MetonGameBinding> game_binding;
    String game_host_entry;
#endif
#ifdef METON_JS_TASK_ENABLED
    using RuntimeBinding = MetonTaskBridge;
    std::unique_ptr<MetonTaskBridge> task_bridge;
    v8::Global<v8::Promise> task_promise;
    std::chrono::steady_clock::time_point task_deadline;
    // 序号在本原生对象寿命内单调增加；取消线程在 execution_mutex 下核对。
    uint64_t task_execution = 0;
    bool task_active = false;
#endif
    MetonNativeMemory *native_memory = nullptr;
    v8::Global<v8::Context> context;
    Dictionary game_cleanup;
    v8::Global<v8::Object> module_namespace;
    std::map<String, ModuleEntry> modules;
    Dictionary sources;
    uint64_t generation = 0;
    bool ready = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> resource_limited{false};
    std::atomic<bool> jobs_limited{false};
    std::atomic<bool> queue_limited{false};
    unsigned job_count = 0;
    unsigned microtask_limit() const;
    const std::thread::id owner = std::this_thread::get_id();
    std::mutex execution_mutex;
    std::condition_variable budget_changed;
    bool executing = false;
    std::thread watchdog;
    bool checkpoint_queued = false;
    bool game_faulted = false;
    Dictionary last_game_fault;
    struct Rejection {
        v8::Global<v8::Promise> promise;
        v8::Global<v8::Value> reason;
    };
    std::vector<Rejection> unhandled_rejections;
    static void promise_rejected(v8::PromiseRejectMessage p_message);
#ifdef METON_JS_GAME_ENABLED
    String unhandled_game_error();
    void queue_game_checkpoint();
    void game_checkpoint(int64_t p_generation);
#endif


    /** 取消线程只在此锁下使用指针，不能与 Isolate 释放交错。 */
    void begin_budget(int p_budget_ms, int p_ceiling_ms = 1000);
    /** 等待/恢复都沿首次调用的绝对截止，不能每次检查点续期。 */
#ifdef METON_JS_TASK_ENABLED
    void begin_task_budget();
#endif
    void end_budget();
    void release_runtime();
    Dictionary response(const String &p_code, const String &p_detail = String()) const;
    Dictionary wrong_thread() const;
    String failure_code() const;
    String exception_text(v8::TryCatch &p_caught);
    v8::Local<v8::String> text(const String &p_value);
    String from_text(v8::Local<v8::Value> p_value);
    static bool valid_name(const String &p_name);
    String resolve_name(const String &p_name, const String &p_parent);
    v8::MaybeLocal<v8::Module> load_module(const String &p_name);
    static v8::MaybeLocal<v8::Module> resolve_module(v8::Local<v8::Context>, v8::Local<v8::String>, v8::Local<v8::FixedArray>, v8::Local<v8::Module>);
    static v8::MaybeLocal<v8::Promise> import_module(v8::Local<v8::Context>, v8::Local<v8::Data>, v8::Local<v8::Value>, v8::Local<v8::String>, v8::Local<v8::FixedArray>);
    static void module_value(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void promise_hook(v8::PromiseHookType, v8::Local<v8::Promise>, v8::Local<v8::Value>);
    static size_t heap_limit(void *p_data, size_t p_current, size_t p_initial);
    v8::MaybeLocal<v8::Value> resolved(v8::Local<v8::Value> p_value);
    Dictionary open_in_scope(const String &p_entry);
#ifdef METON_JS_GAME_ENABLED
    Dictionary invoke_in_scope(const String &p_export, const String &p_json);
#endif
#ifdef METON_JS_TASK_ENABLED
    Dictionary invoke_task_in_scope(const String &p_export, const String &p_json);
    Dictionary task_result_in_scope(v8::Local<v8::Value> p_output);
    Dictionary checkpoint_task_in_scope();
    /** 取消与终局在同一锁下排序，先退出全部 V8 局部句柄后再释放。 */
    Dictionary finish_task(Dictionary p_result);
#endif
    Dictionary open_internal(const Dictionary &p_sources, const String &p_entry, int p_budget_ms, std::unique_ptr<RuntimeBinding> p_binding);
#ifdef METON_JS_GAME_ENABLED
    Dictionary invoke_internal(const String &p_export, const String &p_input_json, int64_t p_generation, int p_budget_ms);
#endif
    /** 先退出全部 V8 局部句柄/上下文，再根据故障结局销毁实例。 */
    Dictionary finish(Dictionary p_result, bool p_opening);
protected:
    static void _bind_methods();
public:
    /** 分配器类型在实现文件完整定义后再生成构造与销毁路径。 */
    MetonJSRuntime();
    /** 仅由 Godot 模块初始化/卸载调用。 */
    static void initialize_engine();
    static void shutdown_engine();
#ifdef METON_JS_TASK_ENABLED
    /** 受信装配传入固定校验材料；任务脚本不能调用此控制入口或替换预算。 */
    Dictionary open_task(const Dictionary &p_sources, const String &p_entry, const String &p_validator, const String &p_validator_digest);
    /** 内部纯计算通道，严格 DTO 与 schema 双向检查。 */
    Dictionary invoke_task(const String &p_export, const String &p_input_json, int64_t p_generation);
    /** 仅由受信宿主驱动；不调用项目导出，不创建新执行或续期。 */
    Dictionary checkpoint_task(int64_t p_generation, int64_t p_execution);
    /** 按原执行取消；迟到取消不能影响同一 VM 后续执行。可跨线程调用。 */
    Dictionary request_task_cancel(int64_t p_generation, int64_t p_execution);
#endif
#ifdef METON_JS_GAME_ENABLED
    /** 目标绑定身份由受信装配提供；游戏源码不能覆盖内建 godot 模块或推进原生阶段。 */
    Dictionary open_game(const Dictionary &p_sources, const String &p_entry, const Dictionary &p_identity, const PackedStringArray &p_actions, int p_budget_ms = 500);
    Dictionary invoke_game(const String &p_export, const String &p_input_json, int64_t p_generation, const String &p_phase, int p_budget_ms = 100);
    Dictionary get_game_support() const;
    /** 只给原生运行装配接入真正场景树，JS 模块无法取得此控制入口。 */
    Array get_game_objects() const;
    /** 原生宿主明确交接树的可达范围与释放权；不向游戏 JS 暴露。 */
    Dictionary adopt_game_tree(Node *p_root, bool p_owned = true);
    /** 字段登记仅供原生所有者装配，不导出到游戏模块。 */
    Dictionary claim_game_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases);
    Dictionary release_game_fields(Node *p_node, const String &p_owner);
    /** 私有 R08 交接，不向普通脚本暴露安装或受管字段写入。 */
    Dictionary configure_game_world(Node *p_node);
    Dictionary configure_game_ui(Node *p_node);
    /** 原生延迟副作用被最终复验拒绝后，先封锁再安排退栈收尾。 */
    void reject_game_effect(const String &p_detail);
    /** 固定内容摘要及版本由受信装配提供，游戏 JS 无此控制入口。 */
    Dictionary install_game_content(const String &p_json, const String &p_digest, const String &p_version);
    Node *instantiate_game_scene(const String &p_path);
    Dictionary get_game_content() const;
    /** 原生局部验证/内存压力安全点；不让游戏请求 GC 作为授权或释放保证。 */
    Dictionary collect_game_garbage();
    /** 受信宿主在安全点等待 GPU 退役完成，不向游戏模块开放。 */
    Dictionary drain_game_textures();
    Dictionary get_game_lifecycle() const;
    /** 引擎回调与嵌套信号共用外层预算；VM 释放只能在原生回调退栈后进行。 */
    bool dispatch_game_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count);
    void cleanup_game_scope(uint32_t p_token, bool p_destroyed);
#endif
    /** 按代取消可跨线程调用；先封锁结果，再请求 V8 中断。 */
    Dictionary request_cancel(int64_t p_generation = -1);
    /** 幂等关闭，旧代次随实际实例释放失效。 */
    Dictionary close();
    Dictionary get_info() const;
    ~MetonJSRuntime();
};
