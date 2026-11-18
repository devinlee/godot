#include "web_game.h"
#include "web_support.generated.h"
#include <cmath>

namespace {
Dictionary ok(const Variant &p_value = Variant()) { Dictionary answer; answer["ok"] = true; answer["value"] = p_value; return answer; }
/** 句柄转换必须精确，错误数值不能截断成另一个存活对象。 */
bool handle(const Variant &p_value, uint32_t &r_token) {
    if ((p_value.get_type() != Variant::INT && p_value.get_type() != Variant::FLOAT) || !std::isfinite(double(p_value)) ||
            double(p_value) < 1 || double(p_value) > UINT32_MAX || double(p_value) != uint32_t(double(p_value))) return false;
    r_token = uint32_t(double(p_value)); return true;
}
constexpr size_t MAX_CONNECTIONS = 256;
}

/** 原生入口的当前调用身份不能被目标重新入树刷新。 */
bool MetonWebGame::active_stamp() const {
    if (!execution_stamp.owner) return true;
    auto owner = objects.find(execution_stamp.owner);
    return owner != objects.end() && !owner->second.retired && owner->second.active && owner->second.epoch == execution_stamp.epoch;
}
bool MetonWebGame::scope_valid(Stamp p_stamp, bool p_new_work) const {
    if (revoked) return false;
    auto owner = objects.find(p_stamp.owner);
    return owner != objects.end() && !owner->second.retired && owner->second.active && owner->second.epoch == p_stamp.epoch &&
            (!p_new_work || !owner->second.closing) && ObjectDB::get_instance(owner->second.object);
}
void MetonWebGame::release_capture(uint32_t p_callable) {
    for (const auto &entry : connections) if (entry.second.callable == p_callable) return;
    handle_event(p_callable, "_handle_release");
}
/** 先拆真实连接再释放 JS 保有；重复解绑不制造新动作。 */
void MetonWebGame::retire_connection(uint32_t p_connection, const char *p_reason) {
    auto found = connections.find(p_connection);
    if (found == connections.end()) return;
    const Connection connection = found->second;
    auto callable = callables.find(connection.callable);
    if (callable != callables.end()) {
        auto *source = ObjectDB::get_instance(connection.source);
        if (source && source->is_connected("tree_exiting", callable->second.native)) source->disconnect("tree_exiting", callable->second.native);
    }
    connections.erase(found);
    if (connection.waiter) {
        const String method = p_reason ? String("_wait_") + p_reason : String("_wait_signal");
        handle_event(p_connection, method.utf8().get_data());
    }
    release_capture(connection.callable);
}
/** ONE_SHOT 在调用监听器前从 Godot 移除，须等原生回调退栈才核对，不能提前丢掉闭包。 */
void MetonWebGame::prune_connections() {
    if (!active_native_objects.empty() || tearing_down) return;
    std::vector<uint32_t> retired;
    for (const auto &entry : connections) {
        auto callable = callables.find(entry.second.callable);
        auto *source = ObjectDB::get_instance(entry.second.source);
        if (callable == callables.end() || !source || !source->is_connected("tree_exiting", callable->second.native)) retired.push_back(entry.first);
    }
    for (uint32_t id : retired) retire_connection(id);
}
/** 正常离树只撤销该入树作用域；源或目标死亡还必须拆除普通原生连接。 */
void MetonWebGame::cleanup_scope(uint32_t p_owner, bool p_destroyed) {
    auto owner = objects.find(p_owner);
    if (owner == objects.end()) return;
    std::vector<uint32_t> retired;
    for (const auto &entry : connections) {
        auto callable = callables.find(entry.second.callable);
        if (entry.second.scope.owner == p_owner || (p_destroyed && (entry.second.source == owner->second.object ||
                (callable != callables.end() && callable->second.target == p_owner)))) retired.push_back(entry.first);
    }
    for (uint32_t id : retired) retire_connection(id, p_destroyed ? "object_invalid" : "scope_ended");
    if (owner->second.scope) scopes.erase(owner->second.scope);
    owner->second.scope = 0;
    if (p_destroyed) {
        std::vector<uint32_t> dead;
        for (const auto &entry : callables) if (entry.second.target == p_owner) dead.push_back(entry.first);
        for (uint32_t id : dead) { handle_event(id, "_handle_retire"); callables.erase(id); }
    }
}

/** 信号与作用域使用原生登记，不把 JS 包装身份当成执行许可。 */
Dictionary MetonWebGame::signal_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    uint32_t id = 0;
    if (op == "callable_release") {
        if (!handle(p_request.get("callable", Variant()), id)) return failure("invalid_argument");
        for (const auto &entry : connections) if (entry.second.callable == id) return ok(false);
        return ok(callables.erase(id) != 0);
    }
    const bool teardown = op == "disconnect" || op == "subscription_disconnect";
    if (phase == MetonGameSupport::P_MODULE || phase == MetonGameSupport::P_CONSTRUCT ||
            (phase == MetonGameSupport::P_CLEANUP && !teardown && op != "signal_get" && op != "scope_of")) return failure("phase_conflict");
    if (op == "subscription_disconnect") {
        if (!handle(p_request.get("connection", Variant()), id)) return failure("invalid_argument");
        retire_connection(id); return ok();
    }
    if (op == "signal_get" || op == "scope_of" || op == "callable") {
        if (!handle(p_request.get("token", Variant()), id)) return failure("invalid_argument");
        auto owner = objects.find(id);
        if (owner == objects.end() || owner->second.retired) return failure("object_invalid");
        auto *node = Object::cast_to<Node>(ObjectDB::get_instance(owner->second.object));
        if (!node || node->is_queued_for_deletion()) return failure("object_invalid");
        if (op == "signal_get") return ok(int64_t(id));
        if (op == "scope_of") {
            Stamp stamp{id, owner->second.epoch};
            if (!scope_valid(stamp, true)) return failure("scope_ended");
            if (!owner->second.scope) {
                if (!next_scope) return failure("budget_exceeded");
                owner->second.scope = next_scope++;
                scopes[owner->second.scope] = stamp;
            }
            return ok(int64_t(owner->second.scope));
        }
        Stamp scope;
        if (p_request.has("scope")) {
            uint32_t scope_id;
            if (!handle(p_request["scope"], scope_id)) return failure("invalid_argument");
            auto registered = scopes.find(scope_id);
            if (registered == scopes.end() || !scope_valid(registered->second, true)) return failure("scope_ended");
            scope = registered->second;
            if (scope.owner != id) return failure("invalid_argument");
        }
        if (callables.size() >= MAX_CONNECTIONS || !next_callable) return failure("budget_exceeded");
        return ok(int64_t(create_callable(id, scope)));
    }
    if (op != "connect" && op != "disconnect" && op != "scope_connect" && op != "scope_wait") return failure("unsupported_member");
    uint32_t source_id, callable_id;
    if (!handle(p_request.get("token", Variant()), source_id) || !handle(p_request.get("callable", Variant()), callable_id)) return failure("invalid_argument");
    auto owner = objects.find(source_id);
    if (owner == objects.end() || owner->second.retired) return failure("object_invalid");
    auto *source = Object::cast_to<Node>(ObjectDB::get_instance(owner->second.object));
    if (!source || source->is_queued_for_deletion() || !callable_valid(callable_id)) return failure("object_invalid");
    const CallableEntry callable = callables.at(callable_id);
    Stamp scope;
    const bool scoped = op == "scope_connect" || op == "scope_wait";
    if (scoped) {
        uint32_t scope_id;
        if (!handle(p_request.get("scope", Variant()), scope_id)) return failure("invalid_argument");
        auto registered = scopes.find(scope_id);
        if (registered == scopes.end() || !scope_valid(registered->second, true)) return failure("scope_ended");
        scope = registered->second;
        if (callable.scope.owner != scope.owner || callable.scope.epoch != scope.epoch) return failure("invalid_argument");
    } else if (callable.scope.owner) return failure("invalid_argument");
    if (op == "disconnect") {
        source->disconnect("tree_exiting", callable.native);
        std::vector<uint32_t> retired;
        for (const auto &entry : connections) if (entry.second.source == owner->second.object && entry.second.callable == callable_id) retired.push_back(entry.first);
        for (uint32_t connection : retired) retire_connection(connection);
        return ok();
    }
    int flags = 0;
    if (op == "scope_wait") flags = Object::CONNECT_ONE_SHOT;
    if (p_request.has("flags")) {
        const Variant raw = p_request["flags"];
        if ((raw.get_type() != Variant::INT && raw.get_type() != Variant::FLOAT) || (double(raw) != 0 && double(raw) != Object::CONNECT_ONE_SHOT)) return failure("invalid_argument");
        flags = int(raw);
    }
    if (connections.size() >= MAX_CONNECTIONS || !next_connection) return failure("budget_exceeded");
    Error result = source->connect("tree_exiting", callable.native, flags);
    if (result != OK) return scoped ? failure("invalid_argument") : ok(int(result));
    const uint32_t connection = next_connection++;
    connections[connection] = {owner->second.object, callable_id, scope, op == "scope_wait"};
    handle_event(callable_id, "_handle_retain");
    if (op == "scope_wait") {
        const Dictionary context = capture_continuation(scope, false);
        if (!bool(context["ok"])) { retire_connection(connection); return context; }
        Dictionary value; value["connection"] = int64_t(connection); value["context"] = context["value"]; return ok(value);
    }
    return ok(scoped ? Variant(int64_t(connection)) : Variant(int(result)));
}
