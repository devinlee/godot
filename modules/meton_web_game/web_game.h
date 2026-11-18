#pragma once
#include "scene/main/node.h"
#include "core/variant/dictionary.h"
#include "core/io/image.h"
#include "meton_native_memory.h"
#include "meton_game_content.h"
#include <map>
#include <vector>
#include <set>
#include <memory>

class CharacterBody2D;
class MetonGameUi;

/** 单个 Wasm 引擎拥有一个内部执行单元；外部 renderer 宿主才有停止权威。 */
class MetonWebGame : public Node {
    GDCLASS(MetonWebGame, Node);
public:
    struct Stamp { uint32_t owner = 0; uint64_t epoch = 0; };
    struct Entry {
        ObjectID object;
        unsigned callbacks = 0;
        uint32_t scope = 0;
        uint64_t epoch = 0, structure = 0;
        bool active = false, closing = false, owned = true, retired = false, javascript = false;
        Stamp creator;
    };
    static MetonWebGame *current;
    std::map<uint32_t, Entry> objects;
    struct CallableEntry { Callable native; uint32_t target; Stamp scope; };
    std::map<uint32_t, CallableEntry> callables;
    uint32_t phase = 0;
    bool revoked = false;
private:
    friend class MetonGameUi;
    std::unique_ptr<MetonGameUi> ui;
    bool attach_ui_port();
    size_t private_ui_objects() const;
    Dictionary ui_state() const;
    friend bool meton_web_predelete_guard(Object *p_object);
    enum Effect { POSITION = 1, VELOCITY = 2, VISIBLE = 4, TEXTURE = 8, DELETE_TREE = 16, DELETE_IMMEDIATE = 32 };
    struct FieldOwner { String owner; unsigned phases; };
    std::map<ObjectID, std::map<String, FieldOwner>> field_owners;
    uint32_t world_token = 0;
    bool world_stepping = false;
    uint64_t world_step_frame = UINT64_MAX, world_applied_frame = UINT64_MAX;
    CharacterBody2D *world_body(bool p_active = true) const;
    bool world_physics_phase() const;
    bool attach_world_port();
    std::set<ObjectID> deleting_native_roots;
    unsigned delayed_delete_rejections = 0;
    const char *impact_error(Node *p_node, unsigned p_effects) const;
    Dictionary impact_request(const Dictionary &p_request);
    void clear_delete_requests();
    void forget_impact(ObjectID p_object);
    Dictionary impact_state() const;
    uint32_t next_object = 1, next_callable = 1;
    uint32_t next_scope = 1, next_connection = 1;
    unsigned execution_depth = 0;
    struct Connection { ObjectID source; uint32_t callable; Stamp scope; bool waiter = false; };
    std::map<uint32_t, Stamp> scopes;
    std::map<uint32_t, Connection> connections;
    Stamp execution_stamp;
    struct Continuation { Stamp stamp; bool module = false; };
    std::map<uint32_t, Continuation> continuations;
    uint32_t next_continuation = 1;
    bool async_checkpoint = false;
    std::map<ObjectID, uint32_t> object_tokens;
    std::vector<ObjectID> active_native_objects;
    bool tearing_down = false;
    struct SceneView { uint32_t root; uint64_t epoch, structure; std::vector<ObjectID> nodes; };
    std::map<uint32_t, SceneView> scene_views;
    std::map<ObjectID, String> scene_roots;
    uint32_t next_view = 1;
    uint64_t process_frame = 0, physics_frame = 0;
    int calls = 0;
    Dictionary last_fault;
    MetonNativeMemory *native_memory;
    Ref<Image> probe_image;
    MetonGameContent content;
    struct ResourceEntry { Ref<Resource> value; uint32_t lease = 0; };
    std::map<uint32_t, ResourceEntry> resources;
    std::map<ObjectID, uint32_t> resource_tokens;
    uint32_t next_resource = 1;
    Dictionary wrap_resource(const Ref<Resource> &p_resource);
    Dictionary resource_request(const Dictionary &p_request);
    /** 只返回已登记的真实原生对象；查询不会取得新的拥有权。 */
    Dictionary wrap_node(Node *p_node);
    Dictionary object_request(const Dictionary &p_request);
    Dictionary scene_request(const Dictionary &p_request);
    uint32_t token_of(Node *p_node) const;
    size_t live_objects() const;
    void prune_objects();
    bool adopt_tree(Node *p_root, bool p_owned);
    void detach_foreign(Node *p_node);
    void release_node(uint32_t p_token);
    void release_created(Stamp p_creator);
    void release_objects();
    bool active_stamp() const;
    bool scope_valid(Stamp p_stamp, bool p_new_work) const;
    uint32_t create_callable(uint32_t p_target, Stamp p_scope);
    void handle_event(uint32_t p_token, const char *p_event);
    void release_capture(uint32_t p_callable);
    void retire_connection(uint32_t p_connection, const char *p_reason = "cancelled");
    void prune_connections();
    void cleanup_scope(uint32_t p_owner, bool p_destroyed);
    Dictionary signal_request(const Dictionary &p_request);
    Dictionary async_request(const Dictionary &p_request);
    Dictionary capture_continuation(Stamp p_stamp, bool p_module);
    void checkpoint();
protected:
    static void _bind_methods();
    void _notification(int p_notification);
public:
    MetonWebGame();
    ~MetonWebGame();
    Dictionary boot();
    /** 私有准备不挂树，原生字段交接之后才能显式激活。 */
    Node *prepare_world();
    bool configure_world(Node *p_node);
    /** 仅受信原生装配交接私有 UI 父节点；普通 JS 不取得控件。 */
    bool configure_ui(Node *p_node);
    /** 固定私有值 ABI 与公开 Godot 支持目录分离。 */
    Dictionary ui_call(int p_operation, const Array &p_arguments);
    Dictionary activate_world();
    Dictionary world_step();
    /** 仅私有 Wasm ABI 使用，不进入公共 request 或普通脚本声明。 */
    Dictionary world_call(int p_operation, double p_x, double p_y);
    Dictionary inspect();
    /** 仅受信场景夹具使用；不进入普通脚本的支持目录。 */
    Dictionary memory_probe(const String &p_name);
    Ref<Image> take_probe_image();
    /** 内容只有受信装配可以安装，普通 JS 不能提供字节、路径重映射或内容身份。 */
    Dictionary install_content(const String &p_json, const String &p_digest, const String &p_version);
    Dictionary resource_probe(const String &p_name);
    /** 固定场景只能由受信装配实例化，不成为普通脚本的任意加载入口。 */
    Node *instantiate_content_scene(const String &p_name);
    bool adopt_node_tree(Node *p_root, bool p_owned);
    /** 字段规则仅由原生受信装配登记；普通脚本不导出这些入口。 */
    bool claim_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases);
    bool release_fields(Node *p_node, const String &p_owner);
    Dictionary object_probe(const String &p_name);
    Dictionary control(const String &p_name);
    Dictionary request(const Dictionary &p_request);
    Dictionary failure(const String &p_code) const;
    bool has_callback(uint32_t p_token, const StringName &p_method) const;
    bool dispatch_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count);
    void entered(uint32_t p_token);
    void closing(uint32_t p_token);
    void exited(uint32_t p_token);
    void object_destroyed(uint32_t p_token);
    void structure_changed(uint32_t p_token);
    bool callable_valid(uint32_t p_token) const;
    void signal(uint32_t p_token);
};
