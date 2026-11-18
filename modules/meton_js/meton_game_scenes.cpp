#include "meton_game_binding.h"
#include "meton_game_content.h"
#include "meton_game_support.generated.h"
#include "scene/main/node.h"

namespace {
constexpr v8::ExternalPointerTypeTag SCENE_TAG = 6;
}

/** 通知来自真实原生结构变化；只传播到本游戏已登记的祖先。 */
void MetonGameBinding::structure_changed(uint32_t p_token) {
    if (closed) return;
    auto entry = objects.find(p_token);
    if (entry == objects.end()) return;
    Node *node = Object::cast_to<Node>(ObjectDB::get_instance(entry->second.native));
    while (node && owns(node)) {
        auto owner = objects.find(token_of(node));
        if (owner == objects.end()) break;
        ++owner->second.structure_epoch;
        node = node->get_parent();
    }
}
/** GC 回调只清 JS 句柄；条目在下一次创建视图时移除。 */
void MetonGameBinding::weak_scene(const v8::WeakCallbackInfo<SceneView> &p_info) {
    auto *entry = p_info.GetParameter(); entry->wrapper.Reset(); entry->retired = true;
}
/** 描述选择不能扩大权限，根、布局、节点类型和本次入树均以原生对象复验。 */
void MetonGameBinding::scene_of(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    auto ctx = binding->isolate->GetCurrentContext();
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Game scope has ended"); return; }
    if (!(binding->phase & (MetonGameSupport::P_ENTER_TREE | MetonGameSupport::P_READY | MetonGameSupport::P_PROCESS |
            MetonGameSupport::P_PHYSICS | MetonGameSupport::P_SIGNAL | MetonGameSupport::P_SAFE_POINT))) {
        binding->error("phase_conflict", -1, "Scene resolution is unavailable in this phase"); return;
    }
    if (!binding->charge_call(-1)) return;
    if (p_args.Length() != 2 || !p_args[0]->IsObject() || !p_args[1]->IsObject() || p_args[1]->IsProxy()) { binding->error("invalid_argument", -1, "sceneOf expects a bound root and fixed descriptor"); return; }
    Node *root = binding->node(p_args[0].As<v8::Object>(), -1);
    if (!root) return;
    const uint32_t token = binding->token_of(root);
    auto &owner = binding->objects.at(token);
    if (!owner.in_tree || owner.closing) { binding->error("scope_ended", -1, "Scene root has no active tree entry"); return; }
    const auto input = p_args[1].As<v8::Object>();
    v8::Local<v8::Value> property, identity;
    if (!input->GetOwnPropertyDescriptor(ctx, binding->text("id")).ToLocal(&property)) return;
    // 不能通过用户 getter/Proxy 在验证描述身份时执行任意代码。
    if (!property->IsObject() || !property.As<v8::Object>()->HasOwnProperty(ctx, binding->text("value")).FromMaybe(false) ||
            !property.As<v8::Object>()->Get(ctx, binding->text("value")).ToLocal(&identity) || !identity->IsString() || identity.As<v8::String>()->Length() != 64) {
        binding->error("invalid_argument", -1, "Descriptor identity must be an own string value"); return;
    }
    const Dictionary *descriptor = binding->content ? binding->content->descriptor(binding->string(identity)) : nullptr;
    auto provenance = binding->scene_roots.find(root->get_instance_id());
    if (!descriptor || provenance == binding->scene_roots.end() || provenance->second != binding->content->resolve("res://" + String((*descriptor)["path"]))) {
        binding->error("permission_denied", -1, "Descriptor does not belong to this fixed scene instance"); return;
    }
    if (root->get_class_name() != StringName((*descriptor)["root_type"])) { binding->error("object_invalid", -1, "Scene root type differs from its fixed descriptor"); return; }
    const Array roles = (*descriptor)["nodes"];
    std::vector<ObjectID> captured;
    for (const Dictionary &role : roles) {
        Node *node = root->get_node_or_null(NodePath(role["path"]));
        if (!node) {
            if (!bool(role["optional"])) { binding->error("object_invalid", -1, "Required scene node is missing"); return; }
            captured.push_back(ObjectID()); continue;
        }
        if (!binding->owns(node)) { binding->error("permission_denied", -1, "Scene role reaches an unadmitted node"); return; }
        if (node->get_class_name() != StringName(role["type"])) { binding->error("object_invalid", -1, "Scene role type has changed"); return; }
        captured.push_back(node->get_instance_id());
    }
    for (auto entry = binding->scene_views.begin(); entry != binding->scene_views.end();) {
        if (entry->second.retired) entry = binding->scene_views.erase(entry); else ++entry;
    }
    if (binding->scene_views.size() >= MetonGameSupport::MAX_OBJECTS) { binding->error("budget_exceeded", -1, "Scene view budget exceeded"); return; }
    auto definition = v8::ObjectTemplate::New(binding->isolate);
    definition->SetInternalFieldCount(6);
    v8::Local<v8::Object> wrapper;
    if (!definition->NewInstance(ctx).ToLocal(&wrapper)) return;
    const uint32_t view_token = binding->next_view++;
    wrapper->SetInternalField(0, v8::External::New(binding->isolate, binding, SCENE_TAG));
    wrapper->SetInternalField(1, v8::Uint32::New(binding->isolate, view_token));
    for (int index = 0; index < roles.size(); ++index) {
        Dictionary role = roles[index];
        v8::Local<v8::Function> getter;
        // 访问器绑定原视图和字段；借到另一视图不能把同一数组下标误解释成另一个角色。
        const uint64_t field = (uint64_t(view_token) << 32) | uint32_t(index);
        if (!v8::Function::New(ctx, scene_get, v8::BigInt::NewFromUnsigned(binding->isolate, field)).ToLocal(&getter)) return;
        v8::PropertyDescriptor property(getter, v8::Undefined(binding->isolate)); property.set_enumerable(true); property.set_configurable(false);
        if (!wrapper->DefineProperty(ctx, binding->text(role["role"]), property).FromMaybe(false)) return;
    }
    if (!wrapper->SetIntegrityLevel(ctx, v8::IntegrityLevel::kFrozen).FromMaybe(false)) return;
    auto &entry = binding->scene_views[view_token];
    entry.root = token; entry.epoch = owner.epoch; entry.structure = owner.structure_epoch; entry.nodes = std::move(captured);
    entry.wrapper.Reset(binding->isolate, wrapper); entry.wrapper.SetWeak(&entry, weak_scene, v8::WeakCallbackType::kParameter);
    p_args.GetReturnValue().Set(wrapper);
}
/** 每次访问固定身份；结构改过再恢复也失效，不把可选节点当活路径查询。 */
void MetonGameBinding::scene_get(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Game scope has ended"); return; }
    if (!binding->charge_call(-1)) return;
    auto wrapper = p_args.This();
    if (wrapper->InternalFieldCount() != 6 || !wrapper->GetInternalField(0)->IsValue() || !wrapper->GetInternalField(0).As<v8::Value>()->IsExternal() ||
            wrapper->GetInternalField(0).As<v8::External>()->Value(SCENE_TAG) != binding || !wrapper->GetInternalField(1)->IsValue() || !wrapper->GetInternalField(1).As<v8::Value>()->IsUint32()) {
        binding->error("invalid_argument", -1, "Expected the original scene view"); return;
    }
    const uint64_t field = p_args.DataV2().As<v8::BigInt>()->Uint64Value();
    const uint32_t token = wrapper->GetInternalField(1).As<v8::Uint32>()->Value();
    if (token != uint32_t(field >> 32)) { binding->error("invalid_argument", -1, "Scene accessor belongs to another view"); return; }
    auto found = binding->scene_views.find(token);
    if (found == binding->scene_views.end() || found->second.retired) { binding->error("object_invalid", -1, "Scene view has ended"); return; }
    const auto &view = found->second;
    if (!binding->scope_valid({view.root, view.epoch}, false)) { binding->error("scope_ended", -1, "Scene tree entry has ended"); return; }
    auto owner = binding->objects.find(view.root);
    if (owner == binding->objects.end() || owner->second.structure_epoch != view.structure) { binding->error("object_invalid", -1, "Scene structure changed; resolve a new view"); return; }
    const uint32_t index = uint32_t(field);
    if (index >= view.nodes.size()) { binding->error("invalid_argument", -1, "Scene field index mismatch"); return; }
    const ObjectID id = view.nodes[index];
    if (id == ObjectID()) { p_args.GetReturnValue().Set(v8::Null(binding->isolate)); return; }
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(id));
    if (!node) { binding->error("object_invalid", -1, "Scene node lifetime has ended"); return; }
    if (!binding->owns(node)) { binding->error("permission_denied", -1, "Scene node owner changed"); return; }
    p_args.GetReturnValue().Set(binding->objects.at(binding->token_of(node)).wrapper.Get(binding->isolate));
}
