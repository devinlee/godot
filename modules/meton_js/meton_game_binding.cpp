#include "meton_game_binding.h"
#include "meton_game_ui.h"
#include "meton_game_ui.generated.h"
#include "meton_game_impact.generated.h"
#include "meton_game_counts.generated.h"
#include "meton_game_textures.generated.h"
#include "meton_game_memory.generated.h"
#include "meton_native_game_profile.h"
#include "meton_world_inspector.generated.h"
#include "meton_game_support.generated.h"
#include "meton_game_identity.generated.h"
#include "meton_game_lifecycle.generated.h"
#include "meton_game_objects.generated.h"
#include "meton_game_resources.generated.h"
#include "meton_game_scenes.generated.h"
#include "meton_game_content.h"
#include "meton_game_script.h"
#include "meton_js_runtime.h"
#include "core/config/engine.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "core/os/thread.h"
#include "scene/2d/node_2d.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/main/node.h"
#include <cmath>
#include <limits>
#include <vector>

using namespace MetonGameSupport;
static constexpr v8::ExternalPointerTypeTag NODE_TAG = 1;
static constexpr v8::ExternalPointerTypeTag VECTOR_TAG = 2;
static_assert(sizeof(real_t) == 4, "B1 当前支持身份只接受单精度 Godot");

v8::Local<v8::String> MetonGameBinding::text(const String &p_text) const {
    const CharString bytes = p_text.utf8();
    return v8::String::NewFromUtf8(isolate, bytes.get_data(), v8::NewStringType::kNormal, bytes.length()).ToLocalChecked();
}
String MetonGameBinding::string(v8::Local<v8::Value> p_value) const {
    v8::String::Utf8Value bytes(isolate, p_value);
    return *bytes ? String::utf8(*bytes, bytes.length()) : String();
}
MetonGameBinding *MetonGameBinding::self(v8::Isolate *p_isolate) {
    return static_cast<MetonGameBinding *>(p_isolate->GetData(1));
}
void MetonGameBinding::error(const char *p_code, int p_member, const char *p_message) const {
    auto ctx = isolate->GetCurrentContext();
    auto failure = v8::Exception::TypeError(text(String(p_code) + ": " + p_message)).As<v8::Object>();
    (void)failure->CreateDataProperty(ctx, text("code"), text(p_code)).FromMaybe(false);
    (void)failure->CreateDataProperty(ctx, text("stage"), text("binding")).FromMaybe(false);
    if (p_member >= 0 && p_member < MEMBER_COUNT) (void)failure->CreateDataProperty(ctx, text("member"), text(MEMBERS[p_member].id)).FromMaybe(false);
    isolate->ThrowException(failure);
}
bool MetonGameBinding::allow(int p_member) {
    if (closed || revoked || !active_stamp() || !Thread::is_main_thread()) { error("scope_ended", p_member, "Game execution is not active on its owner thread"); return false; }
    if (p_member < 0 || p_member >= MEMBER_COUNT) { error("unsupported_member", p_member, "Constructor or member is not supported"); return false; }
    if (!(MEMBERS[p_member].phases & phase)) { error("phase_conflict", p_member, "Member is unavailable in this phase"); return false; }
    return charge_call(p_member);
}
bool MetonGameBinding::active_stamp() const {
    if (!current.owner) return true;
    auto found = objects.find(current.owner);
    return found != objects.end() && found->second.in_tree && found->second.epoch == current.epoch;
}
bool MetonGameBinding::charge_call(int p_member) {
    auto *engine = Engine::get_singleton();
    if (process_frame != engine->get_process_frames() || physics_frame != engine->get_physics_frames()) {
        process_frame = engine->get_process_frames(); physics_frame = engine->get_physics_frames(); calls = 0;
    }
    // 同一帧内跨回调和信号重入不刷新额度；只有真实引擎帧推进才能重新取得预算。
    if (++calls > MAX_CALLS_PER_ENTRY) { error("budget_exceeded", p_member, "Native frame call budget exceeded"); return false; }
    return true;
}
bool MetonGameBinding::number(v8::Local<v8::Value> p_value, real_t &r_value, int p_member) {
    if (!p_value->IsNumber()) { error("invalid_argument", p_member, "Expected a finite number"); return false; }
    const double value = p_value.As<v8::Number>()->Value();
    if (!std::isfinite(value)) { error("invalid_argument", p_member, "Expected a finite number"); return false; }
    if (std::abs(value) > std::numeric_limits<real_t>::max()) { error("out_of_range", p_member, "Value exceeds native float range"); return false; }
    r_value = static_cast<real_t>(value);
    return true;
}
bool MetonGameBinding::vector(v8::Local<v8::Value> p_value, Vector2 &r_value, int p_member) {
    if (!p_value->IsObject()) { error("invalid_argument", p_member, "Expected a bound Vector2"); return false; }
    auto object = p_value.As<v8::Object>();
    // 原型/结构类型不能伪造本绑定的内部标记，分量也不从用户 getter 中读取。
    if (object->InternalFieldCount() != 3 || !object->GetInternalField(0)->IsValue() || !object->GetInternalField(0).As<v8::Value>()->IsExternal() || object->GetInternalField(0).As<v8::External>()->Value(VECTOR_TAG) != &vector_brand) {
        error("invalid_argument", p_member, "Vector2 identity mismatch"); return false;
    }
    return number(object->GetInternalField(1).As<v8::Value>(), r_value.x, p_member) && number(object->GetInternalField(2).As<v8::Value>(), r_value.y, p_member);
}
v8::MaybeLocal<v8::Object> MetonGameBinding::vector(const Vector2 &p_value) {
    auto ctx = isolate->GetCurrentContext();
    v8::Local<v8::Function> constructor;
    if (!vector_template.Get(isolate)->GetFunction(ctx).ToLocal(&constructor)) return {};
    v8::Local<v8::Value> args[] = {v8::Number::New(isolate, p_value.x), v8::Number::New(isolate, p_value.y)};
    return constructor->NewInstance(ctx, 2, args);
}
Node *MetonGameBinding::node(v8::Local<v8::Object> p_wrapper, int p_member) {
    if (p_wrapper->InternalFieldCount() != 2 || !p_wrapper->GetInternalField(0)->IsValue() || !p_wrapper->GetInternalField(0).As<v8::Value>()->IsExternal() || p_wrapper->GetInternalField(0).As<v8::External>()->Value(NODE_TAG) != this || !p_wrapper->GetInternalField(1)->IsValue() || !p_wrapper->GetInternalField(1).As<v8::Value>()->IsUint32()) {
        error("invalid_argument", p_member, "Expected a bound game object"); return nullptr;
    }
    auto found = objects.find(p_wrapper->GetInternalField(1).As<v8::Uint32>()->Value());
    Node *object = found == objects.end() ? nullptr : Object::cast_to<Node>(ObjectDB::get_instance(found->second.native));
    if (!object || object->is_queued_for_deletion()) { error("object_invalid", p_member, "Object lifetime has ended"); return nullptr; }
    if (!owns(object)) { error("permission_denied", p_member, "Object belongs to another native owner"); return nullptr; }
    if (p_member >= 0 && !object->is_class(MEMBERS[p_member].owner)) { error("invalid_argument", p_member, "Receiver class mismatch"); return nullptr; }
    return object;
}
void MetonGameBinding::construct(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int id = p_args.DataV2().As<v8::Int32>()->Value();
    if (!binding->allow(id)) return;
    if (!p_args.IsConstructCall() || p_args.Length() != 0) { binding->error("invalid_argument", id, "Native node constructor requires new and no arguments"); return; }
    if (binding->live_objects() + binding->private_ui_objects() >= MAX_OBJECTS || !binding->next_object) { binding->error("budget_exceeded", id, "Native object budget exceeded"); return; }
    auto *object = Object::cast_to<Node>(ClassDB::instantiate(MEMBERS[id].owner));
    if (!object) { binding->error("unsupported_member", id, "Native class is unavailable"); return; }
    const uint32_t token = binding->next_object++;
    auto &entry = binding->objects[token];
    entry.native = object->get_instance_id();
    entry.creator = binding->current;
    object_owners[entry.native] = binding;
    entry.wrapper.Reset(binding->isolate, p_args.This());
    p_args.This()->SetInternalField(0, v8::External::New(binding->isolate, binding, NODE_TAG));
    p_args.This()->SetInternalField(1, v8::Uint32::New(binding->isolate, token));
    if (!binding->capture_callbacks(token)) {
        object_owners.erase(entry.native); entry.wrapper.Reset(); binding->objects.erase(token); memdelete(object); return;
    }
    object->set_script_instance(memnew(MetonGameScriptInstance(binding, token, object)));
    p_args.GetReturnValue().Set(p_args.This());
}
void MetonGameBinding::vector_construct(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int id = p_args.Length() == 0 ? M_VECTOR2_NEW_ZERO : M_VECTOR2_NEW_XY;
    if (!binding->allow(id)) return;
    if (!p_args.IsConstructCall() || (p_args.Length() != 0 && p_args.Length() != 2)) { binding->error("invalid_argument", id, "Vector2 expects zero or two numeric arguments"); return; }
    Vector2 value;
    if (p_args.Length() && (!binding->number(p_args[0], value.x, id) || !binding->number(p_args[1], value.y, id))) return;
    p_args.This()->SetInternalField(0, v8::External::New(binding->isolate, &binding->vector_brand, VECTOR_TAG));
    p_args.This()->SetInternalField(1, v8::Number::New(binding->isolate, value.x));
    p_args.This()->SetInternalField(2, v8::Number::New(binding->isolate, value.y));
    if (!p_args.This()->SetIntegrityLevel(binding->isolate->GetCurrentContext(), v8::IntegrityLevel::kFrozen).FromMaybe(false)) return;
    p_args.GetReturnValue().Set(p_args.This());
}
void MetonGameBinding::readonly_component(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    self(p_args.GetIsolate())->error("invalid_argument", p_args.DataV2().As<v8::Int32>()->Value(), "Vector2 components are readonly copies; assign a whole value");
}
void MetonGameBinding::dispatch(const v8::FunctionCallbackInfo<v8::Value> &p_args) {
    auto *binding = self(p_args.GetIsolate());
    const int id = p_args.DataV2().As<v8::Int32>()->Value();
    if (!binding->allow(id)) return;
    if (p_args.Length() != MEMBERS[id].arguments) { binding->error("invalid_argument", id, "Argument count mismatch"); return; }
    if (id == M_VECTOR2_X || id == M_VECTOR2_Y) {
        Vector2 value;
        if (binding->vector(p_args.This(), value, id)) p_args.GetReturnValue().Set(id == M_VECTOR2_X ? value.x : value.y);
        return;
    }
    if (id == M_INPUT_GET_AXIS) {
        if (!p_args[0]->IsString() || !p_args[1]->IsString()) { binding->error("invalid_argument", id, "Input action must be a string"); return; }
        const String negative = binding->string(p_args[0]), positive = binding->string(p_args[1]);
        if (!binding->actions.count(negative) || !binding->actions.count(positive) || !InputMap::get_singleton()->has_action(negative) || !InputMap::get_singleton()->has_action(positive)) {
            binding->error("permission_denied", id, "Input action is not admitted for this game"); return;
        }
        p_args.GetReturnValue().Set(Input::get_singleton()->get_axis(negative, positive));
        return;
    }
    Node *object = binding->node(p_args.This(), id);
    if (!object) return;
    switch (id) {
        case M_NODE2D_POSITION_GET:
        case M_CHARACTERBODY2D_VELOCITY_GET: {
            const Vector2 value = id == M_NODE2D_POSITION_GET ? Object::cast_to<Node2D>(object)->get_position() : Object::cast_to<CharacterBody2D>(object)->get_velocity();
            v8::Local<v8::Object> copy;
            if (binding->vector(value).ToLocal(&copy)) p_args.GetReturnValue().Set(copy);
            return;
        }
        case M_NODE2D_POSITION_SET:
        case M_CHARACTERBODY2D_VELOCITY_SET: {
            Vector2 value;
            if (!binding->vector(p_args[0], value, id)) return;
            if (!binding->guard_impact(object, id == M_NODE2D_POSITION_SET ? POSITION : VELOCITY)) return;
            if (id == M_NODE2D_POSITION_SET) Object::cast_to<Node2D>(object)->set_position(value);
            else Object::cast_to<CharacterBody2D>(object)->set_velocity(value);
            return;
        }
        case M_CANVASITEM_VISIBLE_GET:
            p_args.GetReturnValue().Set(Object::cast_to<CanvasItem>(object)->is_visible()); return;
        case M_CANVASITEM_VISIBLE_SET:
            if (!p_args[0]->IsBoolean()) { binding->error("invalid_argument", id, "Visible expects a boolean"); return; }
            if (!binding->guard_impact(object, VISIBLE)) return;
            Object::cast_to<CanvasItem>(object)->set_visible(p_args[0].As<v8::Boolean>()->Value()); return;
        case M_CHARACTERBODY2D_MOVE_AND_SLIDE:
        case M_CHARACTERBODY2D_IS_ON_FLOOR:
            if (!object->is_inside_tree()) { binding->error("phase_conflict", id, "Physics object has not entered its game tree"); return; }
            if (id == M_CHARACTERBODY2D_MOVE_AND_SLIDE && !binding->guard_impact(object, POSITION | VELOCITY)) return;
            p_args.GetReturnValue().Set(id == M_CHARACTERBODY2D_MOVE_AND_SLIDE ? Object::cast_to<CharacterBody2D>(object)->move_and_slide() : Object::cast_to<CharacterBody2D>(object)->is_on_floor()); return;
        default:
            binding->error("unsupported_member", id, "No native dispatcher exists"); return;
    }
}
bool MetonGameBinding::exports(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module) {
    auto exported = [&](const String &name, v8::Local<v8::Value> value) { return p_module->SetSyntheticModuleExport(isolate, text(name), value).FromMaybe(false); };
    auto callback = [&](v8::FunctionCallback function, int id) { return v8::FunctionTemplate::New(isolate, function, v8::Int32::New(isolate, id)); };
    for (const auto &type : CLASSES) {
        auto definition = callback(construct, type.constructor);
        definition->SetClassName(text(type.name));
        definition->InstanceTemplate()->SetInternalFieldCount(2);
        if (String(type.base) != String()) definition->Inherit(classes.at(type.base).Get(isolate));
        classes[type.name].Reset(isolate, definition);
    }
    if (!signal_exports(p_context, p_module)) return false;
    if (!object_exports(p_context)) return false;
    if (!impact_exports(p_context)) return false;
    if (!resource_exports(p_context, p_module)) return false;
    for (int id = 0; id < MEMBER_COUNT; ++id) {
        const auto &member = MEMBERS[id];
        if (!classes.count(member.owner)) continue;
        auto prototype = classes.at(member.owner).Get(isolate)->PrototypeTemplate();
        if (String(member.kind) == "property_get") {
            const String name = String(member.id).get_slicec('.', 1);
            const String set_id = String(member.owner) + "." + name + ".set";
            int setter = -1;
            for (int candidate = 0; candidate < MEMBER_COUNT; ++candidate) if (String(MEMBERS[candidate].id) == set_id) setter = candidate;
            prototype->SetAccessorProperty(text(name), callback(dispatch, id), setter < 0 ? v8::Local<v8::FunctionTemplate>() : callback(dispatch, setter));
        } else if (String(member.kind) == "method") {
            prototype->Set(text(String(member.id).get_slicec('.', 1)), callback(dispatch, id));
        }
    }
    for (const auto &type : CLASSES) {
        v8::Local<v8::Function> constructor;
        if (!classes.at(type.name).Get(isolate)->GetFunction(p_context).ToLocal(&constructor) || !exported(type.name, constructor)) return false;
    }
    auto vector_class = v8::FunctionTemplate::New(isolate, vector_construct);
    vector_class->SetClassName(text("Vector2"));
    vector_class->InstanceTemplate()->SetInternalFieldCount(3);
    for (const auto &part : {std::pair<const char *, int>{"x", M_VECTOR2_X}, {"y", M_VECTOR2_Y}}) {
        vector_class->PrototypeTemplate()->SetAccessorProperty(text(part.first), callback(dispatch, part.second), callback(readonly_component, part.second));
    }
    vector_template.Reset(isolate, vector_class);
    v8::Local<v8::Function> vector_constructor, input_axis;
    if (!vector_class->GetFunction(p_context).ToLocal(&vector_constructor) || !exported("Vector2", vector_constructor)) return false;
    auto input = v8::Object::New(isolate);
    if (!v8::Function::New(p_context, dispatch, v8::Int32::New(isolate, M_INPUT_GET_AXIS)).ToLocal(&input_axis) ||
        !input->CreateDataProperty(p_context, text("get_axis"), input_axis).FromMaybe(false) ||
        !input->SetIntegrityLevel(p_context, v8::IntegrityLevel::kFrozen).FromMaybe(false) || !exported("Input", input)) return false;
    auto variant = v8::Object::New(isolate), types = v8::Object::New(isolate);
    if (!types->CreateDataProperty(p_context, text("TYPE_FLOAT"), v8::Int32::New(isolate, Variant::FLOAT)).FromMaybe(false) ||
        !types->SetIntegrityLevel(p_context, v8::IntegrityLevel::kFrozen).FromMaybe(false) ||
        !variant->CreateDataProperty(p_context, text("Type"), types).FromMaybe(false) ||
        !variant->SetIntegrityLevel(p_context, v8::IntegrityLevel::kFrozen).FromMaybe(false) || !exported("Variant", variant)) return false;
    return true;
}
v8::MaybeLocal<v8::Promise> MetonGameBinding::evaluate(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module) {
    auto *binding = self(v8::Isolate::GetCurrent());
    if (!binding->exports(p_context, p_module)) return {};
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(p_context).ToLocal(&resolver) || !resolver->Resolve(p_context, v8::Undefined(binding->isolate)).FromMaybe(false)) return {};
    return resolver->GetPromise();
}
v8::Local<v8::Module> MetonGameBinding::module() {
    std::vector<v8::Local<v8::String>> names;
    for (const auto &type : CLASSES) names.push_back(text(type.name));
    for (const char *name : {"Vector2", "Input", "Variant", "Callable", "Signal"}) names.push_back(text(name));
    for (const auto &type : MetonResourceSupport::CLASSES) names.push_back(text(type.name));
    names.push_back(text("ResourceLoader"));
    return v8::Module::CreateSyntheticModule(isolate, text("godot"), names, evaluate);
}
Dictionary MetonGameBinding::support() {
    Dictionary result;
    result["format_version"] = 1;
    result["godot_revision"] = MetonGameRuntimeProfile::ENGINE_REVISION;
    result["v8_version"] = v8::V8::GetVersion();
    result["binding_digest"] = METON_GAME_BINDING_DIGEST;
    result["content_profile"] = "gw3-native-content-v1";
    result["description_digest"] = MetonMemorySupport::INPUT_DIGEST;
    result["runtime_profile_digest"] = MetonGameRuntimeProfile::INPUT_DIGEST;
    result["runtime_profile"] = MetonGameRuntimeProfile::PROFILE;
    result["gpu_pixels"] = int64_t(MetonGameRuntimeProfile::GPU_PIXELS);
    Dictionary content_limits;
    content_limits["files"] = MetonResourceSupport::CONTENT_FILES;
    content_limits["bytes"] = MetonResourceSupport::CONTENT_BYTES;
    content_limits["depth"] = MetonResourceSupport::CONTENT_DEPTH;
    content_limits["pixels"] = MetonResourceSupport::GPU_PIXELS;
    result["content_limits"] = content_limits;
    result["support_digest"] = MetonGameRuntimeProfile::TARGET_SUPPORT;
    result["world_support_digest"] = MetonWorldSupport::INPUT_DIGEST;
    result["ui_candidate"] = true;
    result["world_candidate"] = true;
    result["impact_candidate"] = true;
    result["counts_candidate"] = true;
    result["texture_payload_candidate"] = true;
    result["native_allocation_candidate"] = true;
    result["full_native_budget_verified"] = false;
    result["external_stop_verified"] = false;
    result["scenes_candidate"] = true;
    result["base_support_digest"] = SUPPORT_DIGEST;
    result["lifecycle_candidate"] = true;
    result["objects_candidate"] = true;
    result["resources_candidate"] = true;
    result["metadata_digest"] = METADATA_DIGEST;
    result["target"] = MetonGameRuntimeProfile::TARGET;
    result["precision_bytes"] = int(sizeof(real_t));
    result["runtime_verified"] = false;
    Array classes_report, members_report;
    bool valid = String(v8::V8::GetVersion()) == V8_VERSION && sizeof(real_t) == 4;
    for (const auto &type : CLASSES) {
        Dictionary row;
        const bool matched = ClassDB::class_exists(type.name) && ClassDB::get_parent_class(type.name) == StringName(type.base) && (type.constructor < 0 || ClassDB::can_instantiate(type.name));
        row["name"] = type.name; row["base"] = type.base; row["constructor"] = type.constructor >= 0; row["matched"] = matched;
        valid = valid && matched; classes_report.push_back(row);
    }
    for (const auto &member : MEMBERS) {
        Dictionary row;
        bool matched = true;
        if (String(member.method) != String()) {
            MethodBind *method = ClassDB::get_method(member.owner, member.method);
            matched = method && method->get_argument_count() == member.arguments && method->get_argument_type(-1) == member.result;
            if (matched) for (int i = 0; i < member.arguments; ++i) matched = matched && method->get_argument_type(i) == member.arguments_types[i];
        }
        row["id"] = member.id; row["matched"] = matched; row["phases"] = int(member.phases);
        members_report.push_back(row); valid = valid && matched;
    }
    for (const auto &member : MetonLifecycleSupport::MEMBERS) {
        Dictionary row;
        // 同步回调由真实 ScriptInstance 取证；信号本体必须存在于固定原生 Node 类。
        const bool matched = ClassDB::has_signal("Node", "tree_exiting");
        row["id"] = member.id; row["phases"] = int(member.phases); row["matched"] = matched;
        members_report.push_back(row); valid = valid && matched;
    }
    for (const auto &member : MetonObjectSupport::MEMBERS) {
        MethodBind *method = ClassDB::get_method("Node", member.name);
        bool matched = method && method->get_argument_count() == 1 && method->get_argument_type(0) == Variant::NODE_PATH && method->get_argument_type(-1) == Variant::OBJECT;
        Dictionary row; row["id"] = member.id; row["phases"] = int(member.phases); row["matched"] = matched;
        members_report.push_back(row); valid = valid && matched;
    }
    for (const auto &type : MetonResourceSupport::CLASSES) {
        bool matched = ClassDB::class_exists(type.name) && ClassDB::get_parent_class(type.name) == StringName(type.base);
        Dictionary row; row["name"] = type.name; row["base"] = type.base; row["constructor"] = false; row["matched"] = matched;
        classes_report.push_back(row); valid = valid && matched;
    }
    for (int i = 0; i < 3; ++i) {
        MethodBind *method = ClassDB::get_method(i == 2 ? "ResourceLoader" : "Sprite2D", i == 0 ? "get_texture" : i == 1 ? "set_texture" : "load");
        bool matched = method && method->get_argument_count() == (i == 2 ? 3 : i) && method->get_argument_type(-1) == (i == 1 ? Variant::NIL : Variant::OBJECT);
        Dictionary row; row["id"] = MetonResourceSupport::MEMBERS[i].id; row["phases"] = int(MetonResourceSupport::MEMBERS[i].phases); row["matched"] = matched;
        members_report.push_back(row); valid = valid && matched;
    }
    for (int i = 0; i < 2; ++i) {
        bool matched;
        if (!i) {
            MethodBind *method = ClassDB::get_method("Node", "queue_free");
            matched = method && method->get_argument_count() == 0 && method->get_argument_type(-1) == Variant::NIL;
        } else {
            Object probe;
            bool has_count = false;
            matched = probe.has_method("free") && probe.get_method_argument_count("free", &has_count) == 0 && has_count;
        }
        Dictionary row; row["id"] = MetonImpactSupport::MEMBERS[i].id; row["phases"] = int(MetonImpactSupport::MEMBERS[i].phases); row["matched"] = matched;
        members_report.push_back(row); valid = valid && matched;
    }
    result["classes"] = classes_report; result["members"] = members_report; result["classdb_matched"] = valid;
    return result;
}
bool MetonGameBinding::identity_matches(const Dictionary &p_identity) {
    Dictionary actual = support();
    if (!bool(actual["classdb_matched"])) return false;
    for (const char *key : {"godot_revision", "v8_version", "binding_digest", "support_digest", "metadata_digest", "target"}) {
        if (!p_identity.has(key) || p_identity[key].get_type() != Variant::STRING || p_identity[key] != actual[key]) return false;
    }
    return true;
}
bool MetonGameBinding::actions_valid(const PackedStringArray &p_actions) {
    if (p_actions.size() > 128) return false;
    std::set<String> distinct;
    for (const String &action : p_actions) {
        if (action.is_empty() || action.utf8().length() > 256 || !InputMap::get_singleton()->has_action(action) || !distinct.insert(action).second) return false;
    }
    return true;
}
MetonGameBinding::MetonGameBinding(const PackedStringArray &p_actions) {
    content = std::make_unique<MetonGameContent>();
    for (const String &action : p_actions) actions.insert(action);
}
void MetonGameBinding::initialize(v8::Isolate *p_isolate, MetonJSRuntime *p_runtime) { isolate = p_isolate; runtime = p_runtime; isolate->SetData(1, this); }
bool MetonGameBinding::set_phase(const String &p_phase, bool p_new_entry) {
    static const std::pair<const char *, uint32_t> phases[] = {{"module", P_MODULE}, {"construct", P_CONSTRUCT}, {"enter_tree", P_ENTER_TREE}, {"ready", P_READY}, {"process", P_PROCESS}, {"physics", P_PHYSICS}, {"signal", P_SIGNAL}, {"safe_point", P_SAFE_POINT}, {"cleanup", P_CLEANUP}};
    for (const auto &entry : phases) if (p_phase == entry.first) { phase = entry.second; return true; }
    return false;
}
Array MetonGameBinding::native_objects() const {
    Array result;
    for (const auto &entry : objects) {
        Object *object = ObjectDB::get_instance(entry.second.native);
        if (object) result.push_back(object);
    }
    return result;
}
bool MetonGameBinding::capture_callbacks(uint32_t p_token) {
    auto ctx = isolate->GetCurrentContext();
    auto &entry = objects.at(p_token);
    for (int index = 0; index < 5; ++index) {
        auto value = entry.wrapper.Get(isolate)->GetPrototype();
        unsigned depth = 0;
        while (value->IsObject()) {
            if (++depth > 64 || value->IsProxy()) { error("invalid_argument", -1, "Unsupported callback prototype chain"); return false; }
            auto prototype = value.As<v8::Object>();
            v8::Local<v8::Value> descriptor;
            if (!prototype->GetOwnPropertyDescriptor(ctx, text(MetonLifecycleSupport::CALLBACKS[index].name)).ToLocal(&descriptor)) return false;
            if (descriptor->IsObject()) {
                v8::Local<v8::Value> method;
                auto object = descriptor.As<v8::Object>();
                // 固定原型方法，不在 Godot has_method 等无预算查询中执行用户 getter。
                if (!object->HasOwnProperty(ctx, text("value")).FromMaybe(false) || object->HasOwnProperty(ctx, text("get")).FromMaybe(true) ||
                        object->HasOwnProperty(ctx, text("set")).FromMaybe(true)) {
                    error("invalid_argument", -1, "Lifecycle callback must be a data method"); return false;
                }
                if (!object->Get(ctx, text("value")).ToLocal(&method)) return false;
                if (!method->IsFunction() || method->IsAsyncFunction()) {
                    error("invalid_argument", -1, "Lifecycle callback must be a synchronous prototype method"); return false;
                }
                entry.callbacks[index].Reset(isolate, method.As<v8::Function>());
                break;
            }
            value = prototype->GetPrototype();
        }
    }
    return true;
}
bool MetonGameBinding::has_callback(uint32_t p_token, const StringName &p_method) const {
    if (closed || revoked) return false;
    auto found = objects.find(p_token);
    if (found == objects.end()) return false;
    for (int i = 0; i < 5; ++i) if (p_method == MetonLifecycleSupport::CALLBACKS[i].name) return !found->second.callbacks[i].IsEmpty();
    return false;
}
bool MetonGameBinding::validate_instance(uint32_t p_token) {
    auto found = objects.find(p_token);
    if (found == objects.end() || found->second.wrapper.IsEmpty()) return false;
    auto ctx = isolate->GetCurrentContext();
    for (const auto &description : MetonLifecycleSupport::CALLBACKS) {
        v8::Local<v8::Value> descriptor;
        if (!found->second.wrapper.Get(isolate)->GetOwnPropertyDescriptor(ctx, text(description.name)).ToLocal(&descriptor)) return false;
        // 类字段在 super 返回后才初始化，首次入树必须补查，不能静默忽略遮蔽原型的回调字段。
        if (!descriptor->IsUndefined()) { error("invalid_argument", -1, "Lifecycle callback fields cannot shadow admitted prototype methods"); return false; }
    }
    return true;
}
bool MetonGameBinding::dispatch_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count) {
    if (!has_callback(p_token, p_method)) return false;
    runtime->dispatch_game_callback(p_token, p_method, p_args, p_count);
    return true;
}
bool MetonGameBinding::call_callback(uint32_t p_token, const StringName &p_method, const Variant **p_args, int p_count) {
    auto found = objects.find(p_token);
    if (found == objects.end() || closed || revoked) return false;
    auto &entry = found->second;
    for (int i = 0; i < 5; ++i) if (p_method == MetonLifecycleSupport::CALLBACKS[i].name) {
        const auto &description = MetonLifecycleSupport::CALLBACKS[i];
        if (entry.callbacks[i].IsEmpty()) return true;
        if (p_count != description.arguments || (p_count && p_args[0]->get_type() != Variant::FLOAT)) {
            error("invalid_argument", -1, "Native lifecycle argument mismatch"); return false;
        }
        const Stamp previous = current;
        const uint32_t previous_phase = phase;
        current = {p_token, entry.epoch};
        set_phase(description.phase);
        auto ctx = isolate->GetCurrentContext();
        v8::Local<v8::Value> argument = p_count ? v8::Local<v8::Value>(v8::Number::New(isolate, double(*p_args[0]))) : v8::Local<v8::Value>(v8::Undefined(isolate));
        v8::Local<v8::Value> returned;
        active_native_objects.push_back(entry.native);
        bool ok = entry.callbacks[i].Get(isolate)->Call(ctx, entry.wrapper.Get(isolate), p_count, &argument).ToLocal(&returned);
        // TS void 可以接受异步函数；运行时仍核对 thenable，读取 then 同样受当前预算约束。
        if (ok && returned->IsObject()) {
            v8::Local<v8::Value> then;
            ok = returned.As<v8::Object>()->Get(ctx, text("then")).ToLocal(&then);
            if (ok && (returned->IsPromise() || then->IsFunction())) {
                error("invalid_argument", -1, "Synchronous lifecycle returned a thenable"); ok = false;
            }
        }
        active_native_objects.pop_back();
        current = previous; phase = previous_phase;
        return ok;
    }
    return false;
}
void MetonGameBinding::entered(uint32_t p_token) {
    if (closed || revoked) return;
    auto found = objects.find(p_token);
    if (found == objects.end()) return;
    ++found->second.epoch; found->second.in_tree = true; found->second.closing = false; found->second.scope.Reset();
    runtime->dispatch_game_callback(p_token, "_meton_validate", nullptr, 0);
}
void MetonGameBinding::closing(uint32_t p_token) {
    if (closed) return;
    auto found = objects.find(p_token);
    if (found != objects.end()) found->second.closing = true;
}
void MetonGameBinding::exited(uint32_t p_token) {
    if (closed) return;
    auto found = objects.find(p_token);
    if (found != objects.end()) {
        found->second.in_tree = false; found->second.closing = true; found->second.scope.Reset();
        runtime->cleanup_game_scope(p_token, false);
        release_created({p_token, found->second.epoch});
    }
}
void MetonGameBinding::object_destroyed(uint32_t p_token) {
    if (closed) return;
    auto found = objects.find(p_token);
    if (found == objects.end()) return;
    object_owners.erase(found->second.native);
    forget_impact(found->second.native);
    scene_roots.erase(found->second.native);
    found->second.in_tree = false; found->second.closing = true;
    runtime->cleanup_game_scope(p_token, true);
    found->second.wrapper.Reset();
    found->second.signal.Reset(); found->second.scope.Reset();
    for (auto &callback : found->second.callbacks) callback.Reset();
    // 数量额度立即归还，墓碑在原生通知栈退回后清理；单调 token 不会重新签发。
    found->second.retired = true;
}
Dictionary MetonGameBinding::lifecycle_state() const {
    Dictionary result;
    Array nodes;
    for (const auto &entry : objects) {
        Dictionary item;
        item["token"] = int64_t(entry.first);
        item["epoch"] = int64_t(entry.second.epoch);
        item["in_tree"] = entry.second.in_tree;
        item["closing"] = entry.second.closing;
        item["alive"] = ObjectDB::get_instance(entry.second.native) != nullptr;
        nodes.push_back(item);
    }
    result["live_objects"] = int(live_objects());
    result["private_ui"] = ui_state();
    result["native_object_slots"] = int(live_objects() + private_ui_objects());
    result["retired_objects"] = int(objects.size() - live_objects());
    result["nodes"] = nodes; result["revoked"] = revoked; result["calls_this_frame"] = int(calls);
    result["callables"] = int(callables.size()); result["connections"] = int(connections.size());
    int retained = 0, waits = 0;
    for (const auto &entry : callables) retained += !entry.second.retained.IsEmpty();
    for (const auto &entry : connections) waits += !entry.second.waiter.IsEmpty();
    result["retained_callables"] = retained; result["waits"] = waits;
    result["pending_reactions"] = int(reactions.size());
    result["impact"] = impact_state();
    return result;
}
void MetonGameBinding::release() {
    if (closed) return;
    clear_delete_requests();
    field_owners.clear();
    active_native_objects.clear(); deleting_native_roots.clear();
    closed = true;
    // 私有所有者先释放，不能由普通父节点清理把 UI 拆下后遗失原租约。
    if (ui) ui->close_all();
    world_token = 0;
    world_context.Reset(); world_record_prototype.Reset(); world_array_prototype.Reset();
    lifetime->binding = nullptr;
    for (auto &entry : connections) {
        auto callable = callables.find(entry.second.callable);
        if (callable != callables.end() && ObjectDB::get_instance(entry.second.signal.get_object_id()) && entry.second.signal.is_connected(callable->second.native))
            entry.second.signal.disconnect(callable->second.native);
        entry.second.waiter.Reset();
    }
    connections.clear(); callables.clear(); handles.clear();
    for (auto &entry : scene_views) entry.second.wrapper.Reset();
    scene_views.clear(); scene_roots.clear();
    reactions.clear(); continuation_stack.clear(); promise_stamp.Reset(); promise_ticket.Reset();
    if (isolate) isolate->SetData(1, nullptr);
    vector_template.Reset(); classes.clear();
    // 先清 JS 环，再按登记所有权释放。树上的父子关系不能授予删除其他拥有者的权力。
    for (auto &entry : objects) {
        entry.second.wrapper.Reset();
        entry.second.signal.Reset(); entry.second.scope.Reset();
        for (auto &callback : entry.second.callbacks) callback.Reset();
    }
    for (const auto &entry : objects) release_node(entry.first);
    for (auto &entry : resources) entry.second.wrapper.Reset();
    resources.clear(); resource_tokens.clear();
    content->drain_textures();
    resource_cleanup = content->state();
    content.reset();
    objects.clear(); isolate = nullptr;
}
MetonGameBinding::~MetonGameBinding() { release(); }
