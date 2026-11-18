#include "meton_game_binding.h"
#include "meton_game_content.h"
#include "meton_game_resources.generated.h"
#include "meton_game_support.generated.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/texture.h"
#include "scene/2d/sprite_2d.h"

namespace {
constexpr v8::ExternalPointerTypeTag RESOURCE_TAG = 4;
}
/** 固定资源类只用于类型收窄；未列构造器不能借继承直接创建实例。 */
bool MetonGameBinding::resource_exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module) {
    for (const auto &type : MetonResourceSupport::CLASSES) {
        auto definition = v8::FunctionTemplate::New(isolate, construct, v8::Int32::New(isolate, -1));
        // 与 Node(2)、Vector2(3)、作用域句柄(5) 区分，避免错误参数触发 V8 External 标签检查而不是绑定拒绝。
        definition->SetClassName(text(type.name)); definition->InstanceTemplate()->SetInternalFieldCount(4);
        definition->Inherit(classes.at(type.base).Get(isolate));
        classes[type.name].Reset(isolate, definition);
        v8::Local<v8::Function> constructor;
        if (!definition->GetFunction(p_context).ToLocal(&constructor) || !p_module->SetSyntheticModuleExport(isolate, text(type.name), constructor).FromMaybe(false)) return false;
    }
    auto prototype = classes.at("Sprite2D").Get(isolate)->PrototypeTemplate();
    prototype->SetAccessorProperty(text("texture"), v8::FunctionTemplate::New(isolate, resource_dispatch, v8::Int32::New(isolate, 0)),
        v8::FunctionTemplate::New(isolate, resource_dispatch, v8::Int32::New(isolate, 1)));
    auto loader = v8::Object::New(isolate);
    v8::Local<v8::Function> load;
    return v8::Function::New(p_context, resource_dispatch, v8::Int32::New(isolate, 2)).ToLocal(&load) &&
        loader->CreateDataProperty(p_context, text("load"), load).FromMaybe(false) && loader->SetIntegrityLevel(p_context, v8::IntegrityLevel::kFrozen).FromMaybe(false) &&
        p_module->SetSyntheticModuleExport(isolate, text("ResourceLoader"), loader).FromMaybe(false);
}
/** GC 只标记待释放；原生引用在退出 GC 后的安全点清理。 */
void MetonGameBinding::weak_resource(const v8::WeakCallbackInfo<ResourceEntry> &p_info) {
    auto *entry = p_info.GetParameter(); entry->wrapper.Reset(); entry->retired = true;
}
/** 只放掉本方 Ref，不调用 memdelete，也不清除仍被其他原生拥有者持有的资源。 */
void MetonGameBinding::prune_resources() {
    for (auto entry = resources.begin(); entry != resources.end();) {
        if (!entry->second.retired) { ++entry; continue; }
        resource_tokens.erase(entry->second.retained->get_instance_id());
        entry = resources.erase(entry);
    }
}
/** 活跃包装复用同一 JS 身份；已经回收的包装不会被同名或新 ObjectID 复活。 */
v8::MaybeLocal<v8::Object> MetonGameBinding::wrap_resource(const Ref<Resource> &p_resource) {
    if (!content || !content->owns(p_resource)) { error("permission_denied", -1, "Resource did not originate in this game content"); return {}; }
    prune_resources();
    auto previous = resource_tokens.find(p_resource->get_instance_id());
    if (previous != resource_tokens.end()) return resources.at(previous->second).wrapper.Get(isolate);
    if (resources.size() >= MetonResourceSupport::NATIVE_RESOURCES || !next_resource) { error("budget_exceeded", -1, "Resource wrapper budget exceeded"); return {}; }
    String type = Object::cast_to<Texture2D>(p_resource.ptr()) ? "Texture2D" : "Resource";
    v8::Local<v8::Object> wrapper;
    if (!classes.at(type).Get(isolate)->InstanceTemplate()->NewInstance(isolate->GetCurrentContext()).ToLocal(&wrapper)) return {};
    const uint32_t token = next_resource++;
    auto &entry = resources[token]; entry.token = token; entry.retained = p_resource; entry.wrapper.Reset(isolate, wrapper);
    wrapper->SetInternalField(0, v8::External::New(isolate, this, RESOURCE_TAG));
    wrapper->SetInternalField(1, v8::Uint32::New(isolate, token));
    resource_tokens[p_resource->get_instance_id()] = token;
    entry.wrapper.SetWeak(&entry, weak_resource, v8::WeakCallbackType::kParameter);
    return wrapper;
}
/** 原型和字段形状都不是资源身份，必须找到本 VM 仍保有的原生引用。 */
Ref<Resource> MetonGameBinding::unwrap_resource(v8::Local<v8::Value> p_value) {
    if (!p_value->IsObject()) { error("invalid_argument", -1, "Expected a bound Resource or null"); return {}; }
    auto object = p_value.As<v8::Object>();
    if (object->InternalFieldCount() != 4 || !object->GetInternalField(0)->IsValue() || !object->GetInternalField(0).As<v8::Value>()->IsExternal() ||
        object->GetInternalField(0).As<v8::External>()->Value(RESOURCE_TAG) != this || !object->GetInternalField(1)->IsValue() || !object->GetInternalField(1).As<v8::Value>()->IsUint32()) {
        error("invalid_argument", -1, "Resource identity mismatch"); return {};
    }
    auto found = resources.find(object->GetInternalField(1).As<v8::Uint32>()->Value());
    if (found == resources.end() || found->second.retired || !content || !content->owns(found->second.retained)) { error("object_invalid", -1, "Resource lifetime has ended"); return {}; }
    return found->second.retained;
}
/** 资源加载保留同步/空值语义，类型与归属拒绝发生在派发前。 */
void MetonGameBinding::resource_dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int operation = p_args.DataV2().As<v8::Int32>()->Value();
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "Game scope has ended"); return; }
    if (!(MetonResourceSupport::MEMBERS[operation].phases & binding->phase)) { binding->error("phase_conflict", -1, "Resource operation is unavailable in this phase"); return; }
    if (!binding->charge_call(-1)) return;
    if (p_args.Length() != (operation ? 1 : 0)) { binding->error("invalid_argument", -1, "Resource operation argument count mismatch"); return; }
    Ref<Resource> resource;
    if (operation == 2) {
        if (!p_args[0]->IsString() || p_args[0].As<v8::String>()->Length() > 246) { binding->error("invalid_argument", -1, "Expected a fixed resource path or UID"); return; }
        v8::String::Value units(binding->isolate, p_args[0]);
        for (int i = 0; i < units.length(); ++i) if ((*units)[i] < 32 || (*units)[i] > 126) { binding->error("invalid_argument", -1, "Resource key is not canonical ASCII"); return; }
        const String name = binding->string(p_args[0]);
        if (!binding->content || binding->content->resolve(name).is_empty()) { binding->error("permission_denied", -1, "Resource is outside the fixed content closure"); return; }
        Error status = OK;
        resource = binding->content->load(name, &status);
        if (status == ERR_UNAVAILABLE) { binding->error("unsupported_target", -1, "Real texture backend is unavailable"); return; }
        if (status == ERR_OUT_OF_MEMORY) { binding->error("budget_exceeded", -1, "Native resource reservation exceeds the game limit"); return; }
        if (status == ERR_UNAUTHORIZED) { binding->error("permission_denied", -1, "Native resource remap is not admitted"); return; }
    } else {
        Node *node = binding->node(p_args.This(), -1);
        if (!node) return;
        auto *sprite = Object::cast_to<Sprite2D>(node);
        if (!sprite) { binding->error("invalid_argument", -1, "Texture receiver must be Sprite2D"); return; }
        if (operation == 1) {
            if (!binding->guard_impact(node, TEXTURE)) return;
            if (p_args[0]->IsNull()) { sprite->set_texture(Ref<Texture2D>()); return; }
            resource = binding->unwrap_resource(p_args[0]);
            if (resource.is_null()) return;
            Ref<Texture2D> texture = resource;
            if (texture.is_null()) { binding->error("invalid_argument", -1, "Texture expects Texture2D or null"); return; }
            sprite->set_texture(texture); return;
        }
        resource = sprite->get_texture();
    }
    if (resource.is_null()) { p_args.GetReturnValue().Set(v8::Null(binding->isolate)); return; }
    v8::Local<v8::Object> wrapper;
    if (binding->wrap_resource(resource).ToLocal(&wrapper)) p_args.GetReturnValue().Set(wrapper);
}
bool MetonGameBinding::install_content(const String &p_json, const String &p_digest, const String &p_version) {
    return !closed && !revoked && content && content->install(p_json, p_digest, p_version);
}
/** 场景只由宿主装配；游戏侧没有通用 instantiate、脚本加载或系统入口。 */
Node *MetonGameBinding::instantiate_content_scene(const String &p_path) {
    if (closed || revoked || !content) return nullptr;
    prune_objects();
    const size_t nodes = content->scene_nodes(p_path);
    if (!nodes || nodes + live_objects() + private_ui_objects() > MetonGameSupport::MAX_OBJECTS || !next_object || nodes > uint64_t(UINT32_MAX) - next_object + 1) return nullptr;
    Ref<PackedScene> scene = content->load(p_path);
    if (scene.is_null() || !content->reserve_scene(p_path)) return nullptr;
    Node *root = scene->instantiate();
    if (!root) return nullptr;
    if (!content->matches_scene(root, p_path)) { memdelete(root); return nullptr; }
    if (!adopt_tree(root, true)) { memdelete(root); return nullptr; }
    content->remember_scene(root);
    scene_roots[root->get_instance_id()] = content->resolve(p_path);
    return root;
}
Dictionary MetonGameBinding::content_state() const {
    Dictionary state = content ? content->state() : resource_cleanup.duplicate(true);
    state["wrappers"] = int(resources.size());
    int pending = 0; for (const auto &entry : resources) pending += entry.second.retired;
    state["pending_release"] = pending;
    return state;
}
