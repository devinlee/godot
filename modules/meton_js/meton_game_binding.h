#pragma once
#include "core/math/vector2.h"
#include "core/object/object_id.h"
#include "core/io/resource.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
namespace v8 { class TypedArray; }
#include <v8.h>
#include <map>
#include <set>
#include <array>
#include <memory>
#include <vector>

class Node;
class MetonJSRuntime;
class MetonGameCallable;
class MetonGameContent;
class CharacterBody2D;
class MetonGameUi;

/** G04 的最小普通脚本绑定；只接受宿主给出的阶段/输入资格，不提供通用原生反射。 */
class MetonGameBinding {
    friend class MetonJSRuntime;
    friend class MetonGameCallable;
    friend class MetonGameUi;
    friend bool meton_game_predelete_guard(Object *p_object);
    enum Impact : unsigned { DELETE_TREE = 1, POSITION = 2, VELOCITY = 4, VISIBLE = 8, TEXTURE = 16, DELETE_IMMEDIATE = 32 };
    struct FieldOwner { String owner; unsigned phases = 0; };
    std::map<ObjectID, std::map<String, FieldOwner>> field_owners;
    unsigned delayed_delete_rejections = 0;
    std::vector<ObjectID> active_native_objects;
    std::set<ObjectID> deleting_native_roots;
    const char *impact_error(Node *p_node, unsigned p_effects) const;
    bool guard_impact(Node *p_node, unsigned p_effects);
    bool impact_exports(v8::Local<v8::Context> p_context);
    static void delete_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    void clear_delete_requests();
    void forget_impact(ObjectID p_object);
    struct Lifetime { MetonGameBinding *binding; };
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(Lifetime{this});
    struct Stamp { uint32_t owner = 0; uint64_t epoch = 0; };
    struct ObjectEntry {
        ObjectID native;
        Stamp creator;
        bool owned = true;
        bool retired = false;
        v8::Global<v8::Object> wrapper;
        std::array<v8::Global<v8::Function>, 5> callbacks;
        uint64_t epoch = 0;
        uint64_t structure_epoch = 0;
        bool in_tree = false;
        bool closing = false;
        v8::Global<v8::Object> signal;
        v8::Global<v8::Object> scope;
    };
    struct CallableEntry {
        MetonGameBinding *binding = nullptr;
        uint32_t id = 0;
        uint32_t target = 0;
        Callable native;
        v8::Global<v8::Object> wrapper;
        v8::Global<v8::Object> retained;
        Stamp scoped;
    };
    struct Connection {
        Signal signal;
        uint32_t callable = 0;
        Stamp scope;
        v8::Global<v8::Promise::Resolver> waiter;
    };
    std::map<uint32_t, CallableEntry> callables;
    std::map<uint32_t, Connection> connections;
    std::map<int, v8::Global<v8::FunctionTemplate>> handles;
    uint32_t next_callable = 1;
    uint32_t next_connection = 1;
    struct Reaction {
        MetonGameBinding *binding = nullptr;
        uint32_t id = 0;
        v8::Global<v8::Promise> promise;
    };
    std::map<uint32_t, Reaction> reactions;
    std::vector<std::pair<Stamp, uint32_t>> continuation_stack;
    v8::Global<v8::Private> promise_stamp;
    v8::Global<v8::Private> promise_ticket;
    uint32_t next_reaction = 1;
    void stamp_promise(v8::Local<v8::Promise> p_promise, Stamp p_stamp);
    bool promise_event(v8::PromiseHookType p_type, v8::Local<v8::Promise> p_promise, v8::Local<v8::Value> p_parent);
    static void weak_reaction(const v8::WeakCallbackInfo<Reaction> &p_info);
    MetonJSRuntime *runtime = nullptr;
    v8::Isolate *isolate = nullptr;
    std::map<uint32_t, ObjectEntry> objects;
    struct ResourceEntry {
        uint32_t token = 0;
        Ref<Resource> retained;
        v8::Global<v8::Object> wrapper;
        bool retired = false;
    };
    std::unique_ptr<MetonGameContent> content;
    std::map<uint32_t, ResourceEntry> resources;
    std::map<ObjectID, uint32_t> resource_tokens;
    uint32_t next_resource = 1;
    Dictionary resource_cleanup;
    struct SceneView {
        uint32_t root = 0;
        uint64_t epoch = 0;
        uint64_t structure = 0;
        std::vector<ObjectID> nodes;
        v8::Global<v8::Object> wrapper;
        bool retired = false;
    };
    std::map<ObjectID, String> scene_roots;
    std::map<uint32_t, SceneView> scene_views;
    uint32_t next_view = 1;
    static void scene_of(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void scene_get(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void weak_scene(const v8::WeakCallbackInfo<SceneView> &p_info);
    static std::map<ObjectID, MetonGameBinding *> object_owners;
    Array detached_objects;
    unsigned released_nodes = 0;
    unsigned released_borrows = 0;
    std::map<String, v8::Global<v8::FunctionTemplate>> classes;
    v8::Global<v8::FunctionTemplate> vector_template;
    std::set<String> actions;
    uint32_t next_object = 1;
    uint32_t phase = 1;
    unsigned calls = 0;
    bool closed = false;
    bool revoked = false;
    Stamp current;
    uint64_t process_frame = UINT64_MAX;
    uint64_t physics_frame = UINT64_MAX;
    char vector_brand = 0;
    uint32_t world_token = 0;
    std::unique_ptr<MetonGameUi> ui;
    size_t private_ui_objects() const;
    Dictionary ui_state() const;
    static void ui_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static v8::MaybeLocal<v8::Promise> evaluate_ui(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);
    uint64_t world_applied_frame = UINT64_MAX;
    v8::Global<v8::Context> world_context;
    v8::Global<v8::Value> world_record_prototype;
    v8::Global<v8::Value> world_array_prototype;
    CharacterBody2D *world_body() const;
    bool world_physics_phase() const;
    static void inspect_world_value(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void world_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static v8::MaybeLocal<v8::Promise> evaluate_world(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);

    v8::Local<v8::String> text(const String &p_text) const;
    String string(v8::Local<v8::Value> p_value) const;
    void error(const char *p_code, int p_member, const char *p_message) const;
    bool allow(int p_member);
    bool active_stamp() const;
    bool charge_call(int p_member);
    bool capture_callbacks(uint32_t p_token);
    bool validate_instance(uint32_t p_token);
    bool call_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count);
    bool call_signal(uint32_t p_token, const Variant **p_args, int p_count);
    bool callable_valid(uint32_t p_token) const;
    bool scope_valid(Stamp p_stamp, bool p_new_work) const;
    v8::MaybeLocal<v8::Object> handle(int p_kind, uint32_t p_token, uint64_t p_epoch = 0);
    bool unhandle(v8::Local<v8::Value> p_value, int p_kind, uint32_t &r_token, uint64_t &r_epoch);
    v8::MaybeLocal<v8::Object> create_callable(uint32_t p_target, v8::Local<v8::Function> p_method, Stamp p_scope);
    void retire_connection(uint32_t p_connection, const char *p_reason = nullptr);
    void release_capture(uint32_t p_callable);
    void cleanup_scope(uint32_t p_owner, bool p_destroyed);
    void prune_connections();
    bool signal_exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);
    static void handle_construct(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void signal_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void weak_callable(const v8::WeakCallbackInfo<CallableEntry> &p_info);
    static v8::MaybeLocal<v8::Promise> evaluate_helpers(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);
    bool number(v8::Local<v8::Value> p_value, real_t &r_value, int p_member);
    bool vector(v8::Local<v8::Value> p_value, Vector2 &r_value, int p_member);
    v8::MaybeLocal<v8::Object> vector(const Vector2 &p_value);
    Node *node(v8::Local<v8::Object> p_wrapper, int p_member);
    /** 外部树结构不是授权；每次返回与释放仍核对实际对象的登记拥有者。 */
    bool owns(Node *p_node) const;
    uint32_t token_of(Node *p_node) const;
    void release_node(uint32_t p_token);
    void release_created(Stamp p_creator);
    void detach_foreign(Node *p_node);
    bool object_exports(v8::Local<v8::Context> p_context);
    /** 资源包装仅绑定真实加载/实例化结果，不能从 Resource 路径推定所属游戏。 */
    bool resource_exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);
    v8::MaybeLocal<v8::Object> wrap_resource(const Ref<Resource> &p_resource);
    Ref<Resource> unwrap_resource(v8::Local<v8::Value> p_value);
    void prune_resources();
    size_t live_objects() const;
    void prune_objects();
    static void resource_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void weak_resource(const v8::WeakCallbackInfo<ResourceEntry> &p_info);
    static void object_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static MetonGameBinding *self(v8::Isolate *p_isolate);
    static void construct(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void vector_construct(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static void readonly_component(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    static v8::MaybeLocal<v8::Promise> evaluate(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);
    bool exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);

public:
    /** 字段资格由受信装配给出，游戏脚本不能登记、撤回或覆盖其他所有者。 */
    bool claim_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases);
    bool release_fields(Node *p_node, const String &p_owner);
    Dictionary impact_state() const;
    /** 原生结构事件传播到本游戏祖先，改名再改回也不能复活旧视图。 */
    void structure_changed(uint32_t p_token);
    /** 对外返回被编译的支持身份和实际 ClassDB 对照，不能当作全部行为已通过。 */
    static Dictionary support();
    static bool identity_matches(const Dictionary &p_identity);
    static bool actions_valid(const PackedStringArray &p_actions);
    explicit MetonGameBinding(const PackedStringArray &p_actions);
    void initialize(v8::Isolate *p_isolate, MetonJSRuntime *p_runtime);
    v8::Local<v8::Module> module();
    v8::Local<v8::Module> helpers_module();
    /** 仅受信宿主入口可以导入；项目闭包不开放此模块。 */
    v8::Local<v8::Module> world_module();
    v8::Local<v8::Module> ui_module();
    /** 挂树前一次指定原已登记父节点，私有后代不进入普通对象目录。 */
    bool configure_ui(Node *p_node);
    /** 挂树前把一个已归属节点交给原生物理所有者，不能同代换绑。 */
    bool configure_world(Node *p_node);
    /** 只有所属原生宿主能推进阶段；清理/回调完成后的微任务回到安全点。 */
    bool set_phase(const String &p_phase, bool p_new_entry = false);
    Array native_objects() const;
    /** 仅受信宿主可以将没有脚本的原生子树交给本游戏；JS 查找不隐式准入。 */
    bool adopt_tree(Node *p_root, bool p_owned);
    /** G04 内部接入端口；不作为游戏 JS 的可调用服务。 */
    bool install_content(const String &p_json, const String &p_digest, const String &p_version);
    Node *instantiate_content_scene(const String &p_path);
    Dictionary content_state() const;
    Dictionary drain_content_textures();
    /** 只由 ScriptInstance 及原生生命周期接入调用，脚本无法直接改变这些状态。 */
    bool has_callback(uint32_t p_token, const StringName &p_method) const;
    bool dispatch_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count);
    void entered(uint32_t p_token);
    void closing(uint32_t p_token);
    void exited(uint32_t p_token);
    void object_destroyed(uint32_t p_token);
    Dictionary lifecycle_state() const;
    /** 释放记录只交原生宿主，包含拆下后仍由其他拥有者负责的对象。 */
    Dictionary release_state() const;
    void revoke() { revoked = true; }
    void release();
    ~MetonGameBinding();
};
