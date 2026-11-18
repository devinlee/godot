#include "meton_game_binding.h"
#include "meton_game_impact.generated.h"
#include "meton_game_support.generated.h"
#include "meton_js_runtime.h"
#include "core/os/thread.h"
#include "scene/main/node.h"
#include "scene/2d/node_2d.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/2d/sprite_2d.h"
#include <mutex>
#include <algorithm>

namespace {
// 原生对象可能在其他线程析构；这里只按 ID 查登记，不把游戏绑定指针交给其他线程使用。
std::mutex delete_mutex;
std::map<ObjectID, MetonGameBinding *> delete_requests;
}

/** 仅遍历本操作真正影响的子树；字段所有者与删除权分别核对。 */
const char *MetonGameBinding::impact_error(Node *p_node, unsigned p_effects) const {
    if (!p_node) return "object_invalid";
    // 原生退出通知可同步重入脚本；不能在同一析构栈里再次删除正在析构的节点或后代。
    for (Node *ancestor = p_node; ancestor; ancestor = ancestor->get_parent()) {
        if (deleting_native_roots.count(ancestor->get_instance_id())) return "object_invalid";
    }
    std::vector<Node *> pending{p_node};
    const bool subtree = p_effects & (DELETE_TREE | POSITION | VISIBLE);
    for (size_t index = 0; index < pending.size(); ++index) {
        Node *node = pending[index];
        uint32_t token = token_of(node);
        if (!token || ((p_effects & DELETE_TREE) && !objects.at(token).owned)) return "permission_denied";
        if ((p_effects & DELETE_IMMEDIATE) && std::find(active_native_objects.begin(), active_native_objects.end(), node->get_instance_id()) != active_native_objects.end()) return "phase_conflict";
        auto claimed = field_owners.find(node->get_instance_id());
        if (claimed != field_owners.end()) {
            const unsigned flags[] = {POSITION, VELOCITY, VISIBLE, TEXTURE};
            for (int field = 0; field < 4; ++field) {
                // velocity/texture 只作用于当前对象；位置与可见性还会影响后代的派生表现。
                if (!(p_effects & flags[field]) || (index && !(flags[field] & (POSITION | VISIBLE)))) continue;
                auto rule = claimed->second.find(MetonImpactSupport::FIELDS[field]);
                if (rule != claimed->second.end() && !(rule->second.phases & phase)) return "phase_conflict";
            }
        }
        if (!subtree) continue;
        int children = node->get_child_count(true);
        if (pending.size() + children > MetonGameSupport::MAX_OBJECTS) return "budget_exceeded";
        for (int child = 0; child < children; ++child) pending.push_back(node->get_child(child, true));
    }
    return nullptr;
}
/** 拒绝发生在原生调用之前，错误不导出私有目标或所有者的详细信息。 */
bool MetonGameBinding::guard_impact(Node *p_node, unsigned p_effects) {
    const char *reason = impact_error(p_node, p_effects);
    if (!reason) return true;
    error(reason, -1, "Native effect is outside the admitted object or field scope");
    return false;
}
/** 先完整验证请求，再更新同一原生字段权威；不同所有者不能互相覆盖。 */
bool MetonGameBinding::claim_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases) {
    if (closed || revoked || !token_of(p_node) || p_node->is_queued_for_deletion() || p_owner.is_empty() || p_owner.length() > 128 ||
            p_fields.is_empty() || p_fields.size() > 4 || p_phases.size() > 8) return false;
    unsigned mask = 0;
    for (int index = 0; index < p_phases.size(); ++index) {
        unsigned bit = 0;
        for (unsigned phase_index = 1; phase_index < 9; ++phase_index) if (p_phases[index] == MetonImpactSupport::PHASES[phase_index]) bit = 1u << phase_index;
        if (!bit || (mask & bit)) return false;
        mask |= bit;
    }
    const bool supports[] = {Object::cast_to<Node2D>(p_node) != nullptr, Object::cast_to<CharacterBody2D>(p_node) != nullptr,
        Object::cast_to<CanvasItem>(p_node) != nullptr, Object::cast_to<Sprite2D>(p_node) != nullptr};
    std::set<String> selected;
    auto original = field_owners.find(p_node->get_instance_id());
    for (int index = 0; index < p_fields.size(); ++index) {
        bool valid = false;
        for (int field = 0; field < 4; ++field) if (p_fields[index] == MetonImpactSupport::FIELDS[field] && supports[field]) valid = true;
        if (!valid || !selected.insert(p_fields[index]).second) return false;
        if (original != field_owners.end()) {
            auto rule = original->second.find(p_fields[index]);
            if (rule != original->second.end() && rule->second.owner != p_owner) return false;
        }
    }
    auto &rules = field_owners[p_node->get_instance_id()];
    for (const auto &field : selected) rules[field] = FieldOwner{p_owner, mask};
    return true;
}
/** 撤回只移除该所有者的登记，不借一个名字释放其他字段或原生对象。 */
bool MetonGameBinding::release_fields(Node *p_node, const String &p_owner) {
    if (closed || revoked || !token_of(p_node) || p_owner.is_empty()) return false;
    auto found = field_owners.find(p_node->get_instance_id());
    if (found == field_owners.end()) return true;
    bool removed = false;
    for (auto field = found->second.begin(); field != found->second.end();) {
        if (field->second.owner == p_owner) { field = found->second.erase(field); removed = true; }
        else ++field;
    }
    if (found->second.empty()) field_owners.erase(found);
    return removed;
}
/** Object.free 是原生特殊分派；Resource 等 RefCounted 不获得直接删除入口。 */
bool MetonGameBinding::impact_exports(v8::Local<v8::Context> p_context) {
    classes.at("Node").Get(isolate)->PrototypeTemplate()->Set(text("queue_free"), v8::FunctionTemplate::New(isolate, delete_dispatch, v8::Int32::New(isolate, 0)));
    classes.at("Object").Get(isolate)->PrototypeTemplate()->Set(text("free"), v8::FunctionTemplate::New(isolate, delete_dispatch, v8::Int32::New(isolate, 1)));
    return true;
}
/** 所有权检查不把原生队列调用转换为立即删除；排队后在原生析构前再检查。 */
void MetonGameBinding::delete_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    int operation = p_args.DataV2().As<v8::Int32>()->Value();
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Game scope has ended"); return; }
    if (!(MetonImpactSupport::MEMBERS[operation].phases & binding->phase)) { binding->error("phase_conflict", -1, "Deletion is unavailable in this phase"); return; }
    if (!binding->charge_call(-1)) return;
    if (p_args.Length()) { binding->error("invalid_argument", -1, "Deletion takes no arguments"); return; }
    Node *node = binding->node(p_args.This(), -1);
    if (!node || !binding->guard_impact(node, DELETE_TREE | (operation ? DELETE_IMMEDIATE : 0))) return;
    const ObjectID id = node->get_instance_id();
    {
        std::lock_guard<std::mutex> lock(delete_mutex);
        delete_requests[id] = binding;
    }
    if (!operation) { node->queue_free(); return; }
    Callable::CallError call_error;
    node->callp("free", nullptr, 0, call_error);
    // 调用可能因原生锁或通知拒绝而没有删除；不能留下将来可冒用的授权。
    {
        std::lock_guard<std::mutex> lock(delete_mutex);
        delete_requests.erase(id);
    }
    if (call_error.error != Callable::CallError::CALL_OK || ObjectDB::get_instance(id)) {
        binding->deleting_native_roots.erase(id);
        binding->error("execution_failed", -1, "Native object could not be freed");
    }
}
/** 核对最终实际子树，失败时绝不先发送退出通知或释放任何子节点。 */
bool meton_game_predelete_guard(Object *p_object) {
    MetonGameBinding *binding;
    {
        std::lock_guard<std::mutex> lock(delete_mutex);
        auto request = delete_requests.find(p_object->get_instance_id());
        if (request == delete_requests.end()) return true;
        if (!Thread::is_main_thread()) return false;
        binding = request->second;
        delete_requests.erase(request);
    }
    Node *node = Object::cast_to<Node>(p_object);
    if (!binding->closed && !binding->revoked && !binding->impact_error(node, MetonGameBinding::DELETE_TREE)) {
        binding->deleting_native_roots.insert(p_object->get_instance_id());
        return true;
    }
    ++binding->delayed_delete_rejections;
    binding->runtime->reject_game_effect("Deferred native deletion no longer has authority for its actual subtree");
    return false;
}
/** 游戏停止撤回尚未执行的请求，随后按原拥有者逻辑保全借用与私有子树。 */
void MetonGameBinding::clear_delete_requests() {
    std::lock_guard<std::mutex> lock(delete_mutex);
    for (auto request = delete_requests.begin(); request != delete_requests.end();) {
        if (request->second == this) request = delete_requests.erase(request);
        else ++request;
    }
}
/** 原生寿命结束后清掉字段登记；重建对象不会继承旧登记。 */
void MetonGameBinding::forget_impact(ObjectID p_object) {
    field_owners.erase(p_object);
    deleting_native_roots.erase(p_object);
    std::lock_guard<std::mutex> lock(delete_mutex);
    delete_requests.erase(p_object);
}
/** 只向受信宿主交出计数和迟到拒绝，不给游戏查其他所有者的规则。 */
Dictionary MetonGameBinding::impact_state() const {
    Dictionary result;
    result["managed_objects"] = int(field_owners.size());
    result["delayed_delete_rejections"] = int(delayed_delete_rejections);
    std::lock_guard<std::mutex> lock(delete_mutex);
    unsigned pending = 0;
    for (const auto &request : delete_requests) pending += request.second == this;
    result["pending_deletes"] = int(pending);
    return result;
}
