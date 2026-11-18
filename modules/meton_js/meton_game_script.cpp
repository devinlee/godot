#include "meton_game_script.h"
#include "meton_game_binding.h"
#include "meton_game_lifecycle.generated.h"
#include "scene/main/node.h"

bool MetonGameScript::has_method(const StringName &p_method) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (p_method == entry.name) return true;
    return false;
}
MethodInfo MetonGameScript::get_method_info(const StringName &p_method) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (p_method == entry.name) {
        return entry.arguments ? MethodInfo(p_method, PropertyInfo(Variant::FLOAT, "delta")) : MethodInfo(p_method);
    }
    return {};
}
void MetonGameScript::get_script_method_list(List<MethodInfo> *p_list) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) p_list->push_back(get_method_info(entry.name));
}
MetonGameScriptInstance::MetonGameScriptInstance(MetonGameBinding *p_binding, uint32_t p_token, Object *p_owner) :
        binding(p_binding), token(p_token), owner(p_owner->get_instance_id()) {
    script.instantiate();
    script->set_base(p_owner->get_class_name());
}
void MetonGameScriptInstance::get_method_list(List<MethodInfo> *p_list) const {
    for (const auto &entry : MetonLifecycleSupport::CALLBACKS) if (has_method(entry.name)) p_list->push_back(script->get_method_info(entry.name));
}
bool MetonGameScriptInstance::has_method(const StringName &p_method) const {
    return binding->has_callback(token, p_method);
}
Variant MetonGameScriptInstance::callp(const StringName &p_method, const Variant **p_args, int p_count, Callable::CallError &r_error) {
    // 即使没有用户退出方法，也要先关闭准入，再由 Godot 发出 tree_exiting。
    if (p_method == StringName("_exit_tree")) binding->closing(token);
    r_error.error = binding->dispatch_callback(token, p_method, p_args, p_count) ? Callable::CallError::CALL_OK : Callable::CallError::CALL_ERROR_INVALID_METHOD;
    return {};
}
void MetonGameScriptInstance::notification(int p_notification, bool p_reversed) {
    if (p_notification == Node::NOTIFICATION_ENTER_TREE) binding->entered(token);
    else if (p_notification == Node::NOTIFICATION_EXIT_TREE) binding->exited(token);
    else if (p_notification == Node::NOTIFICATION_PARENTED || p_notification == Node::NOTIFICATION_UNPARENTED ||
            p_notification == Node::NOTIFICATION_PATH_RENAMED || p_notification == Node::NOTIFICATION_CHILD_ORDER_CHANGED ||
            p_notification == Node::NOTIFICATION_MOVED_IN_PARENT) binding->structure_changed(token);
}
MetonGameScriptInstance::~MetonGameScriptInstance() {
    binding->object_destroyed(token);
}
