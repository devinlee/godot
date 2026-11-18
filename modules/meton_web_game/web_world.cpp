#include "web_game.h"
#include "web_support.generated.h"
#include "core/config/engine.h"
#include "core/io/json.h"
#include "scene/2d/physics/character_body_2d.h"
#include <emscripten.h>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
Dictionary world_ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
}

/** 私有 ABI 不接项目对象、字段名或 JSON 输入；原生再次复验执行栈和所选实际节点。 */
extern "C" EMSCRIPTEN_KEEPALIVE const char *meton_web_world_request(int p_operation, double p_x, double p_y) {
    static CharString output;
    Dictionary result;
    if (MetonWebGame::current) result = MetonWebGame::current->world_call(p_operation, p_x, p_y);
    else { result["ok"] = false; result["code"] = "scope_ended"; }
    output = JSON::stringify(result).utf8();
    return output.get_data();
}

EM_JS(int, meton_web_world_attach_private, (), {
    try {
        globalThis.metonE1WorldNativeAttach({
            request: function(operation, x, y) {
                if (x === undefined) x = 0;
                if (y === undefined) y = 0;
                if (operation !== 0 && operation !== 1 && operation !== 2 && operation !== 3 || typeof x !== 'number' || typeof y !== 'number') return {ok: false, code: 'invalid_argument'};
                return JSON.parse(UTF8ToString(_meton_web_world_request(operation, x, y)));
            }
        });
        return 1;
    } catch (error) { globalThis.metonC1Fault(error); return 0; }
});
EM_JS(char *, meton_web_world_step_private, (), {
    try { return stringToNewUTF8(JSON.stringify(globalThis.metonE1WorldStep())); }
    catch (error) { globalThis.metonC1Fault(error); return stringToNewUTF8('{"status":"rejected","code":"handler_failed"}'); }
});
EM_JS(void, meton_web_world_failed_private, (const char *code), {
    globalThis.metonC1Fault({code: UTF8ToString(code), message: 'World step sealed; committed effects are retained'});
});

/** 安装发生在普通脚本构造前，浏览器端须将端口保存在私有装配闭包。 */
bool MetonWebGame::attach_world_port() { return meton_web_world_attach_private() != 0; }

/** 已登记且未挂树的所属物体才能交接；空阶段集拒绝所有普通位置/速度写入。 */
bool MetonWebGame::configure_world(Node *p_node) {
    if (revoked || world_token || !Object::cast_to<CharacterBody2D>(p_node) || p_node->is_inside_tree()) return false;
    const uint32_t token = token_of(p_node);
    auto entry = objects.find(token);
    if (!token || entry == objects.end() || !entry->second.owned || entry->second.retired || entry->second.closing) return false;
    PackedStringArray fields;
    fields.push_back("Node2D.position"); fields.push_back("CharacterBody2D.velocity");
    if (!claim_fields(p_node, fields, "runtime.world.physics", PackedStringArray())) return false;
    world_token = token;
    return true;
}

/** 不缓存原生指针；对象释放、离树或字段撤回立即让旧端口失效。 */
CharacterBody2D *MetonWebGame::world_body(bool p_active) const {
    if (revoked || !world_token) return nullptr;
    auto entry = objects.find(world_token);
    if (entry == objects.end() || entry->second.retired || entry->second.closing || !entry->second.owned || (p_active && !entry->second.active)) return nullptr;
    auto *body = Object::cast_to<CharacterBody2D>(ObjectDB::get_instance(entry->second.object));
    if (!body || (p_active && !body->is_inside_tree()) || body->is_queued_for_deletion()) return nullptr;
    auto fields = field_owners.find(entry->second.object);
    if (fields == field_owners.end()) return nullptr;
    for (const String &name : {String("Node2D.position"), String("CharacterBody2D.velocity")}) {
        auto rule = fields->second.find(name);
        if (rule == fields->second.end() || rule->second.owner != "runtime.world.physics" || rule->second.phases != 0) return nullptr;
    }
    return body;
}

/** 只有固定 GDScript 的本次世界步骤可开启私有阶段，普通物理回调仍无此权。 */
bool MetonWebGame::world_physics_phase() const {
    return world_stepping && phase == MetonGameSupport::P_PHYSICS && Engine::get_singleton()->is_in_physics_frame();
}

/** 挂树触发原生命周期；失效不复活，重复激活也不复用旧上下文。 */
Dictionary MetonWebGame::activate_world() {
    if (revoked || !world_token || world_step_frame != UINT64_MAX) return failure("scope_ended");
    auto *body = world_body(false);
    if (!body || body->get_parent() || body->is_inside_tree()) return failure("phase_conflict");
    add_child(body);
    return world_body() ? world_ok() : failure("scope_ended");
}

/** 不给普通调用开任意移动入口，只有本步骤能把业务意图交给原物理拥有者。 */
Dictionary MetonWebGame::world_call(int p_operation, double p_x, double p_y) {
    MetonNativeMemoryScope memory_scope(native_memory);
    auto *body = world_body();
    if (p_operation == 0) return world_ok(body != nullptr);
    if (p_operation == 1) return world_ok(body && world_physics_phase() ? "physics" : "unavailable");
    if (!body) return failure("scope_ended");
    if (!world_physics_phase()) return failure("phase_conflict");
    auto *engine = Engine::get_singleton();
    if (process_frame != engine->get_process_frames() || physics_frame != engine->get_physics_frames()) {
        process_frame = engine->get_process_frames(); physics_frame = engine->get_physics_frames(); calls = 0;
    }
    if (++calls > MetonGameSupport::MAX_CALLS_PER_ENTRY) return failure("budget_exceeded");
    if (p_operation == 2) {
        if (!std::isfinite(p_x) || !std::isfinite(p_y) || std::abs(p_x) > std::numeric_limits<real_t>::max() || std::abs(p_y) > std::numeric_limits<real_t>::max()) return failure("invalid_argument");
        const uint64_t frame = engine->get_physics_frames();
        if (world_applied_frame == frame) return failure("phase_conflict");
        // 先登记已进入原生物理，信号重入和返回失败都不能让同帧意图再应用一次。
        world_applied_frame = frame;
        active_native_objects.push_back(body->get_instance_id());
        body->set_velocity(Vector2(p_x, p_y));
        body->move_and_slide();
        active_native_objects.pop_back();
        return world_body() ? world_ok() : failure("scope_ended");
    }
    if (p_operation == 3) {
        Dictionary value;
        const Vector2 position = body->get_position();
        value["x"] = position.x; value["y"] = position.y; value["on_floor"] = body->is_on_floor();
        return world_ok(value);
    }
    return failure("unsupported_member");
}

/** 每帧最多一次且保持原执行上下文；Signal 中的新输入由 R03 排到下一步骤。 */
Dictionary MetonWebGame::world_step() {
    auto *body = world_body();
    auto *engine = Engine::get_singleton();
    if (!body) return failure("scope_ended");
    if (world_stepping || execution_depth || !engine->is_in_physics_frame() || world_step_frame == engine->get_physics_frames()) return failure("phase_conflict");
    MetonNativeMemoryScope memory_scope(native_memory);
    const uint32_t previous_phase = phase;
    const Stamp previous_stamp = execution_stamp;
    world_stepping = true; world_step_frame = engine->get_physics_frames();
    phase = MetonGameSupport::P_PHYSICS; execution_stamp = {world_token, objects.at(world_token).epoch};
    active_native_objects.push_back(body->get_instance_id());
    ++execution_depth;
    char *json = meton_web_world_step_private();
    --execution_depth;
    active_native_objects.pop_back(); execution_stamp = previous_stamp; phase = previous_phase; world_stepping = false;
    const Variant parsed = JSON::parse_string(String::utf8(json)); free(json);
    Dictionary result = parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
    if (revoked || result.get("status", "") != Variant("completed")) {
        revoked = true; last_fault["code"] = result.get("code", "handler_failed"); last_fault["operation"] = "world_step";
        // 观察保留已提交/已发布的两个水位，绝不写一个虚假的 committed=false。
        last_fault["world_observation"] = result.get("observation", Variant());
        meton_web_world_failed_private(String(last_fault["code"]).utf8().get_data());
    }
    return result;
}
