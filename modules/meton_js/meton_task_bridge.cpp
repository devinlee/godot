#include "meton_task_bridge.h"
#include "meton_task_data.generated.h"

namespace {
constexpr v8::ExternalPointerTypeTag TASK_INSPECTOR_TAG = 9;
/** 这里只转换受信固定名称或已经确认的字符串值。 */
v8::Local<v8::String> text(v8::Isolate *isolate, const char *value) {
    return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}
v8::Local<v8::String> text(v8::Isolate *isolate, const String &value) {
    const auto encoded = value.utf8();
    return v8::String::NewFromUtf8(isolate, encoded.get_data(), v8::NewStringType::kNormal, encoded.length()).ToLocalChecked();
}
/** 显式标记 data 属性，避免原型 setter 被意外调用。 */
bool field(v8::Local<v8::Context> context, v8::Local<v8::Object> target, const char *key, v8::Local<v8::Value> value) {
    return target->CreateDataProperty(context, text(v8::Isolate::GetCurrent(), key), value).FromMaybe(false);
}
/** 原生内部值即使换原型也不得变成丢失内部状态的空记录。 */
bool native_value(v8::Local<v8::Value> value) {
    return value->IsDate() || value->IsRegExp() || value->IsMap() || value->IsSet() || value->IsWeakMap() || value->IsWeakSet()
        || value->IsMapIterator() || value->IsSetIterator() || value->IsPromise() || value->IsGeneratorObject()
        || value->IsArgumentsObject() || value->IsNativeError() || value->IsBigIntObject() || value->IsBooleanObject()
        || value->IsNumberObject() || value->IsStringObject() || value->IsSymbolObject() || value->IsArrayBuffer()
        || value->IsSharedArrayBuffer() || value->IsArrayBufferView() || value->IsExternal() || value->IsModuleNamespaceObject()
        || value->IsWasmMemoryObject() || value->IsWasmModuleObject();
}
}

void MetonTaskBridge::inspect_value(const v8::FunctionCallbackInfo<v8::Value> &args) {
    auto *self = static_cast<MetonTaskBridge *>(args.Data().As<v8::External>()->Value(TASK_INSPECTOR_TAG));
    auto *isolate = args.GetIsolate();
    auto ctx = self->private_context.Get(isolate);
    auto reject = [&](const char *reason) {
        auto result = v8::Object::New(isolate);
        if (field(ctx, result, "kind", text(isolate, "rejected")) && field(ctx, result, "reason", text(isolate, reason))) args.GetReturnValue().Set(result);
    };
    if (args.Length() != 2 || !args[0]->IsObject() || !args[1]->IsUint32()) { reject("inspection_failed"); return; }
    auto value = args[0];
    // V8 的 Proxy 判别不进入其 getPrototypeOf/ownKeys/descriptor 陷阱，包括 revoked Proxy。
    if (value->IsProxy()) { reject("proxy"); return; }
    auto object = value.As<v8::Object>();
    if (native_value(value) || object->InternalFieldCount() != 0) { reject("unsupported_value"); return; }
    const bool array = value->IsArray();
    auto prototype = object->GetPrototype();
    if (array ? !prototype->StrictEquals(self->array_prototype.Get(isolate))
              : !prototype->IsNull() && !prototype->StrictEquals(self->record_prototype.Get(isolate))) { reject("prototype"); return; }
    v8::Local<v8::Array> keys;
    if (!object->GetOwnPropertyNames(ctx, v8::ALL_PROPERTIES, v8::KeyConversionMode::kConvertToString).ToLocal(&keys)) { reject("inspection_failed"); return; }
    const unsigned maximum = args[1].As<v8::Uint32>()->Value();
    const unsigned length = array ? value.As<v8::Array>()->Length() : 0;
    if (keys->Length() > maximum + (array ? 1 : 0) || length > maximum) { reject("entries"); return; }
    if (array && keys->Length() != length + 1) { reject("array_shape"); return; }
    auto items = v8::Array::New(isolate, array ? length : keys->Length());
    unsigned count = 0;
    for (unsigned i = 0; i < keys->Length(); ++i) {
        v8::Local<v8::Value> key, description;
        if (!keys->Get(ctx, i).ToLocal(&key)) { reject("inspection_failed"); return; }
        if (!key->IsString()) { reject("symbol_key"); return; }
        if (array && key->StrictEquals(text(isolate, "length"))) continue;
        if (!object->GetOwnPropertyDescriptor(ctx, key.As<v8::Name>()).ToLocal(&description) || !description->IsObject()) { reject("inspection_failed"); return; }
        auto descriptor = description.As<v8::Object>();
        if (!descriptor->HasOwnProperty(ctx, text(isolate, "value")).FromMaybe(false)) { reject("accessor"); return; }
        v8::Local<v8::Value> child, enumerable;
        if (!descriptor->Get(ctx, text(isolate, "value")).ToLocal(&child) || !descriptor->Get(ctx, text(isolate, "enumerable")).ToLocal(&enumerable)) { reject("inspection_failed"); return; }
        if (!enumerable->IsTrue()) { reject("non_enumerable"); return; }
        if (array) {
            // ToArrayIndex 只接已经确认的原生字符串，不转换外来对象。
            v8::Local<v8::Uint32> index;
            if (!key->ToArrayIndex(ctx).ToLocal(&index) || index->Value() >= length) { reject("array_shape"); return; }
            if (!items->CreateDataProperty(ctx, index->Value(), child).FromMaybe(false)) { reject("inspection_failed"); return; }
        } else {
            auto entry = v8::Object::New(isolate);
            if (!field(ctx, entry, "key", key) || !field(ctx, entry, "value", child) || !items->CreateDataProperty(ctx, count++, entry).FromMaybe(false)) { reject("inspection_failed"); return; }
        }
    }
    auto result = v8::Object::New(isolate);
    if (!field(ctx, result, "kind", text(isolate, array ? "array" : "record")) || !field(ctx, result, array ? "items" : "fields", items)) { reject("inspection_failed"); return; }
    args.GetReturnValue().Set(result);
}

v8::MaybeLocal<v8::Module> MetonTaskBridge::deny_module(v8::Local<v8::Context> context, v8::Local<v8::String>, v8::Local<v8::FixedArray>, v8::Local<v8::Module>) {
    auto *isolate = v8::Isolate::GetCurrent();
    isolate->ThrowException(v8::Exception::Error(text(isolate, "Task validator imports are forbidden")));
    return {};
}

bool MetonTaskBridge::initialize(v8::Isolate *p_isolate, v8::Local<v8::Context> p_task_context) {
    isolate = p_isolate;
    {
        v8::Context::Scope scope(p_task_context);
        record_prototype.Reset(isolate, v8::Object::New(isolate)->GetPrototype());
        array_prototype.Reset(isolate, v8::Array::New(isolate)->GetPrototype());
    }
    auto ctx = v8::Context::New(isolate);
    if (ctx.IsEmpty()) return false;
    private_context.Reset(isolate, ctx);
    v8::Context::Scope scope(ctx);
    ctx->AllowCodeGenerationFromStrings(false);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> evaluated, api, check;
    if (!v8::Script::Compile(ctx, text(isolate, MetonTaskSupport::DATA_CODE)).ToLocal(&script) || !script->Run(ctx).ToLocal(&evaluated)
        || !ctx->Global()->Get(ctx, text(isolate, "MetonTaskData")).ToLocal(&api) || !api->IsObject()
        || !api.As<v8::Object>()->Get(ctx, text(isolate, "checkTaskData")).ToLocal(&check) || !check->IsFunction()) return false;
    check_function.Reset(isolate, check.As<v8::Function>());
    v8::Local<v8::Function> inspect;
    if (!v8::Function::New(ctx, inspect_value, v8::External::New(isolate, this, TASK_INSPECTOR_TAG), 2, v8::ConstructorBehavior::kThrow).ToLocal(&inspect)) return false;
    inspect_function.Reset(isolate, inspect);
    v8::ScriptOrigin origin(text(isolate, "meton-private:task-validator"), 0, 0, false, -1, {}, false, false, true);
    v8::ScriptCompiler::Source source(text(isolate, validator_source), origin);
    v8::Local<v8::Module> module;
    v8::Local<v8::Promise> completion;
    if (!v8::ScriptCompiler::CompileModule(isolate, &source).ToLocal(&module) || !module->InstantiateModule(ctx, deny_module).FromMaybe(false)
        || !module->Evaluate(ctx).ToLocal(&completion)) return false;
    isolate->PerformMicrotaskCheckpoint();
    if (completion->State() != v8::Promise::kFulfilled || isolate->IsExecutionTerminating()) return false;
    auto exports = module->GetModuleNamespace().As<v8::Object>();
    for (bool output : {false, true}) {
        v8::Local<v8::Value> validator;
        if (!exports->Get(ctx, text(isolate, output ? "validateOutput" : "validateInput")).ToLocal(&validator) || !validator->IsFunction()) return false;
        (output ? output_validator : input_validator).Reset(isolate, validator.As<v8::Function>());
        auto limits = v8::Object::New(isolate);
        if (!field(ctx, limits, "bytes", v8::Integer::NewFromUnsigned(isolate, output ? MetonTaskSupport::DTO_OUTPUT_BYTES : MetonTaskSupport::DTO_INPUT_BYTES))
            || !field(ctx, limits, "depth", v8::Integer::NewFromUnsigned(isolate, MetonTaskSupport::DTO_DEPTH))
            || !field(ctx, limits, "entries", v8::Integer::NewFromUnsigned(isolate, MetonTaskSupport::DTO_ENTRIES))
            || !field(ctx, limits, "string_bytes", v8::Integer::NewFromUnsigned(isolate, MetonTaskSupport::DTO_STRING_BYTES))
            || !limits->SetIntegrityLevel(ctx, v8::IntegrityLevel::kFrozen).FromMaybe(false)) return false;
        (output ? output_limits : input_limits).Reset(isolate, limits);
    }
    validator_source = String();
    return true;
}

bool MetonTaskBridge::check(v8::Local<v8::Value> value, bool output, String &json, String &reason) {
    auto ctx = private_context.Get(isolate);
    v8::Context::Scope scope(ctx);
    v8::Local<v8::Value> args[] = {value, (output ? output_limits : input_limits).Get(isolate), inspect_function.Get(isolate), (output ? output_validator : input_validator).Get(isolate)};
    v8::Local<v8::Value> result, status, encoded;
    reason = "inspection_failed";
    if (!check_function.Get(isolate)->Call(ctx, v8::Undefined(isolate), 4, args).ToLocal(&result) || !result->IsObject()) return false;
    auto object = result.As<v8::Object>();
    if (!object->Get(ctx, text(isolate, "status")).ToLocal(&status)) return false;
    if (!status->StrictEquals(text(isolate, "accepted"))) {
        v8::Local<v8::Value> failure;
        if (object->Get(ctx, text(isolate, "reason")).ToLocal(&failure) && failure->IsString()) {
            v8::String::Utf8Value text(isolate, failure); if (*text) reason = String::utf8(*text, text.length());
        }
        return false;
    }
    if (!object->Get(ctx, text(isolate, "json")).ToLocal(&encoded) || !encoded->IsString()) return false;
    v8::String::Utf8Value text(isolate, encoded);
    if (!*text) return false;
    json = String::utf8(*text, text.length()); reason = String();
    return true;
}

bool MetonTaskBridge::freeze_input(v8::Local<v8::Context> ctx, v8::Local<v8::Value> value, unsigned depth) {
    if (!value->IsObject()) return true;
    if (depth > MetonTaskSupport::DTO_DEPTH || value->IsProxy()) return false;
    auto object = value.As<v8::Object>();
    if (value->IsArray()) {
        for (unsigned i = 0; i < value.As<v8::Array>()->Length(); ++i) {
            v8::Local<v8::Value> child;
            if (!object->Get(ctx, i).ToLocal(&child) || !freeze_input(ctx, child, depth + 1)) return false;
        }
    } else {
        v8::Local<v8::Array> keys;
        if (!object->GetOwnPropertyNames(ctx).ToLocal(&keys)) return false;
        for (unsigned i = 0; i < keys->Length(); ++i) {
            v8::Local<v8::Value> key, child;
            if (!keys->Get(ctx, i).ToLocal(&key) || !object->Get(ctx, key).ToLocal(&child) || !freeze_input(ctx, child, depth + 1)) return false;
        }
    }
    return object->SetIntegrityLevel(ctx, v8::IntegrityLevel::kFrozen).FromMaybe(false);
}

v8::MaybeLocal<v8::Value> MetonTaskBridge::input(v8::Local<v8::Context> ctx, const String &json, String &reason) {
    v8::Local<v8::Value> parsed;
    if (!v8::JSON::Parse(ctx, text(isolate, json)).ToLocal(&parsed)) { reason = "invalid_json"; return {}; }
    String canonical;
    if (!check(parsed, false, canonical, reason)) return {};
    // JSON 是唯一跨私有域出口；不把私有 Array.prototype 通过输入参数送给项目。
    if (!v8::JSON::Parse(ctx, text(isolate, canonical)).ToLocal(&parsed) || !freeze_input(ctx, parsed, 1)) { reason = "input_freeze_failed"; return {}; }
    return parsed;
}
