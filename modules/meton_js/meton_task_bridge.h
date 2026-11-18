#pragma once
#include "core/string/ustring.h"
namespace v8 { class TypedArray; }
#include <v8.h>

/** 任务私有校验域；任何私有函数、数组和原型都不交给项目脚本。 */
class MetonTaskBridge {
    String validator_source;
    v8::Isolate *isolate = nullptr;
    v8::Global<v8::Context> private_context;
    v8::Global<v8::Value> record_prototype;
    v8::Global<v8::Value> array_prototype;
    v8::Global<v8::Function> inspect_function;
    v8::Global<v8::Function> check_function;
    v8::Global<v8::Function> input_validator;
    v8::Global<v8::Function> output_validator;
    v8::Global<v8::Object> input_limits;
    v8::Global<v8::Object> output_limits;

    /** 先原生拒绝 Proxy，再读取自有数据描述符，不调用脚本反射函数。 */
    static void inspect_value(const v8::FunctionCallbackInfo<v8::Value> &p_args);
    /** 生成的独立校验模块没有导入；解析出现引用一律拒绝。 */
    static v8::MaybeLocal<v8::Module> deny_module(v8::Local<v8::Context>, v8::Local<v8::String>, v8::Local<v8::FixedArray>, v8::Local<v8::Module>);
    /** 只用于已验证并重新 JSON 解析的目标值，不对任意外来对象使用。 */
    bool freeze_input(v8::Local<v8::Context> p_context, v8::Local<v8::Value> p_value, unsigned p_depth);
public:
    explicit MetonTaskBridge(const String &p_validator) : validator_source(p_validator) {}
    /** 项目派生的校验代码计入编译后任务闭包，受信固定 R05 字节另属平台。 */
    uint64_t material_bytes() const { return validator_source.utf8().length(); }
    /** 在项目模块求值之前保存目标内建原型并建立不可达的私有域。 */
    bool initialize(v8::Isolate *p_isolate, v8::Local<v8::Context> p_task_context);
    /** 只交出规范 JSON 与固定拒绝原因，校验器不执行跨域对象的方法。 */
    bool check(v8::Local<v8::Value> p_value, bool p_output, String &r_json, String &r_reason);
    /** 输入重新进入项目自己的内建原型域，随后由原生冻结。 */
    v8::MaybeLocal<v8::Value> input(v8::Local<v8::Context> p_context, const String &p_json, String &r_reason);
};
