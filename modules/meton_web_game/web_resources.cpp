#include "web_game.h"
#include "web_support.generated.h"
#include "meton_game_resources.generated.h"
#include "scene/2d/sprite_2d.h"
#include "scene/resources/texture.h"
#include <cmath>

namespace {
Dictionary ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
bool resource_token(const Variant &p_value, uint32_t &r_value) {
    if ((p_value.get_type() != Variant::FLOAT && p_value.get_type() != Variant::INT) || !std::isfinite(double(p_value)) ||
            double(p_value) < 1 || double(p_value) > UINT32_MAX || double(p_value) != uint32_t(double(p_value))) return false;
    r_value = uint32_t(double(p_value)); return true;
}
}

/** 只有实际固定加载器产生的 Ref 可包装，路径和 JS 对象形状不构成资源身份。 */
Dictionary MetonWebGame::wrap_resource(const Ref<Resource> &p_resource) {
    if (p_resource.is_null()) return ok();
    if (!content.owns(p_resource)) return failure("permission_denied");
    uint32_t token;
    auto existing = resource_tokens.find(p_resource->get_instance_id());
    if (existing != resource_tokens.end()) token = existing->second;
    else {
        if (resources.size() >= MetonResourceSupport::NATIVE_RESOURCES || !next_resource) return failure("budget_exceeded");
        token = next_resource++;
        resources[token] = {p_resource, 0};
        resource_tokens[p_resource->get_instance_id()] = token;
    }
    Dictionary value;
    value["token"] = int64_t(token); value["lease"] = int64_t(resources.at(token).lease);
    value["type"] = Object::cast_to<Texture2D>(p_resource.ptr()) ? "Texture2D" : "Resource";
    return ok(value);
}

Dictionary MetonWebGame::resource_request(const Dictionary &p_request) {
    const String op = p_request.get("op", "");
    uint32_t token = 0;
    if (op == "resource_release" || op == "resource_renew") {
        if (!resource_token(p_request.get("token", Variant()), token)) return failure("invalid_argument");
        auto found = resources.find(token);
        if (found == resources.end()) return op == "resource_release" ? ok(false) : failure("object_invalid");
        const Variant lease = p_request.get("lease", Variant());
        if ((lease.get_type() != Variant::FLOAT && lease.get_type() != Variant::INT) ||
                !std::isfinite(double(lease)) || double(lease) != double(found->second.lease)) return failure("object_invalid");
        if (op == "resource_release") {
            resource_tokens.erase(found->second.value->get_instance_id()); resources.erase(found); return ok(true);
        }
        if (found->second.lease == UINT32_MAX) return failure("budget_exceeded");
        return ok(int64_t(++found->second.lease));
    }
    const int operation = op == "texture_get" ? 0 : op == "texture_set" ? 1 : 2;
    if (!(MetonResourceSupport::MEMBERS[operation].phases & phase)) return failure("phase_conflict");
    if (op == "resource_load") {
        const Variant raw = p_request.get("path", Variant());
        if (raw.get_type() != Variant::STRING || String(raw).length() > 246) return failure("invalid_argument");
        String name = raw;
        for (int i = 0; i < name.length(); ++i) if (name[i] < 32 || name[i] > 126) return failure("invalid_argument");
        if (content.resolve(name).is_empty()) return failure("permission_denied");
        Error error = OK;
        Ref<Resource> resource = content.load(name, &error);
        if (error == ERR_OUT_OF_MEMORY) return failure("budget_exceeded");
        if (error == ERR_UNAUTHORIZED) return failure("permission_denied");
        if (error != OK) return failure("dependency_failed");
        return wrap_resource(resource);
    }
    if (op != "texture_get" && op != "texture_set") return failure("unsupported_member");
    if (!resource_token(p_request.get("token", Variant()), token) || !objects.count(token) || objects.at(token).retired) return failure("object_invalid");
    auto *node = Object::cast_to<Node>(ObjectDB::get_instance(objects.at(token).object));
    if (!node || node->is_queued_for_deletion()) return failure("object_invalid");
    auto *sprite = Object::cast_to<Sprite2D>(node);
    if (!sprite) return failure("invalid_argument");
    if (op == "texture_get") return wrap_resource(sprite->get_texture());
    if (const char *reason = impact_error(node, TEXTURE)) return failure(reason);
    const Variant value = p_request.get("resource", Variant());
    if (value.get_type() == Variant::NIL) { sprite->set_texture(Ref<Texture2D>()); return ok(); }
    uint32_t resource;
    if (!resource_token(value, resource)) return failure("invalid_argument");
    auto found = resources.find(resource);
    if (found == resources.end() || !content.owns(found->second.value)) return failure("object_invalid");
    Ref<Texture2D> texture = found->second.value;
    if (texture.is_null()) return failure("invalid_argument");
    sprite->set_texture(texture); return ok();
}

Dictionary MetonWebGame::install_content(const String &p_json, const String &p_digest, const String &p_version) {
    if (revoked || !objects.empty()) return failure("phase_conflict");
    MetonNativeMemoryScope memory_scope(native_memory);
    return content.install(p_json, p_digest, p_version) ? ok() : failure("content_mismatch");
}

/** 受信夹具观察原生缓存并改变资源名称，普通脚本未开放反射或任意资源字段。 */
Dictionary MetonWebGame::resource_probe(const String &p_name) {
    if (revoked) return failure("scope_ended");
    MetonNativeMemoryScope memory_scope(native_memory);
    Dictionary result = content.state();
    Ref<Resource> shared;
    if (p_name == "read" || p_name == "rename") shared = content.load("res://shared.tres");
    if (shared.is_valid()) {
        if (p_name == "rename") shared->set_name("only-this-game");
        result["shared_name"] = shared->get_name();
    }
    if (p_name == "lease") {
        // 只有本夹具使用未交给脚本的 local.tres，模拟晚到的旧 GC 通知。
        Ref<Resource> local = content.load("res://local.tres");
        const Dictionary descriptor = wrap_resource(local)["value"];
        Dictionary request; request["op"] = "resource_renew"; request["token"] = descriptor["token"]; request["lease"] = descriptor["lease"];
        const Dictionary renewed = resource_request(request);
        request["op"] = "resource_release";
        const Dictionary stale = resource_request(request);
        result["stale_rejected"] = stale.get("code", "") == Variant("object_invalid");
        result["new_lease_retained"] = resources.count(int64_t(descriptor["token"])) != 0;
        request["lease"] = renewed["value"];
        result["current_released"] = resource_request(request)["value"];
    }
    result["wrappers"] = int(resources.size());
    return result;
}
