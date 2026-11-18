#include "web_game.h"
#include "meton_game_impact.generated.h"
#include "web_support.generated.h"
#include "core/os/thread.h"
#include "scene/2d/node_2d.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/2d/sprite_2d.h"
#include <emscripten.h>
#include <mutex>
#include <algorithm>
#include <cmath>

namespace {
// 这里只按实际 ObjectID 认原生已登记请求；私有场景和其他来源不会自动取得所属权。
std::mutex delete_mutex;
std::map<ObjectID, MetonWebGame *> delete_requests;
Dictionary ok(const Variant &p_value = Variant()) { Dictionary value; value["ok"] = true; value["value"] = p_value; return value; }
}

/** 仅遍历本操作真正影响的子树；字段所有者与删除权分别核对。 */
const char *MetonWebGame::impact_error(Node *p_node, unsigned p_effects) const {
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
/** 先完整验证请求，再更新同一原生字段权威；不同所有者不能互相覆盖。 */
bool MetonWebGame::claim_fields(Node *p_node, const PackedStringArray &p_fields, const String &p_owner, const PackedStringArray &p_phases) {
    if (revoked || !token_of(p_node) || p_node->is_queued_for_deletion() || p_owner.is_empty() || p_owner.length() > 128 ||
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
bool MetonWebGame::release_fields(Node *p_node, const String &p_owner) {
    if (revoked || !token_of(p_node) || p_owner.is_empty()) return false;
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
/** 游戏停止撤回尚未执行的请求，随后按原拥有者逻辑保全借用与私有子树。 */
void MetonWebGame::clear_delete_requests() {
    std::lock_guard<std::mutex> lock(delete_mutex);
    for (auto request = delete_requests.begin(); request != delete_requests.end();) {
        if (request->second == this) request = delete_requests.erase(request);
        else ++request;
    }
}
/** 原生寿命结束后清掉字段登记；重建对象不会继承旧登记。 */
void MetonWebGame::forget_impact(ObjectID p_object) {
    field_owners.erase(p_object);
    deleting_native_roots.erase(p_object);
    std::lock_guard<std::mutex> lock(delete_mutex);
    delete_requests.erase(p_object);
}
/** 只向受信宿主交出计数和迟到拒绝，不给游戏查其他所有者的规则。 */
Dictionary MetonWebGame::impact_state() const {
    Dictionary result;
    result["managed_objects"] = int(field_owners.size());
    result["delayed_delete_rejections"] = int(delayed_delete_rejections);
    std::lock_guard<std::mutex> lock(delete_mutex);
    unsigned pending = 0;
    for (const auto &request : delete_requests) pending += request.second == this;
    result["pending_deletes"] = int(pending);
    return result;
}

/** 请求与实际析构分别复验，不把 queue_free 改成同步删除。 */
Dictionary MetonWebGame::impact_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    const int operation = op == "free" ? 1 : 0;
    if (!(MetonImpactSupport::MEMBERS[operation].phases & phase)) return failure("phase_conflict");
    const Variant raw = p_request.get("token", Variant());
    if ((raw.get_type() != Variant::INT && raw.get_type() != Variant::FLOAT) || !std::isfinite(double(raw)) ||
            double(raw) < 1 || double(raw) > UINT32_MAX || double(raw) != uint32_t(double(raw))) return failure("invalid_argument");
    auto found = objects.find(uint32_t(double(raw)));
    if (found == objects.end() || found->second.retired) return failure("object_invalid");
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(found->second.object));
    if (!node || node->is_queued_for_deletion()) return failure("object_invalid");
    if (const char *reason = impact_error(node, DELETE_TREE | (operation ? DELETE_IMMEDIATE : 0))) return failure(reason);
    const ObjectID id = node->get_instance_id();
    { std::lock_guard<std::mutex> lock(delete_mutex); delete_requests[id] = this; }
    if (!operation) { node->queue_free(); return ok(); }
    Callable::CallError error;
    node->callp("free", nullptr, 0, error);
    { std::lock_guard<std::mutex> lock(delete_mutex); delete_requests.erase(id); }
    if (error.error != Callable::CallError::CALL_OK || ObjectDB::get_instance(id)) {
        deleting_native_roots.erase(id); return failure("execution_failed");
    }
    return ok();
}

/** 与原生 B4 同一接缝：先拒绝和封锁；外部 renderer 权威负责确认整局结束。 */
bool meton_web_predelete_guard(Object *p_object) {
    MetonWebGame *game;
    {
        std::lock_guard<std::mutex> lock(delete_mutex);
        auto request = delete_requests.find(p_object->get_instance_id());
        if (request == delete_requests.end()) return true;
        if (!Thread::is_main_thread()) return false;
        game = request->second; delete_requests.erase(request);
    }
    if (!game->revoked && !game->impact_error(Object::cast_to<Node>(p_object), MetonWebGame::DELETE_TREE)) {
        game->deleting_native_roots.insert(p_object->get_instance_id()); return true;
    }
    ++game->delayed_delete_rejections;
    game->revoked = true;
    game->last_fault["code"] = "permission_denied";
    game->last_fault["operation"] = "deferred_delete";
    game->last_fault["committed"] = false;
    // 诊断只交受信装配，不开放给普通脚本重新派发或挑选原生目标。
    EM_ASM({ globalThis.metonC2ImpactFault(); });
    return false;
}
