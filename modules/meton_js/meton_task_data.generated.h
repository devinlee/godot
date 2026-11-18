// 由 task_v8.ts prepare 生成；源码与预算来自已固定材料。
#pragma once
namespace MetonTaskSupport {
inline constexpr unsigned SOURCE_FILE_BYTES = 262144;
inline constexpr unsigned SOURCE_CLOSURE_BYTES = 2097152;
inline constexpr unsigned SOURCE_FILES = 64;
inline constexpr unsigned DTO_INPUT_BYTES = 262144;
inline constexpr unsigned DTO_OUTPUT_BYTES = 262144;
inline constexpr unsigned DTO_DEPTH = 16;
inline constexpr unsigned DTO_ENTRIES = 4096;
inline constexpr unsigned DTO_STRING_BYTES = 65536;
inline constexpr unsigned MODULE_EVALUATION_MS = 1500;
inline constexpr unsigned TASK_DURATION_MS = 2000;
inline constexpr unsigned MICROTASKS_PER_CHECKPOINT = 128;
inline constexpr unsigned EVENTS_QUEUED = 256;
inline constexpr const char *DATA_DIGEST = "5691e93403e7540204ff294d2079d3dff4936a8e5b50f37cb156fbffbfc5719d";
inline constexpr const char *DATA_CODE = R"METON_TASK_DATA("use strict";
var MetonTaskData = (() => {
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __hasOwnProp = Object.prototype.hasOwnProperty;
  var __export = (target, all) => {
    for (var name in all)
      __defProp(target, name, { get: all[name], enumerable: true });
  };
  var __copyProps = (to, from, except, desc) => {
    if (from && typeof from === "object" || typeof from === "function") {
      for (let key of __getOwnPropNames(from))
        if (!__hasOwnProp.call(to, key) && key !== except)
          __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
    }
    return to;
  };
  var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

  // runtime/extension_host/task_data.ts
  var task_data_exports = {};
  __export(task_data_exports, {
    checkTaskData: () => checkTaskData,
    copyTaskData: () => copyTaskData
  });
  var quote = JSON.stringify;
  var freeze = Object.freeze;
  var create = Object.create;
  var define = Object.defineProperty;
  var budgetReasons = /* @__PURE__ */ new Set(["bytes", "depth", "entries", "string_bytes"]);
  function utf8Bytes(text, limit, fail, reason) {
    let bytes = 0;
    for (let i = 0; i < text.length; i++) {
      const code = text.charCodeAt(i);
      if (code < 128) bytes++;
      else if (code < 2048) bytes += 2;
      else if (code >= 55296 && code <= 56319) {
        const next = text.charCodeAt(++i);
        if (!(next >= 56320 && next <= 57343)) fail("invalid_unicode");
        bytes += 4;
      } else if (code >= 56320 && code <= 57343) fail("invalid_unicode");
      else bytes += 3;
      if (bytes > limit) fail(reason);
    }
    return bytes;
  }
  function copyTaskData(input, limits, inspect) {
    if (![limits.bytes, limits.depth, limits.entries, limits.string_bytes].every((value) => Number.isSafeInteger(value) && value > 0)) {
      throw new Error("\u4EFB\u52A1\u6570\u636E\u9884\u7B97\u65E0\u6548");
    }
    let reason = "inspection_failed", bytes = 0, entries = 0, depth = 0;
    const stopped = new Error("\u4EFB\u52A1\u6570\u636E\u62D2\u7EDD"), ancestors = /* @__PURE__ */ new Set(), chunks = [];
    function fail(value) {
      reason = value;
      throw stopped;
    }
    function append(text) {
      bytes += utf8Bytes(text, limits.bytes - bytes, fail, "bytes");
      chunks.push(text);
    }
    function copy(value, level) {
      if (++entries > limits.entries) fail("entries");
      if (level > limits.depth) fail("depth");
      depth = Math.max(depth, level);
      if (value === null || typeof value === "boolean") {
        append(value === null ? "null" : value ? "true" : "false");
        return value;
      }
      if (typeof value === "number") {
        if (!Number.isFinite(value)) fail("non_finite");
        const normalized = Object.is(value, -0) ? 0 : value;
        append(quote(normalized));
        return normalized;
      }
      if (typeof value === "string") {
        utf8Bytes(value, limits.string_bytes, fail, "string_bytes");
        append(quote(value));
        return value;
      }
      if (typeof value !== "object") fail("unsupported_value");
      if (ancestors.has(value)) fail("cycle");
      const inspected = inspect(value, limits.entries - entries);
      if (inspected.kind === "rejected") fail(inspected.reason);
      ancestors.add(value);
      if (inspected.kind === "array") {
        if (inspected.items.length > limits.entries - entries) fail("entries");
        const output2 = [];
        append("[");
        for (let i = 0; i < inspected.items.length; i++) {
          if (i) append(",");
          output2.push(copy(inspected.items[i], level + 1));
        }
        append("]");
        ancestors.delete(value);
        return freeze(output2);
      }
      if (inspected.fields.length > limits.entries - entries) fail("entries");
      let keyBytes = 0;
      for (const field of inspected.fields) {
        keyBytes += utf8Bytes(field.key, limits.string_bytes, fail, "string_bytes");
        if (keyBytes > limits.bytes - bytes) fail("bytes");
      }
      const fields = [...inspected.fields].sort((a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0);
      const output = create(null);
      append("{");
      for (let i = 0; i < fields.length; i++) {
        const field = fields[i];
        if (["__proto__", "prototype", "constructor"].includes(field.key)) fail("reserved_key");
        if (i && fields[i - 1].key === field.key) fail("inspection_failed");
        utf8Bytes(field.key, limits.string_bytes, fail, "string_bytes");
        if (i) append(",");
        append(quote(field.key));
        append(":");
        define(output, field.key, { value: copy(field.value, level + 1), enumerable: true, writable: false, configurable: false });
      }
      append("}");
      ancestors.delete(value);
      return freeze(output);
    }
    try {
      const value = copy(input, 1);
      return freeze({ status: "accepted", value, json: chunks.join(""), bytes, entries, depth });
    } catch (error) {
      if (error !== stopped) reason = "inspection_failed";
      return freeze({ status: "rejected", code: budgetReasons.has(reason) ? "data_budget_exceeded" : "invalid_data", reason });
    }
  }
  function checkTaskData(input, limits, inspect, validate) {
    const copied = copyTaskData(input, limits, inspect);
    if (copied.status !== "accepted") return copied;
    try {
      if (validate(copied.value) === true) return copied;
      return freeze({ status: "rejected", code: "invalid_data", reason: "schema_mismatch" });
    } catch {
      return freeze({ status: "rejected", code: "invalid_data", reason: "schema_unavailable" });
    }
  }
  return __toCommonJS(task_data_exports);
})();
)METON_TASK_DATA";
}
