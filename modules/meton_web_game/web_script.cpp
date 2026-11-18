#include "web_script.h"
#include "web_game.h"
namespace MetonLifecycleSupport { struct Callback { const char *name; const char *phase; int arguments; }; constexpr Callback CALLBACKS[] = {{"_enter_tree","enter_tree",0},{"_ready","ready",0},{"_process","process",1},{"_physics_process","physics",1},{"_exit_tree","cleanup",0}}; }
#include "scene/main/node.h"

bool MetonWebScript::has_method(const StringName &p_method) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (p_method == entry.name) return true;
    return false;
}
MethodInfo MetonWebScript::get_method_info(const StringName &p_method) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (p_method == entry.name) {
        return entry.arguments ? MethodInfo(p_method, PropertyInfo(Variant::FLOAT, "delta")) : MethodInfo(p_method);
    }
    return {};
}
void MetonWebScript::get_script_method_list(List<MethodInfo> *p_list) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) p_list->push_back(get_method_info(entry.name));
}
MetonWebScriptInstance::MetonWebScriptInstance(MetonWebGame *p_binding, uint32_t p_token, Object *p_owner) :
        binding(p_binding), token(p_token), owner(p_owner->get_instance_id()) {
    script.instantiate();
    script->set_base(p_owner->get_class_name());
}
void MetonWebScriptInstance::get_method_list(List<MethodInfo> *p_list) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (has_method(entry.name)) p_list->push_back(script->get_method_info(entry.name));
}
bool MetonWebScriptInstance::has_method(const StringName &p_method) const {
    return binding->has_callback(token, p_method);
}
Variant MetonWebScriptInstance::callp(const StringName &p_method, const Variant **p_args, int p_count, Callable::CallError &r_error) {
    // 即使没有用户退出方法，也要先关闭准入，再由 Godot 发出 tree_exiting。
    if (p_method == StringName("_exit_tree")) binding->closing(token);
    r_error.error = binding->dispatch_callback(token, p_method, p_args, p_count) ? Callable::CallError::CALL_OK : Callable::CallError::CALL_ERROR_INVALID_METHOD;
    return {};
}
void MetonWebScriptInstance::notification(int p_notification, bool p_reversed) {
    if (p_notification == Node::NOTIFICATION_ENTER_TREE) binding->entered(token);
    else if (p_notification == Node::NOTIFICATION_EXIT_TREE) binding->exited(token);
    else if (p_notification == Node::NOTIFICATION_PARENTED || p_notification == Node::NOTIFICATION_UNPARENTED ||
            p_notification == Node::NOTIFICATION_PATH_RENAMED || p_notification == Node::NOTIFICATION_CHILD_ORDER_CHANGED ||
            p_notification == Node::NOTIFICATION_MOVED_IN_PARENT) binding->structure_changed(token);
}
MetonWebScriptInstance::~MetonWebScriptInstance() {
    binding->object_destroyed(token);
}
