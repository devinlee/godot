#include "web_game.h"
#include "web_script.h"
#include "web_support.generated.h"
#include "meton_game_objects.generated.h"
#include "scene/2d/node_2d.h"
#include "scene/2d/sprite_2d.h"
#include "scene/resources/packed_scene.h"
#include <cmath>

namespace {
Dictionary ok(const Variant &p_value = Variant()) { Dictionary value; value["ok"] = true; value["value"] = p_value; return value; }
/** JSON 数值到句柄的转换必须精确，不允许截断或环绕到另一个对象。 */
bool token(const Variant &p_value, uint32_t &r_token) {
    if ((p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) || !std::isfinite(double(p_value)) ||
            double(p_value) < 1 || double(p_value) > UINT32_MAX || double(p_value) != uint32_t(double(p_value))) return false;
    r_token = uint32_t(double(p_value)); return true;
}
}

/** 同一原生寿命只分配一个单调 token；原生名字或 parent 不参与所属权判断。 */
uint32_t MetonWebGame::token_of(Node *p_node) const {
    if (!p_node) return 0;
    auto found = object_tokens.find(p_node->get_instance_id());
    return found == object_tokens.end() ? 0 : found->second;
}
size_t MetonWebGame::live_objects() const {
    size_t count = 0;
    for (const auto &entry : objects) count += !entry.second.retired;
    return count;
}
/** 原生通知可能仍引用条目；退出通知栈之后再清墓碑，额度在退休时已归还。 */
void MetonWebGame::prune_objects() {
    if (!active_native_objects.empty()) return;
    for (auto entry = objects.begin(); entry != objects.end();) {
        if (entry->second.retired) entry = objects.erase(entry); else ++entry;
    }
}
Dictionary MetonWebGame::wrap_node(Node *p_node) {
    if (!p_node) return ok();
    const uint32_t id = token_of(p_node);
    if (!id) return failure("permission_denied");
    auto found = objects.find(id);
    if (found == objects.end() || found->second.retired || p_node->is_queued_for_deletion()) return failure("object_invalid");
    Dictionary value; value["token"] = int64_t(id); value["type"] = p_node->get_class_name();
    return ok(value);
}
/** 路径以码点数组跨边界；BOM 保留为普通名字字符，不经过会改变含义的 UTF-8 字符串入口。 */
Dictionary MetonWebGame::object_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    const int operation = op == "get_node" ? 0 : 1;
    if (op != "get_node" && op != "get_node_or_null") return failure("unsupported_member");
    if (!(MetonObjectSupport::MEMBERS[operation].phases & phase)) return failure("phase_conflict");
    uint32_t id;
    if (!token(p_request.get("token", Variant()), id)) return failure("invalid_argument");
    auto found = objects.find(id);
    if (found == objects.end() || found->second.retired) return failure("object_invalid");
    auto *source = Object::cast_to<Node>(ObjectDB::get_instance(found->second.object));
    if (!source || source->is_queued_for_deletion()) return failure("object_invalid");
    const Variant raw = p_request.get("path", Variant());
    if (raw.get_type() != Variant::ARRAY) return failure("invalid_argument");
    Array points = raw;
    if (points.is_empty() || points.size() > 1024) return failure("invalid_argument");
    String path;
    unsigned units = 0;
    for (const Variant &value : points) {
        if ((value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) || !std::isfinite(double(value)) ||
                double(value) < 32 || double(value) > 0x10ffff || double(value) != uint32_t(double(value))) return failure("invalid_argument");
        const char32_t point = uint32_t(double(value));
        if (point == 127 || point == ':' || (point >= 0xd800 && point <= 0xdfff)) return failure("invalid_argument");
        units += point > 0xffff ? 2 : 1;
        if (units > 1024) return failure("invalid_argument");
        path += point;
    }
    Node *result = operation == 0 ? source->get_node(NodePath(path)) : source->get_node_or_null(NodePath(path));
    return wrap_node(result);
}
/** 整树先检查数量、类型、脚本和身份，然后一次登记，失败不覆盖原脚本。 */
bool MetonWebGame::adopt_tree(Node *p_root, bool p_owned) {
    if (revoked || !p_root || p_root->get_parent() || p_root->is_inside_tree()) return false;
    prune_objects();
    const size_t alive = live_objects() + private_ui_objects();
    std::vector<Node *> pending{p_root};
    for (size_t i = 0; i < pending.size(); ++i) {
        Node *node = pending[i];
        const String type = node->get_class_name();
        if (alive + pending.size() > MetonGameSupport::MAX_OBJECTS || !next_object || pending.size() > uint64_t(UINT32_MAX) - next_object + 1 ||
                node->get_script_instance() || node->get_script().get_type() != Variant::NIL || token_of(node) || node->is_queued_for_deletion() ||
                (type != "Node" && type != "Node2D" && type != "CharacterBody2D" && type != "Sprite2D")) return false;
        const int children = node->get_child_count(true);
        if (alive + pending.size() + children > MetonGameSupport::MAX_OBJECTS) return false;
        for (int child = 0; child < children; ++child) pending.push_back(node->get_child(child, true));
    }
    for (Node *node : pending) {
        const uint32_t id = next_object++;
        Entry entry; entry.object = node->get_instance_id(); entry.owned = p_owned;
        objects[id] = entry; object_tokens[entry.object] = id;
        node->set_script_instance(memnew(MetonWebScriptInstance(this, id, node)));
    }
    return true;
}
bool MetonWebGame::adopt_node_tree(Node *p_root, bool p_owned) {
    MetonNativeMemoryScope memory_scope(native_memory);
    return adopt_tree(p_root, p_owned);
}
/** 父节点的释放权不包含借用或未接管的子树，收尾前交还这些原生对象。 */
void MetonWebGame::detach_foreign(Node *p_node) {
    for (int i = p_node->get_child_count(true) - 1; i >= 0; --i) {
        Node *child = p_node->get_child(i, true);
        const uint32_t id = token_of(child);
        if (!id || !objects.at(id).owned) p_node->remove_child(child);
        else detach_foreign(child);
    }
}
void MetonWebGame::release_node(uint32_t p_token) {
    auto found = objects.find(p_token);
    if (found == objects.end() || found->second.retired) return;
    Node *node = Object::cast_to<Node>(ObjectDB::get_instance(found->second.object));
    if (!node || token_of(node) != p_token) return;
    if (found->second.owned) { detach_foreign(node); memdelete(node); }
    else node->set_script_instance(nullptr);
}
/** 只回收离树代次创建且仍未挂父节点的对象，已挂树对象继续沿原生树寿命。 */
void MetonWebGame::release_created(Stamp p_creator) {
    std::vector<uint32_t> retired;
    for (const auto &entry : objects) {
        if (entry.second.retired || entry.second.creator.owner != p_creator.owner || entry.second.creator.epoch != p_creator.epoch) continue;
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(entry.second.object));
        if (node && !node->get_parent()) retired.push_back(entry.first);
    }
    for (uint32_t id : retired) release_node(id);
}
Node *MetonWebGame::instantiate_content_scene(const String &p_name) {
    if (revoked) return nullptr;
    MetonNativeMemoryScope memory_scope(native_memory);
    prune_objects();
    const size_t nodes = content.scene_nodes(p_name);
    if (!nodes || nodes + live_objects() + private_ui_objects() > MetonGameSupport::MAX_OBJECTS || !next_object || nodes > uint64_t(UINT32_MAX) - next_object + 1) return nullptr;
    Ref<PackedScene> scene = content.load(p_name);
    if (scene.is_null() || !content.reserve_scene(p_name)) return nullptr;
    Node *root = scene->instantiate();
    if (!root) return nullptr;
    if (!content.matches_scene(root, p_name) || !adopt_tree(root, true)) { memdelete(root); return nullptr; }
    content.remember_scene(root);
    scene_roots[root->get_instance_id()] = content.resolve(p_name);
    return root;
}
/** 只给受信夹具查询内部计数，普通脚本不能获取原生地址或签发句柄。 */
Dictionary MetonWebGame::object_probe(const String &p_name) {
    Dictionary result;
    result["live"] = int(live_objects()); result["records"] = int(objects.size()); result["views"] = int(scene_views.size());
    return result;
}
