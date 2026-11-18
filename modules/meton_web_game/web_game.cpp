#include "web_game.h"
#include "web_ui.h"
#include "web_script.h"
#include "web_support.generated.h"
#include "web_identity.generated.h"
#include "web_memory.generated.h"
#include "meton_game_resources.generated.h"
#include "meton_game_objects.generated.h"
#include "meton_game_impact.generated.h"
#include "core/config/engine.h"
#include "core/input/input.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/2d/sprite_2d.h"
#include <emscripten.h>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace MetonGameSupport;
MetonWebGame *MetonWebGame::current = nullptr;

/** Wasm trap 不是进程退出；先封锁绑定，再由独占 renderer 外的宿主确认终止。 */
[[noreturn]] void meton_web_resource_exhausted() {
    if (MetonWebGame::current) MetonWebGame::current->revoked = true;
    // trap 不执行 C++ 析构；致命封锁后的诊断不再沿未退栈的动态分配作用域。
    MetonNativeMemoryScope terminal_scope(nullptr);
    EM_ASM({ globalThis.metonC2NativeBudgetFault(); });
    __builtin_trap();
}
namespace {
const char *callbacks[] = {"_enter_tree", "_ready", "_process", "_physics_process", "_exit_tree"};
const uint32_t callback_phases[] = {P_ENTER_TREE, P_READY, P_PROCESS, P_PHYSICS, P_CLEANUP};
Dictionary ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
bool numeric(const Variant &p_value) { return (p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT) && std::isfinite(double(p_value)); }
bool vector_of(const Variant &p_value, Vector2 &r_vector) {
    if (p_value.get_type() != Variant::ARRAY) return false;
    Array array = p_value;
    if (array.size() != 2 || !numeric(array[0]) || !numeric(array[1]) || std::abs(double(array[0])) > std::numeric_limits<real_t>::max() || std::abs(double(array[1])) > std::numeric_limits<real_t>::max()) return false;
    r_vector = Vector2(double(array[0]), double(array[1])); return true;
}
Array vector_array(const Vector2 &p_vector) { Array value; value.push_back(p_vector.x); value.push_back(p_vector.y); return value; }
}

// 同一 Emscripten 实例中的 ABI 调用不排队、不返回 Promise；异常不得穿越原生析构栈。
EM_JS(void, meton_web_attach, (), {
    globalThis.metonC1NativeAttach({
        request: function(value) {
            const input = stringToNewUTF8(JSON.stringify(value));
            try { return JSON.parse(UTF8ToString(_meton_web_request(input))); }
            finally { _free(input); }
        },
        memory: function(probe) {
            const before = wasmMemory.buffer.byteLength;
            if (!probe) return {bytes: before};
            let refused = false;
            try { wasmMemory.grow(4097); } catch (error) { refused = error instanceof RangeError; }
            return {before: before, after: wasmMemory.buffer.byteLength, refused: refused};
        }
    });
});
EM_JS(int, meton_web_boot, (), {
    try { return globalThis.metonC1Dispatch('boot', 0, "", 0); }
    catch (error) { globalThis.metonC1Fault(error); return 0; }
});
EM_JS(int, meton_web_callback, (int token, const char *method, double value), {
    try { globalThis.metonC1Dispatch('callback', token, UTF8ToString(method), value); return 1; }
    catch (error) { globalThis.metonC1Fault(error); return 0; }
});
EM_JS(char *, meton_web_control, (const char *method), {
    try { return stringToNewUTF8(JSON.stringify(globalThis.metonC1Dispatch('control', 0, UTF8ToString(method), 0))); }
    catch (error) { globalThis.metonC1Fault(error); return stringToNewUTF8('{"fault":true}'); }
});

extern "C" EMSCRIPTEN_KEEPALIVE const char *meton_web_request(const char *p_json) {
    static CharString result;
    Ref<JSON> json; json.instantiate();
    Dictionary answer;
    if (!p_json || strlen(p_json) > 65536 || json->parse(String::utf8(p_json)) != OK || json->get_data().get_type() != Variant::DICTIONARY || !MetonWebGame::current) {
        answer["ok"] = false; answer["code"] = "invalid_argument";
    } else answer = MetonWebGame::current->request(json->get_data());
    result = JSON::stringify(answer).utf8();
    return result.get_data();
}

/** 原生 Callable 使用目标 ObjectID；节点销毁后不能因 JS 包装仍存在而复活。 */
class MetonWebCallable : public CallableCustom {
    ObjectID runtime, target;
    uint32_t token;
    static bool equal(const CallableCustom *a, const CallableCustom *b) {
        auto left = static_cast<const MetonWebCallable *>(a), right = static_cast<const MetonWebCallable *>(b);
        return left->runtime == right->runtime && left->token == right->token;
    }
    static bool less(const CallableCustom *a, const CallableCustom *b) {
        auto left = static_cast<const MetonWebCallable *>(a), right = static_cast<const MetonWebCallable *>(b);
        return left->runtime == right->runtime ? left->token < right->token : left->runtime < right->runtime;
    }
public:
    MetonWebCallable(ObjectID p_runtime, ObjectID p_target, uint32_t p_token) : runtime(p_runtime), target(p_target), token(p_token) {}
    uint32_t hash() const override { return uint32_t(uint64_t(runtime)) ^ token; }
    String get_as_text() const override { return "MetonWebCallable"; }
    CompareEqualFunc get_compare_equal_func() const override { return equal; }
    CompareLessFunc get_compare_less_func() const override { return less; }
    ObjectID get_object() const override { return target; }
    int get_argument_count(bool &r_valid) const override { r_valid = true; return 0; }
    bool is_valid() const override {
        auto *game = Object::cast_to<MetonWebGame>(ObjectDB::get_instance(runtime));
        return game && game->callable_valid(token) && ObjectDB::get_instance(target);
    }
    void call(const Variant **p_args, int p_count, Variant &r_result, Callable::CallError &r_error) const override {
        r_error.error = Callable::CallError::CALL_OK;
        auto *game = Object::cast_to<MetonWebGame>(ObjectDB::get_instance(runtime));
        if (game && is_valid() && p_count == 0) game->signal(token);
    }
};

void MetonWebGame::_bind_methods() {
    ClassDB::bind_method(D_METHOD("prepare_world"), &MetonWebGame::prepare_world);
    ClassDB::bind_method(D_METHOD("configure_world", "node"), &MetonWebGame::configure_world);
    ClassDB::bind_method(D_METHOD("configure_ui", "node"), &MetonWebGame::configure_ui);
    ClassDB::bind_method(D_METHOD("activate_world"), &MetonWebGame::activate_world);
    ClassDB::bind_method(D_METHOD("world_step"), &MetonWebGame::world_step);
    ClassDB::bind_method(D_METHOD("boot"), &MetonWebGame::boot);
    ClassDB::bind_method(D_METHOD("inspect"), &MetonWebGame::inspect);
    ClassDB::bind_method(D_METHOD("control", "name"), &MetonWebGame::control);
    ClassDB::bind_method(D_METHOD("memory_probe", "name"), &MetonWebGame::memory_probe);
    ClassDB::bind_method(D_METHOD("take_probe_image"), &MetonWebGame::take_probe_image);
    ClassDB::bind_method(D_METHOD("install_content", "json", "digest", "version"), &MetonWebGame::install_content);
    ClassDB::bind_method(D_METHOD("resource_probe", "name"), &MetonWebGame::resource_probe);
    ClassDB::bind_method(D_METHOD("object_probe", "name"), &MetonWebGame::object_probe);
    ClassDB::bind_method(D_METHOD("instantiate_content_scene", "path"), &MetonWebGame::instantiate_content_scene);
    ClassDB::bind_method(D_METHOD("adopt_node_tree", "root", "owned"), &MetonWebGame::adopt_node_tree);
    ClassDB::bind_method(D_METHOD("claim_fields", "node", "fields", "owner", "phases"), &MetonWebGame::claim_fields);
    ClassDB::bind_method(D_METHOD("release_fields", "node", "owner"), &MetonWebGame::release_fields);
}
MetonWebGame::MetonWebGame() : native_memory(new MetonNativeMemory(MetonWebMemorySupport::NATIVE_BYTES)) {
    ERR_FAIL_COND(current); current = this; phase = P_MODULE;
    // 所有角色离树后仍要归还已排队的弱保有，不能依赖下一次用户回调或新建对象触发。
    set_process(true);
}
/** PREDELETE 先于 Node 递归删子节点，借用树必须在这个边界交还，不能等 C++ 析构。 */
void MetonWebGame::_notification(int p_notification) {
    if (p_notification == NOTIFICATION_PREDELETE) release_objects();
    else if (p_notification == NOTIFICATION_PROCESS) checkpoint();
}
void MetonWebGame::release_objects() {
    if (tearing_down) return;
    revoked = true; tearing_down = true;
    // 原普通父节点释放前先封锁私有输入并释放本方控件。
    if (ui) ui->close_all();
    clear_delete_requests();
    // 原生析构递归产生通知，因此遍历句柄快照，不让清理修改正在遍历的容器。
    std::vector<uint32_t> snapshot;
    for (const auto &entry : objects) if (!entry.second.retired) snapshot.push_back(entry.first);
    for (uint32_t id : snapshot) {
        auto found = objects.find(id);
        if (found == objects.end() || found->second.retired) continue;
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(found->second.object));
        if (node && (!found->second.owned || !node->get_parent() || !token_of(node->get_parent()))) release_node(id);
    }
    // 借用子树或后挂树对象仍可能活着，只拆本方 ScriptInstance，不留下悬空 binding。
    for (uint32_t id : snapshot) {
        auto found = objects.find(id);
        if (found == objects.end() || found->second.retired) continue;
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(found->second.object));
        if (node) node->set_script_instance(nullptr);
    }
    resources.clear(); resource_tokens.clear(); scene_views.clear(); callables.clear(); connections.clear(); scopes.clear(); continuations.clear();
    field_owners.clear(); deleting_native_roots.clear();
}
MetonWebGame::~MetonWebGame() {
    release_objects();
    if (current == this) current = nullptr;
    probe_image.unref();
    ui.reset();
    native_memory->release();
}
Dictionary MetonWebGame::failure(const String &p_code) const { Dictionary result; result["ok"] = false; result["code"] = p_code; return result; }
Dictionary MetonWebGame::boot() {
    MetonNativeMemoryScope memory_scope(native_memory);
    if (!objects.empty() || revoked) return failure("scope_ended");
    meton_web_attach();
    phase = P_CONSTRUCT;
    ++execution_depth;
    uint32_t actor = meton_web_boot();
    --execution_depth;
    phase = P_SAFE_POINT;
    if (!actor || !objects.count(actor)) { revoked = true; return failure("dependency_failed"); }
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(objects.at(actor).object));
    node->set_name("Actor"); add_child(node);
    return inspect();
}

/** 复用原 boot 的创建上下文；世界候选须先交接字段，不能在准备时提前进入树。 */
Node *MetonWebGame::prepare_world() {
    MetonNativeMemoryScope memory_scope(native_memory);
    if (!objects.empty() || revoked || world_token) return nullptr;
    meton_web_attach();
    if (!attach_world_port() || !attach_ui_port()) { revoked = true; return nullptr; }
    phase = P_CONSTRUCT;
    ++execution_depth;
    const uint32_t actor = meton_web_boot();
    --execution_depth;
    phase = P_SAFE_POINT;
    if (!actor || !objects.count(actor) || revoked) { revoked = true; return nullptr; }
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(objects.at(actor).object));
    if (!node) { revoked = true; return nullptr; }
    node->set_name("Actor");
    return node;
}
Dictionary MetonWebGame::inspect() {
    Dictionary result;
    result["binding_digest"] = METON_WEB_BINDING_DIGEST;
    result["godot_revision"] = GODOT_REVISION;
    result["precision_bytes"] = int(sizeof(real_t));
    result["revoked"] = revoked;
    result["impact"] = impact_state(); result["last_fault"] = last_fault;
    const auto memory = native_memory->state();
    Dictionary allocation;
    allocation["bytes"] = int64_t(memory.bytes); allocation["peak"] = int64_t(memory.peak);
    allocation["blocks"] = int64_t(memory.blocks); allocation["allocations"] = int64_t(memory.allocations);
    allocation["limit"] = int64_t(memory.limit); allocation["exhaustion"] = "revoke_wasm_then_external_renderer_stop";
    result["native_memory"] = allocation;
    result["content"] = content.state();
    result["resource_wrappers"] = int(resources.size());
    result["callables"] = int(callables.size()); result["connections"] = int(connections.size()); result["scopes"] = int(scopes.size());
    result["continuations"] = int(continuations.size());
    result["live_objects"] = int(live_objects()); result["object_records"] = int(objects.size()); result["scene_views"] = int(scene_views.size());
    result["private_ui"] = ui_state();
    result["native_slots"] = int(live_objects() + private_ui_objects());
    result["process_frames"] = int64_t(Engine::get_singleton()->get_process_frames());
    result["physics_frames"] = int64_t(Engine::get_singleton()->get_physics_frames());
    Array members; bool matched = true;
    for (const auto &entry : MEMBERS) {
        bool valid = true;
        if (String(entry.method) != String()) {
            auto *method = ClassDB::get_method(entry.owner, entry.method);
            valid = method && method->get_argument_count() == entry.arguments && method->get_argument_type(-1) == entry.result;
        }
        Dictionary item; item["id"] = entry.id; item["matched"] = valid; members.push_back(item); matched &= valid;
    }
    const char *resource_owners[] = {"Sprite2D", "Sprite2D", "ResourceLoader"};
    const char *resource_methods[] = {"get_texture", "set_texture", "load"};
    const int resource_arguments[] = {0, 1, 3};
    for (int i = 0; i < 3; ++i) {
        auto *method = ClassDB::get_method(resource_owners[i], resource_methods[i]);
        const bool valid = method && method->get_argument_count() == resource_arguments[i] &&
                method->get_argument_type(-1) == (i == 1 ? Variant::NIL : Variant::OBJECT);
        Dictionary item; item["id"] = MetonResourceSupport::MEMBERS[i].id; item["matched"] = valid;
        members.push_back(item); matched &= valid;
    }
    for (const auto &entry : MetonObjectSupport::MEMBERS) {
        auto *method = ClassDB::get_method("Node", entry.name);
        const bool valid = method && method->get_argument_count() == 1 && method->get_argument_type(0) == Variant::NODE_PATH && method->get_argument_type(-1) == Variant::OBJECT;
        Dictionary item; item["id"] = entry.id; item["matched"] = valid; members.push_back(item); matched &= valid;
    }
    for (int i = 0; i < 2; ++i) {
        bool valid = false;
        if (i == 0) {
            auto *method = ClassDB::get_method("Node", "queue_free");
            valid = method && method->get_argument_count() == 0 && method->get_argument_type(-1) == Variant::NIL;
        } else {
            Object probe; bool has_count = false;
            valid = probe.has_method("free") && probe.get_method_argument_count("free", &has_count) == 0 && has_count;
        }
        Dictionary item; item["id"] = MetonImpactSupport::MEMBERS[i].id; item["matched"] = valid; members.push_back(item); matched &= valid;
    }
    result["classdb_matched"] = matched; result["members"] = members;
    return result;
}
Dictionary MetonWebGame::control(const String &p_name) {
    // 仅受信浏览器装配可进入；重复通知也必须符合真实上下文故障，不接受脚本伪造状态。
    if (p_name == "gpu_context_lost" && !content.gpu_healthy()) {
        revoked = true; last_fault["code"] = "dependency_failed"; last_fault["operation"] = "gpu_retirement"; last_fault["committed"] = false;
        return inspect();
    }
    if (revoked) return failure("scope_ended");
    // 帧外观察也是安全点；没有新属性调用的游戏仍须归还退休记录占用。
    prune_objects(); prune_connections();
    Node *actor = objects.count(1) ? Object::cast_to<Node>(ObjectDB::get_instance(objects.at(1).object)) : nullptr;
    phase = P_SAFE_POINT;
    if (p_name == "exit" && actor && actor->get_parent()) remove_child(actor);
    else if (p_name == "enter" && actor && !actor->get_parent()) add_child(actor);
    else if (p_name == "request_ready" && actor) actor->request_ready();
    else if (p_name == "emit" && actor) actor->emit_signal("tree_exiting");
    else if (p_name == "free" && actor) memdelete(actor);
    else if (p_name == "native_block") { volatile unsigned value = 0; for (;;) value = value + 1; }
    ++execution_depth;
    char *json = meton_web_control(p_name.utf8().get_data());
    --execution_depth;
    prune_connections();
    Variant value = JSON::parse_string(String::utf8(json)); free(json);
    Dictionary result = inspect(); result["script"] = value;
    // 同步用户代码可在回调中销毁对象；控制返回前必须重新按 ObjectID 查询。
    actor = objects.count(1) ? Object::cast_to<Node>(ObjectDB::get_instance(objects.at(1).object)) : nullptr;
    if (actor) result["position"] = vector_array(Object::cast_to<Node2D>(actor)->get_position());
    return result;
}

Dictionary MetonWebGame::request(const Dictionary &request) {
    if (revoked) return failure("scope_ended");
    MetonNativeMemoryScope memory_scope(native_memory);
    const String op = request.get("op", "");
    if (op.begins_with("async_")) return async_request(request);
    // GC 通知只归还已有保有，不取得场景调用资格；其余请求必须来自真实同步入口。
    if (op == "callable_release") return signal_request(request);
    if (op != "resource_release" && op != "scene_release" && (!execution_depth || !active_stamp())) return failure("scope_ended");
    auto *engine = Engine::get_singleton();
    if (process_frame != engine->get_process_frames() || physics_frame != engine->get_physics_frames()) {
        process_frame = engine->get_process_frames(); physics_frame = engine->get_physics_frames(); calls = 0;
    }
    if (++calls > MAX_CALLS_PER_ENTRY) return failure("budget_exceeded");
    prune_objects(); prune_connections();
    if (op == "free" || op == "queue_free") return impact_request(request);
    if (op == "signal_get" || op == "scope_of" || op == "callable" || op == "connect" || op == "disconnect" || op == "scope_connect" || op == "scope_wait" || op == "subscription_disconnect") return signal_request(request);
    if (op == "get_node" || op == "get_node_or_null") return object_request(request);
    if (op.begins_with("scene_")) return scene_request(request);
    if (op.begins_with("resource_") || op == "texture_get" || op == "texture_set") return resource_request(request);
    if (op == "new") {
        if (!(phase & (P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT))) return failure("phase_conflict");
        if (live_objects() + private_ui_objects() >= MAX_OBJECTS || !next_object) return failure("budget_exceeded");
        String type = request.get("type", "");
        if (type != "Node2D" && type != "Sprite2D" && type != "CharacterBody2D") return failure("unsupported_member");
        auto *node = Object::cast_to<Node>(ClassDB::instantiate(type));
        const uint32_t token = next_object++;
        Entry entry; entry.object = node->get_instance_id(); entry.creator = execution_stamp;
        objects[token] = entry; object_tokens[entry.object] = token; return ok(token);
    }
    if (op == "input") {
        if (!(phase & (P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT))) return failure("phase_conflict");
        String left = request.get("left", ""), right = request.get("right", "");
        if ((left != "left" && left != "right") || (right != "left" && right != "right")) return failure("permission_denied");
        return ok(Input::get_singleton()->get_axis(left, right));
    }
    uint32_t token = int64_t(request.get("token", 0));
    if (!objects.count(token) || objects.at(token).retired) return failure("object_invalid");
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(objects.at(token).object));
    if (!node || node->is_queued_for_deletion()) return failure("object_invalid");
    if (op == "bind") {
        if (node->get_script_instance()) return failure("invalid_argument");
        unsigned mask = int64_t(request.get("mask", 0));
        if (mask > 31) return failure("invalid_argument");
        objects[token].callbacks = mask; objects[token].javascript = true;
        node->set_script_instance(memnew(MetonWebScriptInstance(this, token, node)));
        return ok();
    }
    String member = request.get("member", "");
    int id = -1;
    for (int i = 0; i < MEMBER_COUNT; ++i) if (member == MEMBERS[i].id) id = i;
    if (id < 0) return failure("unsupported_member");
    if (!(MEMBERS[id].phases & phase)) return failure("phase_conflict");
    if (!node->is_class(MEMBERS[id].owner)) return failure("invalid_argument");
    auto *item = Object::cast_to<CanvasItem>(node);
    auto *node2d = Object::cast_to<Node2D>(node);
    auto *body = Object::cast_to<CharacterBody2D>(node);
    Vector2 value;
    switch (id) {
        case M_CANVASITEM_VISIBLE_GET: return ok(item->is_visible());
        case M_CANVASITEM_VISIBLE_SET:
            if (request.get("value", Variant()).get_type() != Variant::BOOL) return failure("invalid_argument");
            if (const char *reason = impact_error(node, VISIBLE)) return failure(reason);
            item->set_visible(request["value"]); return ok();
        case M_NODE2D_POSITION_GET: return ok(vector_array(node2d->get_position()));
        case M_NODE2D_POSITION_SET:
            if (!vector_of(request.get("value", Variant()), value)) return failure("invalid_argument");
            if (const char *reason = impact_error(node, POSITION)) return failure(reason);
            node2d->set_position(value); return ok();
        case M_CHARACTERBODY2D_VELOCITY_GET: return ok(vector_array(body->get_velocity()));
        case M_CHARACTERBODY2D_VELOCITY_SET:
            if (!vector_of(request.get("value", Variant()), value)) return failure("invalid_argument");
            if (const char *reason = impact_error(node, VELOCITY)) return failure(reason);
            body->set_velocity(value); return ok();
        case M_CHARACTERBODY2D_MOVE_AND_SLIDE:
            if (!body->is_inside_tree()) return failure("phase_conflict");
            if (const char *reason = impact_error(node, POSITION | VELOCITY)) return failure(reason);
            return ok(body->move_and_slide());
        case M_CHARACTERBODY2D_IS_ON_FLOOR: return ok(body->is_on_floor());
        default: return failure("unsupported_member");
    }
}
bool MetonWebGame::has_callback(uint32_t token, const StringName &method) const {
    auto found = objects.find(token);
    if (found == objects.end() || found->second.retired || revoked) return false;
    for (int i = 0; i < 5; ++i) if (method == callbacks[i]) return (found->second.callbacks & (1u << i)) != 0;
    return false;
}
bool MetonWebGame::dispatch_callback(uint32_t token, const StringName &method, const Variant **args, int count) {
    if (!has_callback(token, method)) return false;
    MetonNativeMemoryScope memory_scope(native_memory);
    const uint32_t previous = phase;
    const Stamp previous_stamp = execution_stamp;
    execution_stamp = {token, objects.at(token).epoch};
    active_native_objects.push_back(objects.at(token).object);
    for (int i = 0; i < 5; ++i) if (method == callbacks[i]) phase = callback_phases[i];
    ++execution_depth;
    bool success = meton_web_callback(token, String(method).utf8().get_data(), count ? double(*args[0]) : 0);
    --execution_depth;
    active_native_objects.pop_back(); execution_stamp = previous_stamp; phase = previous;
    prune_connections();
    if (!success) revoked = true;
    return true;
}
void MetonWebGame::entered(uint32_t token) {
    auto found = objects.find(token);
    if (found == objects.end() || found->second.retired || revoked) return;
    auto &entry = found->second;
    if (entry.epoch == UINT64_MAX) { revoked = true; return; }
    ++entry.epoch; entry.active = true; entry.closing = false;
    if (entry.javascript && !meton_web_callback(token, "_validate", 0)) revoked = true;
}
void MetonWebGame::closing(uint32_t token) {
    auto found = objects.find(token); if (found != objects.end()) found->second.closing = true;
}
void MetonWebGame::exited(uint32_t token) {
    auto found = objects.find(token);
    if (found == objects.end() || found->second.retired) return;
    found->second.active = false; found->second.closing = true;
    cleanup_scope(token, false);
    if (!tearing_down) release_created({token, found->second.epoch});
}
void MetonWebGame::object_destroyed(uint32_t token) {
    auto found = objects.find(token);
    if (found == objects.end() || found->second.retired) return;
    forget_impact(found->second.object);
    cleanup_scope(token, true);
    object_tokens.erase(found->second.object); scene_roots.erase(found->second.object);
    found->second.active = false; found->second.closing = true; found->second.retired = true;
    if (!tearing_down) meton_web_callback(token, "_retire", 0);
}
/** JS 保有通知只维护绑定内部表；整局销毁不再进入即将结束的执行域。 */
void MetonWebGame::handle_event(uint32_t p_token, const char *p_event) {
    if (!tearing_down && !meton_web_callback(p_token, p_event, 0)) revoked = true;
}
uint32_t MetonWebGame::create_callable(uint32_t p_target, Stamp p_scope) {
    const uint32_t id = next_callable++;
    callables[id] = {Callable(memnew(MetonWebCallable(get_instance_id(), objects.at(p_target).object, id))), p_target, p_scope};
    return id;
}
bool MetonWebGame::callable_valid(uint32_t token) const {
    if (revoked) return false;
    auto callable = callables.find(token);
    if (callable == callables.end()) return false;
    auto target = objects.find(callable->second.target);
    if (target == objects.end() || target->second.retired) return false;
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(target->second.object));
    return node && !node->is_queued_for_deletion() && (!callable->second.scope.owner || scope_valid(callable->second.scope, false));
}
void MetonWebGame::signal(uint32_t token) {
    if (!callable_valid(token)) return;
    MetonNativeMemoryScope memory_scope(native_memory);
    std::vector<uint32_t> waiting;
    for (const auto &entry : connections) if (entry.second.callable == token && entry.second.waiter) waiting.push_back(entry.first);
    if (!waiting.empty()) { for (uint32_t id : waiting) retire_connection(id, nullptr); return; }
    const CallableEntry callable = callables.at(token);
    auto found = objects.find(callable.target);
    const Stamp previous_stamp = execution_stamp;
    const uint32_t previous = phase;
    execution_stamp = callable.scope.owner ? callable.scope : Stamp{callable.target, found->second.epoch};
    phase = found->second.closing ? P_CLEANUP : P_SIGNAL;
    active_native_objects.push_back(found->second.object);
    ++execution_depth;
    if (!meton_web_callback(token, "_signal", 0)) revoked = true;
    --execution_depth;
    active_native_objects.pop_back(); execution_stamp = previous_stamp; phase = previous;
    prune_connections();
}

Ref<Image> MetonWebGame::take_probe_image() {
    Ref<Image> result = probe_image;
    probe_image.unref();
    return result;
}

Dictionary MetonWebGame::memory_probe(const String &p_name) {
    if (revoked) return failure("scope_ended");
    if (p_name == "allocate") {
        MetonNativeMemoryScope memory_scope(native_memory);
        probe_image = Image::create_empty(1024, 1024, false, Image::FORMAT_RGBA8);
        probe_image->fill(Color(0.2, 0.4, 0.6, 1));
    } else if (p_name == "free") {
        // 此处故意不设置动态预算，证明退款沿分配头中的原所有者。
        probe_image.unref();
    } else if (p_name == "realloc") {
        void *buffer;
        {
            MetonNativeMemoryScope memory_scope(native_memory);
            buffer = memalloc(1024);
        }
        const auto before = native_memory->state();
        buffer = memrealloc(buffer, 1024 * 1024);
        const auto grown = native_memory->state();
        memfree(buffer);
        const auto released = native_memory->state();
        Dictionary result;
        result["growth"] = int64_t(grown.bytes - before.bytes);
        result["refund"] = int64_t(grown.bytes - released.bytes);
        result["blocks_released"] = int64_t(grown.blocks - released.blocks);
        return result;
    } else if (p_name == "aligned") {
        void *buffer;
        {
            MetonNativeMemoryScope memory_scope(native_memory);
            buffer = Memory::alloc_aligned_static(2048, 64);
        }
        const auto before = native_memory->state();
        buffer = Memory::realloc_aligned_static(buffer, 4096, 2048, 64);
        const auto grown = native_memory->state();
        const bool aligned = (reinterpret_cast<uintptr_t>(buffer) % 64) == 0;
        Memory::free_aligned_static(buffer);
        const auto released = native_memory->state();
        Dictionary result;
        result["aligned"] = aligned; result["growth"] = int64_t(grown.bytes - before.bytes);
        result["refund"] = int64_t(grown.bytes - released.bytes); result["blocks_released"] = int64_t(grown.blocks - released.blocks);
        return result;
    } else if (p_name == "png") {
        MetonNativeMemoryScope memory_scope(native_memory);
        Ref<Image> source = Image::create_empty(256, 256, false, Image::FORMAT_RGBA8);
        source->fill(Color(0.2, 0.4, 0.6, 1));
        PackedByteArray png = source->save_png_to_buffer();
        source.unref();
        const auto before = native_memory->state();
        probe_image.instantiate();
        const Error loaded = probe_image->load_png_from_buffer(png);
        const auto decoded = native_memory->state();
        Dictionary result;
        result["decoded"] = loaded == OK; result["width"] = probe_image->get_width(); result["height"] = probe_image->get_height();
        result["allocations"] = int64_t(decoded.allocations - before.allocations);
        return result;
    } else if (p_name == "exhaust") {
        MetonNativeMemoryScope memory_scope(native_memory);
        // 64 MiB 像素载荷尚未分配就会因分配头和已有申请触及同一固定额度。
        probe_image = Image::create_empty(4096, 4096, false, Image::FORMAT_RGBA8);
    } else return failure("unsupported_member");
    return inspect()["native_memory"];
}
