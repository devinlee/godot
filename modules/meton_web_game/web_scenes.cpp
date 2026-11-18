#include "web_game.h"
#include "web_support.generated.h"
#include <cmath>

namespace {
Dictionary ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
/** 句柄只接受 JSON 能精确表达的正整数。 */
bool token(const Variant &p_value, uint32_t &r_token) {
    if ((p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) || !std::isfinite(double(p_value)) ||
            double(p_value) < 1 || double(p_value) > UINT32_MAX || double(p_value) != uint32_t(double(p_value))) return false;
    r_token = uint32_t(double(p_value)); return true;
}
}

/** 使用真实原生结构通知；改名再改回也会改变代次，不让旧视图悄悄复活。 */
void MetonWebGame::structure_changed(uint32_t p_token) {
    if (tearing_down) return;
    auto entry = objects.find(p_token);
    if (entry == objects.end() || entry->second.retired) return;
    Node *node = Object::cast_to<Node>(ObjectDB::get_instance(entry->second.object));
    while (node) {
        auto current = objects.find(token_of(node));
        if (current == objects.end() || current->second.retired) break;
        if (current->second.structure == UINT64_MAX) { revoked = true; return; }
        ++current->second.structure;
        node = node->get_parent();
    }
}

/** 固定描述只选择已有场景身份；每次对象使用仍按原生来源、入树和结构复验。 */
Dictionary MetonWebGame::scene_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    if (op == "scene_release") {
        uint32_t id;
        if (!token(p_request.get("view", Variant()), id)) return failure("invalid_argument");
        return ok(scene_views.erase(id) != 0);
    }
    if (op == "scene_of") {
        if (!(phase & (MetonGameSupport::P_ENTER_TREE | MetonGameSupport::P_READY | MetonGameSupport::P_PROCESS |
                MetonGameSupport::P_PHYSICS | MetonGameSupport::P_SIGNAL | MetonGameSupport::P_SAFE_POINT))) return failure("phase_conflict");
        uint32_t id;
        if (!token(p_request.get("token", Variant()), id)) return failure("invalid_argument");
        auto found = objects.find(id);
        if (found == objects.end() || found->second.retired) return failure("object_invalid");
        auto &owner = found->second;
        Node *root = Object::cast_to<Node>(ObjectDB::get_instance(owner.object));
        if (!root || root->is_queued_for_deletion()) return failure("object_invalid");
        if (!owner.active || owner.closing) return failure("scope_ended");
        const Variant name = p_request.get("descriptor", Variant());
        if (name.get_type() != Variant::STRING || String(name).length() != 64) return failure("invalid_argument");
        const Dictionary *descriptor = content.descriptor(name);
        auto provenance = scene_roots.find(root->get_instance_id());
        if (!descriptor || provenance == scene_roots.end() || provenance->second != content.resolve("res://" + String((*descriptor)["path"]))) return failure("permission_denied");
        if (root->get_class_name() != StringName((*descriptor)["root_type"])) return failure("object_invalid");
        const Array roles = (*descriptor)["nodes"];
        std::vector<ObjectID> captured;
        Array names;
        for (const Variant &value : roles) {
            const Dictionary role = value;
            Node *node = root->get_node_or_null(NodePath(role["path"]));
            names.push_back(role["role"]);
            if (!node) {
                if (!bool(role["optional"])) return failure("object_invalid");
                captured.push_back(ObjectID()); continue;
            }
            if (!token_of(node)) return failure("permission_denied");
            if (node->is_queued_for_deletion() || node->get_class_name() != StringName(role["type"])) return failure("object_invalid");
            captured.push_back(node->get_instance_id());
        }
        if (scene_views.size() >= MetonGameSupport::MAX_OBJECTS || !next_view) return failure("budget_exceeded");
        const uint32_t view = next_view++;
        scene_views[view] = {id, owner.epoch, owner.structure, std::move(captured)};
        Dictionary result; result["token"] = int64_t(view); result["roles"] = names;
        return ok(result);
    }
    if (op == "scene_get") {
        uint32_t id;
        if (!token(p_request.get("view", Variant()), id)) return failure("invalid_argument");
        auto found = scene_views.find(id);
        if (found == scene_views.end()) return failure("object_invalid");
        const auto &view = found->second;
        auto owner = objects.find(view.root);
        if (owner == objects.end() || owner->second.retired || !owner->second.active || owner->second.closing || owner->second.epoch != view.epoch) return failure("scope_ended");
        if (owner->second.structure != view.structure) return failure("object_invalid");
        const Variant raw = p_request.get("index", Variant());
        if ((raw.get_type() != Variant::INT && raw.get_type() != Variant::FLOAT) || !std::isfinite(double(raw)) ||
                double(raw) < 0 || double(raw) >= view.nodes.size() || double(raw) != uint32_t(double(raw))) return failure("invalid_argument");
        const ObjectID object = view.nodes[uint32_t(double(raw))];
        if (object == ObjectID()) return ok();
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(object));
        if (!node) return failure("object_invalid");
        return wrap_node(node);
    }
    return failure("unsupported_member");
}
