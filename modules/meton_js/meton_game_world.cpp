#include "meton_game_binding.h"
#include "meton_game_support.generated.h"
#include "core/config/engine.h"
#include "scene/2d/physics/character_body_2d.h"
#define METON_WORLD_INSPECTOR_IMPLEMENTATION
#include "meton_world_inspector.generated.h"

/** 受信装配一次性交出已登记节点；空阶段集封锁全部普通绑定写入。 */
bool MetonGameBinding::configure_world(Node *p_node) {
    if (closed || revoked || world_token || !Object::cast_to<CharacterBody2D>(p_node) || p_node->is_inside_tree()) return false;
    const uint32_t token = token_of(p_node);
    auto entry = objects.find(token);
    if (!token || entry == objects.end() || !entry->second.owned || entry->second.retired || entry->second.closing) return false;
    PackedStringArray fields;
    fields.push_back("Node2D.position"); fields.push_back("CharacterBody2D.velocity");
    if (!claim_fields(p_node, fields, "runtime.world.physics", PackedStringArray())) return false;
    world_token = token;
    return true;
}

/** 逐次核对原对象、树代次和字段登记；受信装配撤回后不能继续用旧原生指针。 */
CharacterBody2D *MetonGameBinding::world_body() const {
    if (closed || revoked || !world_token) return nullptr;
    auto entry = objects.find(world_token);
    if (entry == objects.end() || entry->second.retired || entry->second.closing || !entry->second.owned || !entry->second.in_tree) return nullptr;
    auto *body = Object::cast_to<CharacterBody2D>(ObjectDB::get_instance(entry->second.native));
    if (!body || !body->is_inside_tree() || body->is_queued_for_deletion()) return nullptr;
    auto fields = field_owners.find(entry->second.native);
    if (fields == field_owners.end()) return nullptr;
    for (const String &name : {String("Node2D.position"), String("CharacterBody2D.velocity")}) {
        auto found = fields->second.find(name);
        if (found == fields->second.end() || found->second.owner != "runtime.world.physics" || found->second.phases != 0) return nullptr;
    }
    return body;
}

/** 除绑定的阶段外还核对引擎真实物理栈，伪造 invoke_game 阶段字符串不能移动对象。 */
bool MetonGameBinding::world_physics_phase() const {
    return phase == MetonGameSupport::P_PHYSICS && Engine::get_singleton()->is_in_physics_frame();
}

/** 私有端口只操作原生装配选定的物体，不接收项目提供的对象 ID 或字段名。 */
void MetonGameBinding::world_dispatch(const v8::FunctionCallbackInfo<v8::Value> &args) {
    auto *binding = self(args.GetIsolate());
    if (!binding) return;
    const int operation = args.Data().As<v8::Int32>()->Value();
    auto *body = binding->world_body();
    if (operation == 0) { args.GetReturnValue().Set(body != nullptr); return; }
    if (operation == 1) { args.GetReturnValue().Set(binding->text(body && binding->world_physics_phase() ? "physics" : "unavailable")); return; }
    if (!body) { binding->error("scope_ended", -1, "World physics owner is unavailable"); return; }
    if (!binding->world_physics_phase()) { binding->error("phase_conflict", -1, "World requires the actual physics step"); return; }
    if (!binding->charge_call(-1)) return;
    auto ctx = args.GetIsolate()->GetCurrentContext();
    if (operation == 2) {
        real_t x, y;
        if (args.Length() != 2) { binding->error("invalid_argument", -1, "World velocity requires two components"); return; }
        if (!binding->number(args[0], x, -1) || !binding->number(args[1], y, -1)) return;
        const uint64_t frame = Engine::get_singleton()->get_physics_frames();
        if (binding->world_applied_frame == frame) { binding->error("phase_conflict", -1, "World physics already applied in this frame"); return; }
        // 在跨入原生物理前封锁重复调用；同步信号也不能让同帧意图应用两次。
        binding->world_applied_frame = frame;
        binding->active_native_objects.push_back(body->get_instance_id());
        body->set_velocity(Vector2(x, y));
        body->move_and_slide();
        binding->active_native_objects.pop_back();
        if (!binding->world_body()) binding->error("scope_ended", -1, "World ended during native physics");
        return;
    }
    if (operation == 3) {
        const Vector2 position = body->get_position();
        auto observed = v8::Object::New(args.GetIsolate());
        if (!WorldInspection::field(ctx, observed, "x", v8::Number::New(args.GetIsolate(), position.x)) ||
            !WorldInspection::field(ctx, observed, "y", v8::Number::New(args.GetIsolate(), position.y)) ||
            !WorldInspection::field(ctx, observed, "on_floor", v8::Boolean::New(args.GetIsolate(), body->is_on_floor()))) return;
        args.GetReturnValue().Set(observed);
    }
}

/** 检查器在项目执行前捕获原型，用私有 realm 构造描述符，项目污染不能伪造数据字段。 */
v8::MaybeLocal<v8::Promise> MetonGameBinding::evaluate_world(v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
    auto *binding = self(v8::Isolate::GetCurrent());
    auto *isolate = binding->isolate;
    binding->world_record_prototype.Reset(isolate, v8::Object::New(isolate)->GetPrototype());
    binding->world_array_prototype.Reset(isolate, v8::Array::New(isolate)->GetPrototype());
    auto private_context = v8::Context::New(isolate);
    if (private_context.IsEmpty()) return {};
    private_context->AllowCodeGenerationFromStrings(false);
    binding->world_context.Reset(isolate, private_context);
    v8::Local<v8::Function> inspect;
    if (!v8::Function::New(private_context, inspect_world_value, {}, 2, v8::ConstructorBehavior::kThrow).ToLocal(&inspect) ||
        !module->SetSyntheticModuleExport(isolate, binding->text("inspect"), inspect).FromMaybe(false)) return {};
    const char *names[] = {"active", "phase", "apply", "sample"};
    for (int index = 0; index < 4; ++index) {
        v8::Local<v8::Function> function;
        if (!v8::Function::New(context, world_dispatch, v8::Integer::New(isolate, index), index == 2 ? 2 : 0, v8::ConstructorBehavior::kThrow).ToLocal(&function) ||
            !module->SetSyntheticModuleExport(isolate, binding->text(names[index]), function).FromMaybe(false)) return {};
    }
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) || !resolver->Resolve(context, v8::Undefined(isolate)).FromMaybe(false)) return {};
    return resolver->GetPromise();
}

v8::Local<v8::Module> MetonGameBinding::world_module() {
    std::vector<v8::Local<v8::String>> names;
    for (const char *name : {"inspect", "active", "phase", "apply", "sample"}) names.push_back(text(name));
    return v8::Module::CreateSyntheticModule(isolate, text("meton.internal.world"), names, evaluate_world);
}
