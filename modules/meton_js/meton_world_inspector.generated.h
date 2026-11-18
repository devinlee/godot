// 由 game_world_native.ts 从固定 D1 检查器生成；不手改。
#pragma once
namespace MetonWorldSupport {
inline constexpr const char *INPUT_DIGEST = "4f26ee1314bcd33e4d100318da18623ce5d618d86ad59a526fd4d53207cd0466";
}
#ifdef METON_WORLD_INSPECTOR_IMPLEMENTATION
namespace WorldInspection {
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
void MetonGameBinding::inspect_world_value(const v8::FunctionCallbackInfo<v8::Value> &args) {
    auto *self = MetonGameBinding::self(args.GetIsolate());
    auto *isolate = args.GetIsolate();
    auto ctx = self->world_context.Get(isolate);
    auto reject = [&](const char *reason) {
        auto result = v8::Object::New(isolate);
        if (WorldInspection::field(ctx, result, "kind", WorldInspection::text(isolate, "rejected")) && WorldInspection::field(ctx, result, "reason", WorldInspection::text(isolate, reason))) args.GetReturnValue().Set(result);
    };
    if (args.Length() != 2 || !args[0]->IsObject() || !args[1]->IsUint32()) { reject("inspection_failed"); return; }
    auto value = args[0];
    // V8 的 Proxy 判别不进入其 getPrototypeOf/ownKeys/descriptor 陷阱，包括 revoked Proxy。
    if (value->IsProxy()) { reject("proxy"); return; }
    auto object = value.As<v8::Object>();
    if (WorldInspection::native_value(value) || object->InternalFieldCount() != 0) { reject("unsupported_value"); return; }
    const bool array = value->IsArray();
    auto prototype = object->GetPrototype();
    if (array ? !prototype->StrictEquals(self->world_array_prototype.Get(isolate))
              : !prototype->IsNull() && !prototype->StrictEquals(self->world_record_prototype.Get(isolate))) { reject("prototype"); return; }
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
        if (array && key->StrictEquals(WorldInspection::text(isolate, "length"))) continue;
        if (!object->GetOwnPropertyDescriptor(ctx, key.As<v8::Name>()).ToLocal(&description) || !description->IsObject()) { reject("inspection_failed"); return; }
        auto descriptor = description.As<v8::Object>();
        if (!descriptor->HasOwnProperty(ctx, WorldInspection::text(isolate, "value")).FromMaybe(false)) { reject("accessor"); return; }
        v8::Local<v8::Value> child, enumerable;
        if (!descriptor->Get(ctx, WorldInspection::text(isolate, "value")).ToLocal(&child) || !descriptor->Get(ctx, WorldInspection::text(isolate, "enumerable")).ToLocal(&enumerable)) { reject("inspection_failed"); return; }
        if (!enumerable->IsTrue()) { reject("non_enumerable"); return; }
        if (array) {
            // ToArrayIndex 只接已经确认的原生字符串，不转换外来对象。
            v8::Local<v8::Uint32> index;
            if (!key->ToArrayIndex(ctx).ToLocal(&index) || index->Value() >= length) { reject("array_shape"); return; }
            if (!items->CreateDataProperty(ctx, index->Value(), child).FromMaybe(false)) { reject("inspection_failed"); return; }
        } else {
            auto entry = v8::Object::New(isolate);
            if (!WorldInspection::field(ctx, entry, "key", key) || !WorldInspection::field(ctx, entry, "value", child) || !items->CreateDataProperty(ctx, count++, entry).FromMaybe(false)) { reject("inspection_failed"); return; }
        }
    }
    auto result = v8::Object::New(isolate);
    if (!WorldInspection::field(ctx, result, "kind", WorldInspection::text(isolate, array ? "array" : "record")) || !WorldInspection::field(ctx, result, array ? "items" : "fields", items)) { reject("inspection_failed"); return; }
    args.GetReturnValue().Set(result);
}

#endif
