#pragma once
#include "core/object/script_language.h"
#include "core/object/script_instance.h"

class MetonGameBinding;

/** 内部脚本描述只承接已准入的实例；不注册可从项目文件加载 JS 的全局入口。 */
class MetonGameScript : public Script {
    GDCLASS(MetonGameScript, Script);
    StringName base;
protected:
    static void _bind_methods() {}
public:
    void set_base(const StringName &p_base) { base = p_base; }
    bool can_instantiate() const override { return false; }
    Ref<Script> get_base_script() const override { return {}; }
    StringName get_global_name() const override { return {}; }
    bool inherits_script(const Ref<Script> &p_script) const override { return false; }
    StringName get_instance_base_type() const override { return base; }
    ScriptInstance *instance_create(Object *p_this) override { return nullptr; }
    bool has_source_code() const override { return false; }
    String get_source_code() const override { return {}; }
    void set_source_code(const String &p_code) override {}
    Error reload(bool p_keep_state = false) override { return ERR_UNAVAILABLE; }
#ifdef TOOLS_ENABLED
    StringName get_doc_class_name() const override { return {}; }
    Vector<DocData::ClassDoc> get_documentation() const override { return {}; }
    String get_class_icon_path() const override { return {}; }
#endif
    bool has_method(const StringName &p_method) const override;
    MethodInfo get_method_info(const StringName &p_method) const override;
    bool is_tool() const override { return false; }
    bool is_valid() const override { return true; }
    bool is_abstract() const override { return false; }
    ScriptLanguage *get_language() const override { return nullptr; }
    bool has_script_signal(const StringName &p_signal) const override { return false; }
    void get_script_signal_list(List<MethodInfo> *r_signals) const override {}
    bool get_property_default_value(const StringName &p_property, Variant &r_value) const override { return false; }
    void get_script_method_list(List<MethodInfo> *p_list) const override;
    void get_script_property_list(List<PropertyInfo> *p_list) const override {}
    const Variant get_rpc_config() const override { return Dictionary(); }
};

/** Godot 原生通知负责入树/离树次序，不能由 JS 自报代次或测试端代调生命周期。 */
class MetonGameScriptInstance : public ScriptInstance {
    MetonGameBinding *binding;
    uint32_t token;
    ObjectID owner;
    Ref<MetonGameScript> script;
public:
    MetonGameScriptInstance(MetonGameBinding *p_binding, uint32_t p_token, Object *p_owner);
    bool set(const StringName &p_name, const Variant &p_value) override { return false; }
    bool get(const StringName &p_name, Variant &r_ret) const override { return false; }
    void get_property_list(List<PropertyInfo> *p_properties) const override {}
    Variant::Type get_property_type(const StringName &p_name, bool *r_valid = nullptr) const override {
        if (r_valid) *r_valid = false;
        return Variant::NIL;
    }
    void validate_property(PropertyInfo &p_property) const override {}
    bool property_can_revert(const StringName &p_name) const override { return false; }
    bool property_get_revert(const StringName &p_name, Variant &r_ret) const override { return false; }
    Object *get_owner() override { return ObjectDB::get_instance(owner); }
    void get_method_list(List<MethodInfo> *p_list) const override;
    bool has_method(const StringName &p_method) const override;
    Variant callp(const StringName &p_method, const Variant **p_args, int p_count, Callable::CallError &r_error) override;
    void notification(int p_notification, bool p_reversed = false) override;
    Ref<Script> get_script() const override { return script; }
    ScriptLanguage *get_language() override { return nullptr; }
    ~MetonGameScriptInstance() override;
};
