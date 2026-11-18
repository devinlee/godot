#include "meton_game_binding.h"
#include "meton_game_objects.generated.h"
#include "meton_game_support.generated.h"
#include "meton_game_script.h"
#include "scene/main/node.h"

std::map<ObjectID, MetonGameBinding *> MetonGameBinding::object_owners;

/** 只认原生登记，不从父节点、Godot owner 属性或 JS 原型推导权限。 */
bool MetonGameBinding::owns(Node *p_node) const {
    auto found = p_node ? object_owners.find(p_node->get_instance_id()) : object_owners.end();
    return found != object_owners.end() && found->second == this;
}
/** 每个实际 ObjectID 只有一个本游戏包装；旧 token 不分配给重建节点。 */
uint32_t MetonGameBinding::token_of(Node *p_node) const {
    if (!owns(p_node)) return 0;
    for (const auto &entry : objects) if (entry.second.native == p_node->get_instance_id()) return entry.first;
    return 0;
}
/** 只安装 A1 列出的两个节点查询，不引入通用反射或属性路径。 */
bool MetonGameBinding::object_exports(v8::Local<v8::Context> p_context) {
    auto prototype = classes.at("Node").Get(isolate)->PrototypeTemplate();
    for (int i = 0; i < 2; ++i) prototype->Set(text(MetonObjectSupport::MEMBERS[i].name),
        v8::FunctionTemplate::New(isolate, object_dispatch, v8::Int32::New(isolate, i)));
    return true;
}
/** 保留原生缺失诊断，返回之前再次检查结果对象的寿命和所属游戏。 */
void MetonGameBinding::object_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int operation = p_args.DataV2().As<v8::Int32>()->Value();
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Game scope has ended"); return; }
    if (!(MetonObjectSupport::MEMBERS[operation].phases & binding->phase)) { binding->error("phase_conflict", -1, "Lookup is unavailable in this phase"); return; }
    if (!binding->charge_call(-1)) return;
    if (p_args.Length() != 1 || !p_args[0]->IsString()) { binding->error("invalid_argument", -1, "Node lookup requires one path string"); return; }
    auto input = p_args[0].As<v8::String>();
    if (!input->Length() || input->Length() > 1024) { binding->error("invalid_argument", -1, "Node path length is invalid"); return; }
    v8::String::Value units(binding->isolate, input);
    String path;
    // Godot 的 UTF-8 入口会截断 NUL、去掉 BOM；路径必须保留 JS 的原始码点，不能静默变成另一个对象名。
    for (int i = 0; i < units.length(); ++i) {
        char32_t point = (*units)[i];
        if (point >= 0xd800 && point <= 0xdbff) {
            if (i + 1 >= units.length() || (*units)[i + 1] < 0xdc00 || (*units)[i + 1] > 0xdfff) {
                binding->error("invalid_argument", -1, "Node path contains an unpaired surrogate"); return;
            }
            point = 0x10000 + ((point - 0xd800) << 10) + ((*units)[++i] - 0xdc00);
        } else if (point >= 0xdc00 && point <= 0xdfff) {
            binding->error("invalid_argument", -1, "Node path contains an unpaired surrogate"); return;
        }
        if (point < 32 || point == 127) { binding->error("invalid_argument", -1, "Node path contains a control character"); return; }
        path += point;
    }
    if (path.contains(":")) {
        binding->error("invalid_argument", -1, "Expected a node path without subnames"); return;
    }
    Node *source = binding->node(p_args.This(), -1);
    if (!source) return;
    Node *result = operation == 0 ? source->get_node(NodePath(path)) : source->get_node_or_null(NodePath(path));
    if (!result) { p_args.GetReturnValue().Set(v8::Null(binding->isolate)); return; }
    uint32_t token = binding->token_of(result);
    if (!token) { binding->error("permission_denied", -1, "Returned node is outside the admitted game objects"); return; }
    auto &entry = binding->objects.at(token);
    if (result->is_queued_for_deletion() || entry.wrapper.IsEmpty()) { binding->error("object_invalid", -1, "Returned node lifetime has ended"); return; }
    p_args.GetReturnValue().Set(entry.wrapper.Get(binding->isolate));
}
/** 先完成整树的有界准入，再一次交接包装；不会执行原生树上的外来脚本。 */
bool MetonGameBinding::adopt_tree(Node *p_root, bool p_owned) {
    if (closed || revoked || !p_root || p_root->get_parent() || p_root->is_inside_tree()) return false;
    prune_objects();
    const size_t alive = live_objects() + private_ui_objects();
    std::vector<Node *> pending{p_root};
    std::vector<String> types;
    // 整棵树先复验再登记，失败不接管半棵树，也不覆写既有脚本或别的游戏登记。
    for (size_t i = 0; i < pending.size(); ++i) {
        Node *node = pending[i];
        if (alive + pending.size() > MetonGameSupport::MAX_OBJECTS || !next_object || pending.size() > uint64_t(UINT32_MAX) - next_object + 1 || node->get_script_instance() ||
                node->get_script().get_type() != Variant::NIL || object_owners.count(node->get_instance_id()) || node->is_queued_for_deletion()) return false;
        String type = node->get_class_name();
        if (!classes.count(type)) return false;
        types.push_back(type);
        const int children = node->get_child_count(true);
        // 原生宿主树也可能很大，先核对余量再增长临时队列。
        if (alive + pending.size() + children > MetonGameSupport::MAX_OBJECTS) return false;
        for (int child = 0; child < children; ++child) pending.push_back(node->get_child(child, true));
    }
    std::vector<v8::Local<v8::Object>> wrappers;
    for (const auto &type : types) {
        v8::Local<v8::Object> wrapper;
        // 原生接入不调用 JS 构造器，避免构造另一个节点或运行外来回调。
        if (!classes.at(type).Get(isolate)->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()).ToLocal(&wrapper)) return false;
        wrappers.push_back(wrapper);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        const uint32_t token = next_object++;
        auto &entry = objects[token];
        entry.native = pending[i]->get_instance_id(); entry.owned = p_owned;
        entry.wrapper.Reset(isolate, wrappers[i]);
        wrappers[i]->SetInternalField(0, v8::External::New(isolate, this, v8::ExternalPointerTypeTag(1)));
        wrappers[i]->SetInternalField(1, v8::Uint32::New(isolate, token));
        object_owners[entry.native] = this;
        pending[i]->set_script_instance(memnew(MetonGameScriptInstance(this, token, pending[i])));
    }
    return true;
}
/** 删除父节点前保全不属于本方释放权的子树，并记录交回原拥有者的事实。 */
void MetonGameBinding::detach_foreign(Node *p_node) {
    // 所有权不沿 reparent 转移。拆下非本游戏或借用的子树，避免原生父节点析构连带删除。
    for (int i = p_node->get_child_count(true) - 1; i >= 0; --i) {
        Node *child = p_node->get_child(i, true);
        const uint32_t token = token_of(child);
        if (!token || !objects.at(token).owned) {
            Dictionary detached;
            detached["object_id"] = String::num_uint64(uint64_t(child->get_instance_id()));
            detached["reason"] = token ? "borrowed" : "foreign_owner";
            detached_objects.push_back(detached);
            p_node->remove_child(child);
        }
        else detach_foreign(child);
    }
}
/** 按实际拥有权释放本方根或借用包装，已释放和非本方对象均幂等跳过。 */
void MetonGameBinding::release_node(uint32_t p_token) {
    auto found = objects.find(p_token);
    if (found == objects.end()) return;
    Node *node = Object::cast_to<Node>(ObjectDB::get_instance(found->second.native));
    if (!node || !owns(node)) { object_owners.erase(found->second.native); return; }
    if (found->second.owned) {
        detach_foreign(node);
        ++released_nodes;
        memdelete(node);
    } else {
        // 借用包装只释放本方 ScriptInstance 与连接；原对象寿命仍属于交接者。
        node->set_script_instance(nullptr);
        ++released_borrows;
    }
    object_owners.erase(found->second.native);
}
/** 外部宿主在 VM 结束后仍能查询被保全的原生对象，不将它们隐去。 */
Dictionary MetonGameBinding::release_state() const {
    Dictionary report;
    report["closed"] = closed;
    report["deleted_roots"] = int(released_nodes);
    report["borrowed_wrappers_released"] = int(released_borrows);
    report["detached_for_original_owner"] = detached_objects.duplicate(true);
    report["resources"] = resource_cleanup.duplicate(true);
    report["private_ui"] = ui_state();
    report["impact"] = impact_state();
    return report;
}
/** 入树作用域结束仅回收仍未挂父节点的创建物，已挂树的保留原生寿命。 */
void MetonGameBinding::release_created(Stamp p_creator) {
    std::vector<uint32_t> retired;
    for (const auto &entry : objects) if (entry.second.creator.owner == p_creator.owner && entry.second.creator.epoch == p_creator.epoch) {
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(entry.second.native));
        if (node && !node->get_parent()) retired.push_back(entry.first);
    }
    // 快照避免析构递归清理修改正在遍历的容器；挂树对象交给原生树寿命。
    for (uint32_t token : retired) release_node(token);
}
