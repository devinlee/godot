#include "web_game.h"
#include <emscripten.h>
#include "web_support.generated.h"
#include <cmath>

extern "C" int meton_web_callback(int token, const char *method, double value);

namespace {
Dictionary ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
/** 上下文与作业身份不能通过数值截断指向另一条记录。 */
bool token(const Variant &p_value, uint32_t &r_value, bool p_zero = false) {
    if ((p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) || !std::isfinite(double(p_value)) ||
            double(p_value) < (p_zero ? 0 : 1) || double(p_value) > UINT32_MAX || double(p_value) != uint32_t(double(p_value))) return false;
    r_value = uint32_t(double(p_value)); return true;
}
}

/** 签发时保留原生当前上下文，旧续体创建的新 Promise 仍不能取得新代次。 */
Dictionary MetonWebGame::capture_continuation(Stamp p_stamp, bool p_module) {
    if (!next_continuation) return failure("budget_exceeded");
    const uint32_t id = next_continuation++;
    continuations[id] = {p_stamp, p_module};
    Dictionary value; value["id"] = int64_t(id); value["owner"] = int64_t(p_stamp.owner);
    return ok(value);
}
Dictionary MetonWebGame::async_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    if (op == "async_fault") { revoked = true; return ok(); }
    if (op == "async_capture") {
        if (!execution_depth) return failure("scope_ended");
        return capture_continuation(execution_stamp, phase == MetonGameSupport::P_MODULE);
    }
    uint32_t id;
    if (!token(p_request.get("context", Variant()), id, op == "async_run")) return failure("invalid_argument");
    if (op == "async_release") { continuations.erase(id); return ok(); }
    if (op != "async_run") return failure("unsupported_member");
    uint32_t job;
    if (!token(p_request.get("job", Variant()), job)) return failure("invalid_argument");
    // 仅原生检查点允许恢复。普通 JS 即使拿到旧编号，也不能在同步信号内插入作业。
    if (!async_checkpoint || execution_depth || !active_native_objects.empty()) return failure("phase_conflict");
    auto context = continuations.find(id);
    if (id && context == continuations.end()) return failure("scope_ended");
    const Stamp previous = execution_stamp;
    const uint32_t previous_phase = phase;
    execution_stamp = id ? context->second.stamp : Stamp{};
    phase = !id || context->second.module ? MetonGameSupport::P_MODULE : MetonGameSupport::P_SAFE_POINT;
    ++execution_depth;
    const bool success = meton_web_callback(job, "_async_job", 0);
    --execution_depth;
    execution_stamp = previous; phase = previous_phase;
    if (!success) revoked = true;
    return success ? ok() : failure("scope_ended");
}
/** 原生通知栈只有游戏自身，用户同步回调已经退栈，才能开始有界微任务。 */
void MetonWebGame::checkpoint() {
    if (revoked || tearing_down || async_checkpoint || execution_depth || !active_native_objects.empty()) return;
    MetonNativeMemoryScope memory_scope(native_memory);
    if (!content.gpu_healthy()) {
        revoked = true; last_fault["code"] = "dependency_failed"; last_fault["operation"] = "gpu_retirement"; last_fault["committed"] = false;
        EM_ASM({ globalThis.metonC2TextureFault(); });
        return;
    }
    async_checkpoint = true;
    handle_event(0, "_safe_point");
    async_checkpoint = false;
}
