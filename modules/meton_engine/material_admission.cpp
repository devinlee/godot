#include "material_admission.h"
#include "scene_surface.h"
#include "scene_input.h"
#include "texture_preparation.h"
#include "engine_contract.generated.hpp"
#include "core/crypto/crypto_core.h"
#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "core/version.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

namespace meton::bridge {
namespace {
constexpr uint64_t material_budget = 67108864;
struct Pin {
	HANDLE value = INVALID_HANDLE_VALUE;
	explicit Pin(HANDLE p_value) : value(p_value) {}
	~Pin() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
	Pin(const Pin &) = delete;
	Pin &operator=(const Pin &) = delete;
};
struct File {
	std::string digest, volume, id, bytes, prefix;
	uint64_t size = 0;
};
struct PreparedEdit {
	Dictionary route, command, checkpoint;
	std::string body;
	SceneTransformEdit transform;
	bool history = false;
	bool ready = true;
};
struct OpenGesture {
	Dictionary route, view, permit;
	SceneDragTarget target;
	CaptureStatus scope;
	uint64_t pointer = 0, sequence = 0;
	Point2 end;
	bool lifted = false;
};
using Pins = std::vector<std::unique_ptr<Pin>>;
struct DocumentMaterial {
	Pins pins;
	std::set<std::string> allowed;
	std::map<std::string, std::string> types;
	std::map<std::string, std::string> uids;
	std::map<std::string, std::vector<std::string>> references;
	TextureSources textures;
	std::map<std::string, std::string> texture_remaps;
	std::string key, seal_digest, main_scene, scene_bytes;
	String resource_root = "res://";
	Dictionary source, checkpoint, last_route, last_receipt;
	Size2i viewport_size;
};
struct State {
	std::wstring root;
	DocumentMaterial startup;
	DocumentMaterial *current = &startup;
	bool startup_open = true;
	std::map<std::string, std::shared_ptr<DocumentMaterial>> documents;
	std::shared_mutex documents_mutex;
	std::string producer_json;
	bool prepare_textures = false;
	bool check_only = false;
	bool active = false;
	std::optional<SceneSurfaceLaunch> surface;
	std::string hello_digest, project, generation, purpose, session, instance;
	Dictionary build, policy;
	Array capabilities;
	std::optional<PreparedEdit> pending;
	std::optional<OpenGesture> gesture;
	Dictionary preview_binding;
	bool query_enabled = false;
};
std::unique_ptr<State> state;
const char *failure = "unprepared";
const std::map<std::string, std::pair<std::string, std::string>> cache_indexes{
	{".godot/.gdignore", {"IgnoreMarker", "\n"}},
	{".godot/global_script_class_cache.cfg", {"EmptyScriptClasses", "list=[]\n"}},
	{".godot/scene_groups_cache.cfg", {"EmptySceneGroups", ""}},
};
const std::string uid_cache = ".godot/uid_cache.bin";
const std::set<std::string> cache_directories{".godot/imported", ".godot/editor", ".godot/shader_cache"};

bool check(bool condition, const char *reason) { if (!condition) failure = reason; return condition; }
std::string bytes(const String &text) { return text.utf8().get_data(); }
String text(const std::string &value) { return String::utf8(value.data(), value.size()); }
std::wstring wide(const String &value) { auto encoded = value.utf16(); return std::wstring(encoded.get_data(), encoded.get_data() + encoded.length()); }
std::string field(const Dictionary &dict, const char *key) {
	Variant value = dict.get(key, Variant());
	return value.get_type() == Variant::STRING ? bytes(value) : std::string();
}
bool shape(const Variant &value, std::initializer_list<const char *> required, std::initializer_list<const char *> optional = {}) {
	if (value.get_type() != Variant::DICTIONARY) return false;
	Dictionary dict = value;
	for (const char *key : required) if (!dict.has(key)) return false;
	for (const Variant *key = dict.next(nullptr); key; key = dict.next(key)) {
		if (key->get_type() != Variant::STRING) return false;
		const auto name = bytes(*key);
		if (std::none_of(required.begin(), required.end(), [&](const char *item) { return name == item; }) &&
				std::none_of(optional.begin(), optional.end(), [&](const char *item) { return name == item; })) return false;
	}
	return true;
}
bool hex(const std::string &value, size_t size) { return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool decimal(const std::string &value) {
	return !value.empty() && value.size() <= 20 && (value == "0" || value[0] != '0') &&
			value.find_first_not_of("0123456789") == std::string::npos && (value.size() < 20 || value <= "18446744073709551615");
}
bool id(const std::string &value) {
	const std::string first = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	return !value.empty() && value.size() <= 128 && first.find(value[0]) != std::string::npos &&
			value.find_first_not_of(first + "._:-") == std::string::npos;
}
bool integer(const Variant &value, uint64_t maximum, uint64_t &output) {
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) return false;
	double number = value;
	if (!std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) || std::floor(number) != number) return false;
	output = static_cast<uint64_t>(number); return true;
}
bool version(const Dictionary &dict) { uint64_t value; return integer(dict.get("format_version", Variant()), 1, value) && value == 1; }

/** 64 位身份来自规范十进制字符串，策略中的窄整数字段才经 JSON Number。 */
bool read_surface(const Variant &value, const std::string &generation, const std::string &source_version) {
	if (!shape(value, {"surface_id", "adapter_luid", "target_fps", "max_frame_width", "max_frame_height", "max_frame_slots", "max_gpu_bytes",
			"max_queue_messages", "operation_timeout_ms", "shutdown_timeout_ms"})) return false;
	const Dictionary fields = value;
	const auto surface = field(fields, "surface_id"), adapter = field(fields, "adapter_luid"), budget = field(fields, "max_gpu_bytes");
	if (!decimal(surface) || surface == "0" || !decimal(adapter) || adapter == "0" || !decimal(budget) || budget == "0") return false;
	const auto limit = [&](const char *key, std::uint32_t maximum, std::uint32_t &result) {
		uint64_t number = 0;
		if (!integer(fields[key], maximum, number) || !number) return false;
		result = static_cast<std::uint32_t>(number);
		return true;
	};
	SceneSurfaceLaunch launch;
	auto &context = launch.capture;
	context.follows_extent = true;
	context.instance_generation = std::stoull(generation);
	context.surface_id = std::stoull(surface);
	context.adapter_luid = std::stoull(adapter);
	context.limits.max_gpu_bytes = std::stoull(budget);
	if (!limit("target_fps", 1000, context.max_fps) || !limit("max_frame_width", UINT32_MAX, context.limits.max_width) ||
			!limit("max_frame_height", UINT32_MAX, context.limits.max_height) || !limit("max_frame_slots", 3, context.limits.max_slots) ||
			!limit("max_queue_messages", 4096, launch.transport.max_queue_messages) || !limit("operation_timeout_ms", UINT32_MAX, context.operation_timeout_ms) ||
			!limit("shutdown_timeout_ms", UINT32_MAX, context.shutdown_timeout_ms)) return false;
	launch.transport.operation_timeout_ms = context.operation_timeout_ms;
	launch.document_version = std::stoull(source_version);
	state->surface = launch;
	return true;
}
std::string lower(std::string value) {
	for (auto &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	return value;
}
/** Win32 别名和缓存路径不具有第二种拼写；准入路径始终相对于 content。 */
bool path(const std::string &value) {
	if (value.empty() || value.size() > 240 || value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-") != std::string::npos) return false;
	size_t at = 0;
	do {
		auto end = value.find('/', at); if (end == std::string::npos) end = value.size();
		auto part = lower(value.substr(at, end - at));
		if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
				(part.rfind(".godot", 0) == 0 && !(at == 0 && value.substr(0, end) == ".godot"))) return false;
		auto base = part.substr(0, part.find('.'));
		if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
				(base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) && base[3] >= '1' && base[3] <= '9')) return false;
		at = end + 1;
	} while (at <= value.size());
	return true;
}
/** .godot 的普通路径形状不是许可；此批只交付四份规范索引，其他缓存仍拒绝。 */
bool sealed_path(const std::string &value) {
	const std::string prefix = ".godot/imported/";
	const bool texture = value.size() == prefix.size() + 69 && value.rfind(prefix, 0) == 0 &&
			hex(value.substr(prefix.size(), 64), 64) && value.substr(value.size() - 5) == ".ctex";
	return path(value) && (value.rfind(".godot", 0) != 0 || cache_indexes.count(value) || value == uid_cache || texture);
}
bool suffix(const std::string &value, const char *ending) {
	const std::string tail = ending; return value.size() >= tail.size() && value.compare(value.size() - tail.size(), tail.size(), tail) == 0;
}
std::wstring joined(const std::wstring &root, const std::string &relative) {
	auto result = root + L"\\" + wide(text(relative)); std::replace(result.begin(), result.end(), L'/', L'\\'); return result;
}
std::string encode_hex(const uint8_t *data, size_t count) {
	std::string value; constexpr char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < count; ++i) { value += digits[data[i] >> 4]; value += digits[data[i] & 15]; }
	return value;
}
/** 父目录由 E01 持有；子端再拒绝重解析点并持有加载期间的不可替换对象。 */
bool pin_directory(Pins &pins, const std::wstring &name, bool root = false) {
	auto pin = std::make_unique<Pin>(CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | (root ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	FILE_ATTRIBUTE_TAG_INFO attr{};
	if (!check(pin->value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(pin->value, FileAttributeTagInfo, &attr, sizeof(attr)) &&
			(attr.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(attr.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "directory_identity")) return false;
	pins.push_back(std::move(pin)); return true;
}
/** 不经 Godot 的路径重映射或 Variant/Object 解析器读取原始材料。 */
bool read_file(Pins &pins, const std::wstring &name, uint64_t maximum, File &result, bool keep_bytes) {
	auto pin = std::make_unique<Pin>(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	FILE_ATTRIBUTE_TAG_INFO attr{}; FILE_STANDARD_INFO info{}; FILE_ID_INFO identity{};
	if (!check(pin->value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(pin->value, FileAttributeTagInfo, &attr, sizeof(attr)) &&
			GetFileInformationByHandleEx(pin->value, FileStandardInfo, &info, sizeof(info)) && GetFileInformationByHandleEx(pin->value, FileIdInfo, &identity, sizeof(identity)) &&
			!(attr.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.NumberOfLinks == 1 &&
			info.EndOfFile.QuadPart >= 0 && static_cast<uint64_t>(info.EndOfFile.QuadPart) <= maximum, "file_identity")) return false;
	result.size = info.EndOfFile.QuadPart; result.volume = std::to_string(identity.VolumeSerialNumber); result.id = encode_hex(identity.FileId.Identifier, 16);
	CryptoCore::SHA256Context hash; if (!check(hash.start() == OK, "hash_start")) return false;
	std::array<uint8_t, 65536> buffer{}; uint64_t total = 0;
	for (;;) {
		DWORD count = 0; if (!check(ReadFile(pin->value, buffer.data(), buffer.size(), &count, nullptr), "file_read")) return false;
		if (!count) break;
		if (!total) result.prefix.assign(reinterpret_cast<const char *>(buffer.data()), std::min<DWORD>(count, 24));
		if (!check(count <= result.size - total && hash.update(buffer.data(), count) == OK, "file_changed")) return false;
		total += count; if (keep_bytes) result.bytes.append(reinterpret_cast<const char *>(buffer.data()), count);
	}
	uint8_t digest[32];
	if (!check(total == result.size && hash.finish(digest) == OK, "file_size")) return false;
	result.digest = encode_hex(digest, 32); pins.push_back(std::move(pin)); return true;
}
bool json(const File &file, Variant &value) {
	String decoded; if (!check(decoded.append_utf8(file.bytes.data(), file.bytes.size()) == OK, "metadata_utf8")) return false;
	JSON parser; if (!check(parser.parse(decoded) == OK, "metadata_json")) return false;
	value = parser.get_data(); return true;
}
/** 保存状态和历史来自原文档所有者。资源对照包括同摘要多文件的个数。 */
bool admitted_checkpoint(DocumentMaterial &doc, const std::wstring &content, const Variant &value, const Dictionary &source, const std::string &main,
		const std::map<std::string, File> &actual, const Array &admitted) {
	if (!shape(value, {"checkpoint_id","digest","document_id","version","saved_version","history_cursor","history","resources","size_bytes"})) return false;
	const Dictionary cp = value;
	const auto version = field(cp,"version"), saved = field(cp,"saved_version"), cursor = field(cp,"history_cursor");
	const auto file = actual.find(main);
	if (!id(field(cp,"checkpoint_id")) || field(cp,"document_id") != field(source,"document_id") ||
			version != field(source,"source_version") || !decimal(saved) || std::stoull(saved) > std::stoull(version) || !decimal(cursor) ||
			file == actual.end() || field(cp,"digest") != file->second.digest || field(cp,"size_bytes") != std::to_string(file->second.size) ||
			cp["history"].get_type() != Variant::ARRAY || cp["resources"].get_type() != Variant::ARRAY) return false;
	const Array history = cp["history"], resources = cp["resources"];
	if (field(source,"purpose") == "preview" && (cursor != "0" || !history.is_empty())) return false;
	if (history.size() > 256 || resources.size() > 1024 || std::stoull(cursor) > static_cast<uint64_t>(history.size())) return false;
	for (int i = 0; i < history.size(); ++i) {
		if (!shape(history[i], {"history_id","before_checkpoint_id","after_checkpoint_id","operation_id"})) return false;
		const Dictionary entry = history[i];
		for (const char *key : {"history_id","before_checkpoint_id","after_checkpoint_id","operation_id"}) if (!id(field(entry,key))) return false;
	}
	std::multiset<std::pair<std::string,std::string>> remaining;
	for (int i = 0; i < admitted.size(); ++i) {
		const Dictionary row = admitted[i]; const auto path = field(row,"path"), kind = field(row,"kind");
		if (path != main && (kind == "scene" || kind == "resource" || kind == "png")) {
			const auto &ref = actual.at(path); remaining.emplace(ref.digest, std::to_string(ref.size));
		}
	}
	std::set<std::string> ids;
	for (int i = 0; i < resources.size(); ++i) {
		if (!shape(resources[i], {"resource_id","digest","size_bytes"})) return false;
		const Dictionary ref = resources[i]; const auto key = field(ref,"resource_id");
		const auto found = remaining.find({field(ref,"digest"),field(ref,"size_bytes")});
		if (!id(key) || !ids.insert(key).second || found == remaining.end()) return false;
		remaining.erase(found);
	}
	if (!remaining.empty()) return false;
	if (field(source,"purpose") == "preview") {
		doc.checkpoint = cp;
		return true;
	}
	// 原文件已被固定。只保留主场景，资源继续由原检查点和不可变文件持有。
	File body;
	if (!read_file(doc.pins, joined(content, main), 1048576, body, true) ||
			body.digest != file->second.digest || body.id != file->second.id || body.volume != file->second.volume) return false;
	doc.scene_bytes = std::move(body.bytes);
	doc.checkpoint = cp; return true;
}
bool identity(const Variant &value) {
	if (!shape(value, {"volume_serial", "file_id"})) return false;
	Dictionary dict = value; return decimal(field(dict, "volume_serial")) && hex(field(dict, "file_id"), 32);
}
/** 严格闭包连目录项一起核对，未登记的配置、脚本、导入缓存和空目录都不能混入。 */
bool inventory(Pins &pins, const std::wstring &directory, const std::string &prefix, const std::set<std::string> &expected, bool launch) {
	if (!pin_directory(pins, directory)) return false;
	WIN32_FIND_DATAW item{}; HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &item);
	if (search == INVALID_HANDLE_VALUE) return check(false, "inventory_open");
	bool ok = true;
	do {
		std::wstring name = item.cFileName; if (name == L"." || name == L"..") continue;
		std::string relative = prefix + bytes(String(name.c_str()));
		if (!path(relative) || (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = false; break; }
		if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			auto start = expected.lower_bound(relative + "/");
			if (((start == expected.end() || start->rfind(relative + "/", 0) != 0) && !(launch && cache_directories.count(relative))) ||
					!inventory(pins, directory + L"\\" + name, relative + "/", expected, launch)) { ok = false; break; }
		} else if (!expected.count(relative)) { ok = false; break; }
	} while (FindNextFileW(search, &item));
	DWORD last = GetLastError(); FindClose(search);
	return check(ok && last == ERROR_NO_MORE_FILES, "inventory_extra");
}
bool canonical_uid(const std::string &uid) {
	if (uid.rfind("uid://", 0) != 0 || uid.size() < 7 || uid.size() > 19) return false;
	std::string body = uid.substr(6); if (body.size() > 1 && body[0] == 'a') return false;
	uint64_t value = 0;
	for (char c : body) {
		int digit = c >= 'a' && c <= 'y' ? c - 'a' : c >= '0' && c <= '8' ? c - '0' + 25 : -1;
		if (digit < 0 || value > (0x7fffffffffffffffULL - digit) / 34) return false;
		value = value * 34 + digit;
	}
	return true;
}
/** 与锁定版 ResourceUID::encode_binary_cache 同一格式，不调用未受限的缓存解析器。 */
std::string canonical_uid_cache(const DocumentMaterial &doc) {
	std::string result;
	auto append = [&](uint64_t value, int count) { for (int i = 0; i < count; ++i) { result.push_back(static_cast<char>(value & 255)); value >>= 8; } };
	append(doc.uids.size(), 4);
	for (const auto &entry : doc.uids) {
		uint64_t value = 0;
		for (char c : entry.first.substr(6)) value = value * 34 + (c >= 'a' && c <= 'y' ? c - 'a' : c - '0' + 25);
		const auto resource = "res://" + entry.second;
		append(value, 8); append(resource.size(), 4); result += resource;
	}
	return result;
}
bool arguments(const List<String> &args, const std::string &purpose) {
	std::set<std::string> used; bool project = false, editor = false;
	for (auto item = args.front(); item; item = item->next()) {
		const auto key = bytes(item->get()); if (!used.insert(key).second) return check(false, "arguments_duplicate");
		if (key == "--headless" || key == "--no-header" || key == "--verbose" || key == "--quit") continue;
		if (key == "--editor") { editor = true; continue; }
		if (key == "--meton-prepare-png" && purpose == "editor") { state->prepare_textures = true; continue; }
#ifdef METON_CONTENT_TOOL_ENABLED
		if (key == "--meton-check-materials") { state->check_only = true; continue; }
#endif
		if (key != "--path" && key != "--log-file" && key != "--quit-after" && key != "--rendering-driver" && key != "--rendering-method" && key != "--audio-driver")
			return check(false, "arguments_forbidden");
		item = item->next(); if (!item) return check(false, "arguments_value");
		const auto value = bytes(item->get());
		if (key == "--path") { if (wide(item->get().replace("/", "\\")) != state->root + L"\\content") return check(false, "arguments_project"); project = true; }
		if (key == "--log-file" && wide(item->get().replace("/", "\\")) != state->root + L"\\logs\\godot.log") return check(false, "arguments_log");
		if (key == "--quit-after" && (!decimal(value) || value == "0" || value.size() > 3)) return check(false, "arguments_frames");
		if ((key == "--rendering-driver" && value != "d3d12") || (key == "--rendering-method" && value != "forward_plus") ||
				(key == "--audio-driver" && value != "Dummy")) return check(false, "arguments_driver");
	}
#ifdef METON_CONTENT_TOOL_ENABLED
	// 内容工具只能在已封印的无窗口代次执行一项工作，不能进入编辑会话或试播主循环。
	if (!check(purpose == "editor" && used.count("--headless") && state->prepare_textures != state->check_only, "content_tool_task")) return false;
#endif
	return check(project && editor == (purpose == "editor"), "arguments_purpose");
}
/** 诊断只写本代固定日志根；不回传原场景正文、原路径或进程能力。 */
void report(bool accepted, const std::string &control) {
	std::string data = "{\"format_version\":1,\"accepted\":" + std::string(accepted ? "true" : "false") +
			",\"reason\":\"" + (accepted ? "admitted" : failure) + "\",\"files\":" + std::to_string(state->startup.allowed.size()) +
			(control.empty() ? "" : ",\"control\":\"" + control + "\"") + "}";
	Pin file(CreateFileW((state->root + L"\\logs\\admission.json").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (file.value != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file.value, data.data(), data.size(), &written, nullptr); FlushFileBuffers(file.value); }
}
/** 固定 PNG 除源摘要以外还需限制像素规模，避免导入器为小文件分配过大图像。 */
bool png_header(const File &file) {
	const auto &s = file.prefix;
	if (file.size > 16777216 || s.size() != 24 || s.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 || s.substr(12, 4) != "IHDR") return false;
	auto dimension = [&](size_t offset) {
		uint32_t size = 0;
		for (size_t index = offset; index < offset + 4; ++index) size = (size << 8) | static_cast<unsigned char>(s[index]);
		return size > 0 && size <= 4096;
	};
	return dimension(16) && dimension(20);
}
/** 消费者只接受来自已固定生产者的回执。逐源比对独立摘要并重建 sidecar 后，才登记内部重映射。 */
bool verify_textures(DocumentMaterial &doc, bool preparing, const Dictionary &seal, const Dictionary &source, const std::map<std::string, File> &actual,
		const std::map<std::string, std::vector<std::string>> &derived) {
	if (doc.textures.empty() || preparing) {
		return check(derived.empty() && !seal.has("import_receipt_digest") && (!preparing || !doc.textures.empty()), "import_phase");
	}
	auto receipt_file = actual.find("meton-imports.json");
	if (!check(derived.count("meton-imports.json") && receipt_file != actual.end() && receipt_file->second.digest == field(seal, "import_receipt_digest") &&
			derived.size() == 1 + 2 * doc.textures.size(), "import_receipt")) return false;
	Variant parsed;
	if (!json(receipt_file->second, parsed) || !check(shape(parsed, {"format_version","recipe","producer","files"}), "import_receipt_shape")) return false;
	Dictionary receipt = parsed;
	if (!check(version(receipt) && field(receipt, "recipe") == "godot-texture2d-lossless-v1" &&
			shape(receipt["producer"], {"project_id","document_id","source_version","purpose","build"}) && receipt["files"].get_type() == Variant::ARRAY, "import_receipt_shape")) return false;
	Dictionary producer = receipt["producer"];
	for (const char *key : {"project_id","document_id","source_version"}) if (!check(field(producer, key) == field(source, key), "import_source")) return false;
	if (!check(field(producer, "purpose") == "editor" && shape(producer["build"], {"source_sha","build_id","engine_digest","host_digest","policy_digest","protocol_version","platform","graphics"}), "import_producer")) return false;
	Dictionary from = producer["build"], consuming = source["build"];
	for (const char *key : {"source_sha","host_digest","policy_digest","protocol_version","platform","graphics"}) if (!check(field(from, key) == field(consuming, key), "import_producer")) return false;
	if (!check(hex(field(from,"engine_digest"), 64) && id(field(from,"build_id")), "import_producer")) return false;
	if (field(source,"purpose") == "editor") {
		bool paired = field(from,"engine_digest") == field(consuming,"engine_digest") && field(from,"build_id") == field(consuming,"build_id");
#ifdef METON_IMPORT_TOOL_SHA256
#define METON_TOOL_STR_INNER(value) #value
#define METON_TOOL_STR(value) METON_TOOL_STR_INNER(value)
		// 独立工具的原代回执摘要已由宿主固定，只允许编译进本消费组合的确切生产者。
		paired = paired || field(from,"engine_digest") == METON_TOOL_STR(METON_IMPORT_TOOL_SHA256);
#undef METON_TOOL_STR
#undef METON_TOOL_STR_INNER
#endif
		if (!check(paired, "import_producer")) return false;
	}
	Array files = receipt["files"];
	if (!check(size_t(files.size()) == doc.textures.size(), "import_count")) return false;
	for (int i = 0; i < files.size(); ++i) {
		if (!check(shape(files[i], {"path","source_digest","digest","size_bytes"}), "import_file")) return false;
		Dictionary fact = files[i];
		const auto path_name = field(fact,"path");
		const auto src = doc.textures.find(path_name);
		if (!check(src != doc.textures.end() && field(fact,"source_digest") == src->second.digest, "import_source")) return false;
		const auto cache = texture_cache_path(path_name), sidecar = path_name + ".import";
		const auto blob = actual.find(cache), side = actual.find(sidecar);
		uint64_t size;
		if (!check(derived.count(cache) && derived.count(sidecar) && derived.at(sidecar) == std::vector<std::string>{cache} &&
				blob != actual.end() && side != actual.end() && blob->second.digest == field(fact,"digest") &&
				integer(fact["size_bytes"], material_budget, size) && size > 0 && blob->second.size == size && blob->second.prefix.substr(0,4) == "GST2", "import_cache")) return false;
		if (!check(side->second.bytes == texture_sidecar(path_name, src->second, cache), "import_sidecar")) return false;
		if (!check(doc.texture_remaps.emplace(path_name, cache).second, "import_duplicate")) return false;
	}
	return true;
}
/** 启动原稿和后续文档使用同一份文件、依赖、导入和检查点核验。句柄归当前文档持有。 */
bool verify_content(DocumentMaterial &doc, const std::wstring &content, const Dictionary &seal, const std::string &purpose, bool preparing, bool launch) {
	if (!check(seal["files"].get_type() == Variant::ARRAY, "seal_files")) return false;
	Array files = seal["files"]; if (!check(files.size() >= 3 && files.size() <= 256, "seal_files")) return false;
	std::map<std::string, File> actual; std::set<std::string> names, folded, identities; uint64_t total = 0;
	if (!pin_directory(doc.pins, content)) return false;
	for (int i = 0; i < files.size(); ++i) {
		if (!check(shape(files[i], {"path", "digest", "size_bytes", "identity"}), "seal_file_shape")) return false;
		Dictionary entry = files[i]; std::string name = field(entry, "path"); uint64_t size;
		if (!check(sealed_path(name) && hex(field(entry, "digest"), 64) && integer(entry["size_bytes"], material_budget - total, size) &&
				identity(entry["identity"]) && folded.insert(lower(name)).second, "seal_file_fields")) return false;
		// 先固定所有中间目录，文件打开时不经过尚未核对的重解析点。
		size_t at = 0; while ((at = name.find('/', at)) != std::string::npos) {
			if (!pin_directory(doc.pins, joined(content, name.substr(0, at++)))) return false;
		}
		File file; bool keep = name == "meton-admission.json" || name == "project.godot" || cache_indexes.count(name) || name == uid_cache || name == "meton-imports.json" || suffix(name, ".import");
		if (keep && !check(size <= 1048576, "metadata_budget")) return false;
		if (!read_file(doc.pins, joined(content, name), material_budget - total, file, keep)) return false;
		Dictionary stamp = entry["identity"];
		if (!check(file.size == size && file.digest == field(entry, "digest") && file.volume == field(stamp, "volume_serial") && file.id == field(stamp, "file_id") &&
				identities.insert(file.volume + ":" + file.id).second, "copied_file_mismatch")) return false;
		total += size; actual.emplace(name, std::move(file)); names.insert(name);
	}
	if (!inventory(doc.pins, content, "", names, launch)) return false;
	if (launch) for (const auto &directory : cache_directories) if (!pin_directory(doc.pins, joined(content, directory))) return false;
	auto admission_file = actual.find("meton-admission.json");
	if (!check(admission_file != actual.end() && admission_file->second.digest == field(seal, "admission_digest"), "admission_digest")) return false;
	Variant manifest_value; if (!json(admission_file->second, manifest_value)) return false;
	if (!check(shape(manifest_value, {"format_version", "profile", "source", "main_scene", "viewport", "files"}, {"phase","checkpoint"}), "admission_shape")) return false;
	Dictionary manifest = manifest_value;
	if (!check(manifest.has("phase") == preparing && (!preparing || field(manifest, "phase") == "prepare_png"), "import_phase")) return false;
	if (!check(version(manifest) && field(manifest, "profile") == "gw1-pure-data-v1" &&
			shape(manifest["source"], {"project_id", "document_id", "source_version", "purpose", "build"}) &&
			shape(manifest["viewport"], {"width", "height"}) && manifest["files"].get_type() == Variant::ARRAY, "admission_fields")) return false;
	Dictionary source = manifest["source"], viewport = manifest["viewport"];
	if (!check(id(field(source, "project_id")) && id(field(source, "document_id")) && decimal(field(source, "source_version")) && field(source, "purpose") == purpose &&
			shape(source["build"], {"source_sha", "build_id", "engine_digest", "host_digest", "policy_digest", "protocol_version", "platform", "graphics"}), "admission_source")) return false;
	Dictionary build = source["build"];
	if (!launch && !check(field(source, "project_id") == state->project && state->build.recursive_equal(build, 0) && manifest.has("checkpoint"), "document_source")) return false;
	if (!check(field(build, "source_sha") == GODOT_VERSION_HASH && id(field(build, "build_id")) &&
			field(build, "engine_digest") == field(seal, "engine_digest") && field(build, "host_digest") == field(seal, "host_digest") &&
			field(build, "policy_digest") == field(seal, "policy_digest") && field(build, "protocol_version") == "0.1.0" &&
			field(build, "platform") == "windows-x64" && field(build, "graphics") == "d3d12", "admission_build")) return false;
	uint64_t width, height;
	if (!check(integer(viewport["width"], 4096, width) && width > 0 && integer(viewport["height"], 4096, height) && height > 0, "admission_viewport")) return false;
	Array admitted = manifest["files"];
	if (!check(admitted.size() + 1 == files.size(), "admission_count")) return false;
	std::map<std::string, std::vector<std::string>> dependencies; std::set<std::string> source_identities;
	std::string main_scene = field(manifest, "main_scene"); bool main = false, project = false;
	std::set<std::string> metadata;
	std::map<std::string, std::vector<std::string>> derived;
	for (int i = 0; i < admitted.size(); ++i) {
		if (!check(shape(admitted[i], {"path", "kind", "digest", "size_bytes", "type", "dependencies"}, {"identity", "uid"}), "admitted_file_shape")) return false;
		Dictionary entry = admitted[i]; std::string name = field(entry, "path"), kind = field(entry, "kind"), type = field(entry, "type"); uint64_t size;
		auto found = actual.find(name);
		if (!check(found != actual.end() && name != "meton-admission.json" && field(entry, "digest") == found->second.digest &&
				integer(entry["size_bytes"], material_budget, size) && size == found->second.size && entry["dependencies"].get_type() == Variant::ARRAY &&
				!dependencies.count(name), "admitted_file_mismatch")) return false;
		if (kind == "scene") { if (!check(name.rfind(".godot/", 0) != 0 && suffix(name, ".tscn") && type == "PackedScene", "admitted_type")) return false; if (name == main_scene) main = true; }
		else if (kind == "resource") {
			const std::set<std::string> types{"BoxMesh", "PlaneMesh", "QuadMesh", "SphereMesh", "StandardMaterial3D", "Environment", "Curve", "CurveTexture"};
			if (!check(name.rfind(".godot/", 0) != 0 && suffix(name, ".tres") && types.count(type), "admitted_type")) return false;
		} else if (kind == "png") {
			if (!check(suffix(name, ".png") && name.size() <= 233 && type == "Texture2D" && png_header(found->second), "admitted_png")) return false;
			doc.textures.emplace(name, TextureSource{found->second.digest, field(entry, "uid")});
		} else if (kind == "project") {
			if (!check(name == "project.godot" && type == "ProjectSettings" && !entry.has("identity") && !entry.has("uid"), "admitted_project")) return false;
			project = true;
		} else if (kind == "cache_index" || kind == "uid_cache") {
			if (!check(!entry.has("identity") && !entry.has("uid") && (kind == "cache_index" ?
					cache_indexes.count(name) && type == cache_indexes.at(name).first && found->second.bytes == cache_indexes.at(name).second :
					name == uid_cache && type == "ResourceUID"), "cache_index")) return false;
			metadata.insert(name);
		} else if (kind == "import_receipt" || kind == "import_cache" || kind == "import_sidecar") {
			const bool valid = (kind == "import_receipt" && name == "meton-imports.json" && type == "TextureImportReceipt") ||
					(kind == "import_cache" && name.rfind(".godot/imported/",0) == 0 && type == "CompressedTexture2D") ||
					(kind == "import_sidecar" && suffix(name, ".png.import") && type == "Texture2DRemap");
			if (!check(valid && !entry.has("identity") && !entry.has("uid"), "import_role")) return false;
			derived.emplace(name, std::vector<std::string>{});
		} else return check(false, "import_not_prepared");
		if (kind == "scene" || kind == "resource" || kind == "png") {
			if (!check(entry.has("identity") && identity(entry["identity"]), "admitted_identity")) return false;
			Dictionary original = entry["identity"];
			if (!check(source_identities.insert(field(original, "volume_serial") + ":" + field(original, "file_id")).second, "admitted_identity_alias")) return false;
		}
		if (entry.has("uid")) {
			auto uid = field(entry, "uid");
			if (!check(canonical_uid(uid) && doc.uids.emplace(uid, name).second, "admitted_uid")) return false;
		}
		Array refs = entry["dependencies"]; if (!check(refs.size() <= 256 && (kind != "cache_index" || refs.is_empty()), "admitted_dependencies")) return false;
		if (!check((kind != "png" && kind != "import_receipt" && kind != "import_cache") || refs.is_empty(), "admitted_dependencies")) return false;
		std::set<std::string> unique; std::vector<std::string> edges;
		for (int j = 0; j < refs.size(); ++j) {
			if (!check(refs[j].get_type() == Variant::STRING, "admitted_dependency")) return false;
			auto target = bytes(refs[j]);
			const bool target_role = kind == "import_sidecar" ? target.rfind(".godot/imported/", 0) == 0 :
					target != "project.godot" && target != "meton-admission.json" && target != "meton-imports.json" && !suffix(target, ".import") && target.rfind(".godot/", 0) != 0;
			if (!check(target_role && actual.count(target) && unique.insert(target).second, "admitted_dependency")) return false;
			edges.push_back(target);
		}
		if (derived.count(name)) derived[name] = edges;
		dependencies.emplace(name, std::move(edges));
		if (kind == "scene" || kind == "resource" || kind == "png") {
			doc.allowed.insert(name);
			doc.types.emplace(name, type);
		}
	}
	if (!check(main && project && dependencies["project.godot"] == std::vector<std::string>{main_scene}, "admitted_entry")) return false;
	if (!verify_textures(doc, preparing, seal, source, actual, derived)) return false;
	if (preparing) state->producer_json = bytes(JSON::stringify(source));
	std::vector<std::string> uid_paths; for (const auto &entry : doc.uids) uid_paths.push_back(entry.second);
	if (!check(metadata.size() == cache_indexes.size() + 1 && metadata.count(uid_cache) && actual[uid_cache].bytes == canonical_uid_cache(doc) &&
			dependencies[uid_cache] == uid_paths, "uid_cache")) return false;
	std::set<std::string> reached; std::vector<std::string> pending{"project.godot"};
	for (const auto &name : metadata) pending.push_back(name);
	for (const auto &entry : derived) pending.push_back(entry.first);
	while (!pending.empty()) {
		auto item = pending.back(); pending.pop_back(); if (!reached.insert(item).second) continue;
		// UID 索引只是来源闭包的派生，不得让一个原本不可达的资源因列入缓存而获得加载资格。
		if (item != uid_cache) for (const auto &target : dependencies[item]) pending.push_back(target);
	}
	if (!check(reached.size() == dependencies.size(), "admitted_closure")) return false;
	// 按入度去除边，未能全部去除说明清单存在循环；不能让解析器递归试探它。
	std::map<std::string, unsigned> inbound; for (const auto &entry : dependencies) inbound[entry.first] = 0;
	for (const auto &entry : dependencies) for (const auto &target : entry.second) ++inbound[target];
	pending.clear(); for (const auto &entry : inbound) if (entry.second == 0) pending.push_back(entry.first);
	size_t removed = 0; while (!pending.empty()) { auto item = pending.back(); pending.pop_back(); ++removed; for (const auto &target : dependencies[item]) if (--inbound[target] == 0) pending.push_back(target); }
	if (!check(removed == dependencies.size(), "admitted_cycle")) return false;
	const std::string config = "config_version=5\n\n[application]\nconfig/name=\"Meton Controlled Scene\"\nrun/main_scene=\"res://" + main_scene +
			"\"\n\n[display]\nwindow/size/viewport_width=" + std::to_string(width) + "\nwindow/size/viewport_height=" + std::to_string(height) +
			"\n\n[rendering]\nrenderer/rendering_method=\"forward_plus\"\n";
	if (!check(actual["project.godot"].bytes == config, "project_configuration")) return false;
	if (manifest.has("checkpoint") && !check(!preparing && (!launch || seal.has("control")) &&
			admitted_checkpoint(doc, content, manifest["checkpoint"], source, main_scene, actual, admitted), "document_checkpoint")) return false;
	doc.source = source;
	doc.main_scene = main_scene;
	doc.viewport_size = Size2i(width, height);
	doc.references = std::move(dependencies);
	return true;
}
bool verify(const List<String> &args, const std::string &seal_digest) {
	if (!check(hex(seal_digest, 64), "seal_missing")) return false;
	File seal_file; if (!read_file(state->startup.pins, state->root + L"\\execution-seal.json", 1048576, seal_file, true)) return false;
	if (!check(seal_file.digest == seal_digest, "seal_digest")) return false;
	Variant seal_value; if (!json(seal_file, seal_value)) return false;
	if (!check(shape(seal_value, {"format_version", "instance_generation", "purpose", "host_digest", "engine_digest", "policy_digest", "protocol_digest", "admission_digest", "files"}, {"import_receipt_digest", "surface", "control", "document_key"}), "seal_shape")) return false;
	Dictionary seal = seal_value;
	std::string purpose = field(seal, "purpose"), generation = field(seal, "instance_generation");
	if (!check(version(seal) && decimal(generation) && generation != "0" && (purpose == "editor" || purpose == "preview") &&
			hex(field(seal, "host_digest"), 64) && hex(field(seal, "engine_digest"), 64) && hex(field(seal, "policy_digest"), 64) &&
			hex(field(seal, "admission_digest"), 64) && field(seal, "protocol_digest") == meton::engine::contract::schema_digest, "seal_context")) return false;
	// 用途必须绑定当前编译配置，不能拿编辑器 EXE 冒充受限试播，或反向混用。
#ifdef TOOLS_ENABLED
	if (!check(purpose == "editor", "seal_profile")) return false;
#else
	if (!check(purpose == "preview", "seal_profile")) return false;
#endif
	if (!arguments(args, purpose)) return false;
	auto executable = wide(OS::get_singleton()->get_executable_path().replace("/", "\\"));
	if (!check(executable.rfind(state->root + L"\\runtime\\", 0) == 0, "engine_location") || !pin_directory(state->startup.pins, state->root + L"\\runtime")) return false;
	File engine; if (!read_file(state->startup.pins, executable, 1073741824, engine, false) || !check(engine.digest == field(seal, "engine_digest"), "engine_digest")) return false;
	if (!verify_content(state->startup, state->root + L"\\content", seal, purpose, state->prepare_textures, true)) return false;
	auto &doc = state->startup;
	const Dictionary &source = doc.source;
	doc.seal_digest = seal_digest;
	if (seal.has("document_key")) {
		doc.key = field(seal, "document_key");
		if (!check(hex(doc.key, 64) && purpose == "editor" && seal.has("control") && !doc.checkpoint.is_empty(), "document_binding")) return false;
	}
	if (seal.has("surface") && !check(!state->prepare_textures && !args.find("--headless") &&
			read_surface(seal["surface"], generation, field(source, "source_version")), "surface_launch_binding")) return false;
	if (state->surface) {
		state->surface->scene_path = text("res://" + doc.main_scene);
		state->surface->viewport_size = doc.viewport_size;
	}
	if (seal.has("control")) {
		if (!check(state->surface.has_value() && shape(seal["control"], {"request_digest", "policy"}), "scene_control_seal")) return false;
		const Dictionary control = seal["control"];
		state->hello_digest = field(control, "request_digest");
		if (!check(hex(state->hello_digest, 64) && control["policy"].get_type() == Variant::DICTIONARY, "scene_control_policy")) return false;
		state->policy = control["policy"];
		if (!state->current->checkpoint.is_empty()) {
			uint64_t maximum = 0, inflight = 0;
			if (!check(integer(state->policy.get("max_message_bytes", Variant()),1048576,maximum) && maximum &&
					integer(state->policy.get("max_inflight_requests",Variant()),64,inflight) && inflight,"document_budget")) return false;
			state->surface->transport.max_document_bytes = static_cast<uint32_t>(maximum);
			state->surface->transport.max_document_requests = static_cast<uint32_t>(inflight);
		}
		state->build = source["build"]; state->generation = generation; state->purpose = purpose; state->project = field(source, "project_id");
		state->surface->transport.allow_scene_hello = true;
	}
	return true;
}

/** G01 时间与 JS toISOString 一致；比较墙钟毫秒，两条独立期限都须有效。 */
bool future_utc(const std::string &value, uint32_t max_ms = 0) {
	if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
			value[16] != ':' || value[19] != '.' || value[23] != 'Z') return false;
	SYSTEMTIME date{};
	const auto number = [&](size_t pos, size_t count, WORD &out) {
		unsigned n = 0;
		for (size_t i = pos; i < pos + count; ++i) {
			if (value[i] < '0' || value[i] > '9') return false;
			n = n * 10 + static_cast<unsigned>(value[i] - '0');
		}
		out = static_cast<WORD>(n); return true;
	};
	FILETIME due{}, now{};
	if (!number(0,4,date.wYear) || !number(5,2,date.wMonth) || !number(8,2,date.wDay) || !number(11,2,date.wHour) ||
			!number(14,2,date.wMinute) || !number(17,2,date.wSecond) || !number(20,3,date.wMilliseconds) || !SystemTimeToFileTime(&date, &due)) return false;
	GetSystemTimeAsFileTime(&now);
	const uint64_t end = (uint64_t(due.dwHighDateTime) << 32) | due.dwLowDateTime;
	const uint64_t start = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	return end > start && (!max_ms || end - start <= uint64_t(max_ms) * 10000);
}

/** 手势与命令共用同一张原视口许可，许可的截止不能超出租约。 */
bool write_permit(const Dictionary &route, const Dictionary &permit) {
	const Dictionary binding = route["binding"];
	if (!shape(permit,{"permit_id","project_id","document_id","view_id","lease_id","document_version","instance_generation","lease_generation","expires_at"}) ||
			!id(field(permit,"permit_id")) || field(permit,"document_id") != field(route,"document_id") || field(permit,"view_id") != field(route,"view_id") ||
			field(permit,"document_version") != field(state->current->checkpoint,"version") || !future_utc(field(permit,"expires_at")) ||
			field(permit,"expires_at") > field(binding,"lease_expires_at")) return false;
	for (const char *key : {"project_id","lease_id","lease_generation","instance_generation"}) if (field(permit,key) != field(binding,key)) return false;
	return true;
}

/** 原路由已经由受限宿主封装。清理只解除这一租约的尚未提交手势。 */
const char *gesture_input(const Dictionary &route, const Dictionary &view, const Dictionary &packet) {
	if (!state->capabilities.has("scene.gesture") || field(route,"access") != "edit" || !id(field(route,"view_id")) ||
			!shape(view,{"view_id","document_id","document_version","view_generation"}) || field(view,"view_id") != field(route,"view_id") ||
			field(view,"document_id") != field(route,"document_id") || !decimal(field(view,"document_version")) ||
			!decimal(field(view,"view_generation")) || field(view,"view_generation") == "0") return "permission_denied";
	if (!shape(packet,{"kind","request_id","deadline_at","sequence","frame_sequence","intent","input"},{"permit"}) ||
			field(packet,"kind") != "input" || !decimal(field(packet,"sequence")) || field(packet,"sequence") == "0" ||
			!decimal(field(packet,"frame_sequence")) || packet["input"].get_type() != Variant::DICTIONARY) return "invalid_argument";
	const Dictionary input = packet["input"];
	auto &open = state->gesture;
	if (field(input,"kind") == "reset") {
		const auto reason = field(input,"reason");
		if (field(packet,"intent") != "observe" || !shape(input,{"kind","reason"}) || packet.has("permit") || (reason != "blur" && reason != "occluded" && reason != "stopping" &&
				reason != "superseded" && reason != "cancelled")) return "invalid_argument";
		if (state->pending && state->pending->route.recursive_equal(route,0) && field(state->pending->command["change"],"kind") == "gesture") return "recovery_required";
		if (open && open->route.recursive_equal(route,0)) {
			if (!open->view.recursive_equal(view,0)) return "stale_generation";
			open.reset();
		}
		return nullptr;
	}
	if (field(packet,"intent") != "edit" || field(view,"document_version") != field(state->current->checkpoint,"version") || !packet.has("permit") || packet["permit"].get_type() != Variant::DICTIONARY ||
			!write_permit(route,packet["permit"]) || !future_utc(field(packet["permit"],"expires_at"),state->surface->capture.operation_timeout_ms)) return "permission_denied";
	uint64_t pointer, x, y, buttons;
	if (!shape(input,{"kind","phase","pointer_id","x","y","buttons","modifiers"}) || field(input,"kind") != "pointer" ||
			!integer(input["pointer_id"],UINT32_MAX,pointer) || !integer(input["x"],UINT32_MAX,x) || !integer(input["y"],UINT32_MAX,y) ||
			!integer(input["buttons"],1,buttons) || input["modifiers"].get_type() != Variant::ARRAY || !Array(input["modifiers"]).is_empty()) return "invalid_argument";
	const auto phase = field(input,"phase");
	if ((phase != "down" && phase != "up") || buttons != (phase == "down" ? 1u : 0u)) return "invalid_argument";
	CaptureStatus current;
	const auto sequence = std::stoull(field(packet,"sequence"));
	if (!gesture_frame(std::stoull(field(packet,"frame_sequence")), std::stoull(field(view,"document_version")),x,y,current)) return "stale_generation";
	if (phase == "down") {
		if (open && !future_utc(field(open->permit,"expires_at"))) open.reset();
		if (open || state->pending) return "budget_exceeded";
		OpenGesture next;
		if (const char *error = begin_scene_drag(route["document_id"], Point2(x,y), next.target)) return error;
		if (!state->capabilities.has(next.target.camera.is_valid() ? "scene.transform3d" : "scene.transform2d")) return "permission_denied";
		next.route = route; next.view = view; next.permit = packet["permit"]; next.pointer = pointer; next.sequence = sequence; next.scope = current;
		open = std::move(next);
	} else {
		if (!open || open->pointer != pointer || open->lifted || sequence <= open->sequence || !open->route.recursive_equal(route,0) ||
				!open->view.recursive_equal(view,0) || !open->permit.recursive_equal(packet["permit"],0) || !gesture_scope_current(open->scope)) return "stale_generation";
		Dictionary transform;
		if (const char *error = finish_scene_drag(open->target,Point2(x,y),transform)) return error;
		open->end = Point2(x,y); open->sequence = sequence; open->lifted = true;
	}
	return nullptr;
}

/** 每次处理写命令都核对原操作、原视口和原代次。 */
bool same_operation(const Dictionary &a, const Dictionary &b) {
	return field(a,"operation_id") == field(b,"operation_id") && field(a,"params_digest") == field(b,"params_digest");
}
bool pending_for(const Dictionary &route, const Dictionary &op) {
	return state->pending && state->pending->route.recursive_equal(route,0) && same_operation(state->pending->command,op);
}
/** 原引擎可证明未改动树时，保留原操作的终态供 A09 核对。 */
void remember_uncommitted(const Dictionary &route, const Dictionary &command, const char *status, const char *error) {
	Dictionary receipt;
	for (const char *key : {"operation_id","params_digest","document_id"}) receipt[key] = command[key];
	for (const char *key : {"version","saved_version","checkpoint_id"}) receipt[key] = state->current->checkpoint[key];
	receipt["status"] = status; receipt["commit_state"] = "not_committed";
	receipt["receipt_id"] = "receipt:" + (String(command["operation_id"]) + "\n" + String(command["params_digest"])).sha256_text();
	if (error) { Dictionary detail; detail["code"] = error; receipt["error"] = detail; }
	state->current->last_route = route; state->current->last_receipt = receipt;
}
static void prepared_reply(const Dictionary &command, Dictionary &result) {
	result["kind"] = "prepared"; result["operation_id"] = command["operation_id"]; result["params_digest"] = command["params_digest"];
	result["checkpoint"] = state->pending->checkpoint;
}
/** 只收原操作当前游标的相邻检查点。分片阶段不解析场景、触碰编辑树或推进事件。 */
static const char *prepare_history(const Dictionary &route, const Dictionary &command, const Dictionary &change, Dictionary &result) {
	if (!shape(change,{"kind"}) || !shape(command.get("history_material",Variant()),{"history_id","digest","size_bytes","chunk"})) return "invalid_argument";
	const Dictionary material = command["history_material"];
	if (!shape(material["chunk"],{"range","hex"})) return "invalid_argument";
	const Dictionary chunk = material["chunk"];
	if (!shape(chunk["range"],{"operation_id","params_digest","checkpoint_id","offset","length"})) return "invalid_argument";
	const Dictionary range = chunk["range"];
	const auto size_text = field(material,"size_bytes"), digest = field(material,"digest"), encoded = field(chunk,"hex");
	uint64_t offset = 0, length = 0;
	if (!same_operation(command,range) || !decimal(size_text) || !hex(digest,64) ||
			!integer(range["offset"],1048575,offset) || !integer(range["length"],32768,length) || !length || !hex(encoded,length*2)) return "invalid_argument";
	const uint64_t size = std::stoull(size_text);
	if (!size || size > 1048576 || size > std::stoull(field(state->policy,"max_snapshot_bytes")) || offset+length > size) return "budget_exceeded";
	const int cursor = int(std::stoull(field(state->current->checkpoint,"history_cursor")));
	const bool undo = field(change,"kind") == "undo";
	const Array rows = state->current->checkpoint["history"];
	const int index = undo ? cursor-1 : cursor;
	if (index < 0 || index >= rows.size()) return "version_conflict";
	const Dictionary row = rows[index];
	if (field(row,"history_id") != field(material,"history_id") ||
			field(row,undo ? "after_checkpoint_id" : "before_checkpoint_id") != field(state->current->checkpoint,"checkpoint_id") ||
			field(row,undo ? "before_checkpoint_id" : "after_checkpoint_id") != field(range,"checkpoint_id")) return "version_conflict";
	if (const char *error = check_scene_history_target()) return error;
	Dictionary original = command.duplicate(); original.erase("history_material");
	if (!state->pending) {
		if (offset || field(state->current->last_receipt,"operation_id") == field(command,"operation_id")) return "version_conflict";
		PreparedEdit edit;
		edit.history = true; edit.ready = false; edit.command = original; edit.route = route; edit.body.reserve(size);
		edit.checkpoint = state->current->checkpoint.duplicate(true);
		auto &cp = edit.checkpoint;
		cp["checkpoint_id"] = range["checkpoint_id"]; cp["digest"] = material["digest"]; cp["size_bytes"] = material["size_bytes"];
		cp["history_cursor"] = String::num_int64(undo ? cursor-1 : cursor+1);
		cp["version"] = String::num_uint64(std::stoull(field(state->current->checkpoint,"version"))+1);
		state->pending = std::move(edit);
	}
	auto &edit = *state->pending;
	if (!edit.history || !pending_for(route,command) || !edit.command.recursive_equal(original,0) ||
			field(edit.checkpoint,"digest") != digest || field(edit.checkpoint,"size_bytes") != size_text) return "version_conflict";
	if (edit.ready) {
		if (offset+length != size || encode_hex(reinterpret_cast<const uint8_t *>(edit.body.data()+offset),length) != encoded) return "version_conflict";
		prepared_reply(command,result); return nullptr;
	}
	if (edit.body.size() != offset) return "version_conflict";
	const auto nibble = [](char c) -> unsigned { return c <= '9' ? c-'0' : c-'a'+10; };
	for (size_t i=0; i<encoded.size(); i+=2) edit.body.push_back(char((nibble(encoded[i])<<4) | nibble(encoded[i+1])));
	if (edit.body.size() < size) { result["kind"] = "ack"; return nullptr; }
	uint8_t hash[32];
	if (CryptoCore::sha256(reinterpret_cast<const uint8_t *>(edit.body.data()),size,hash) != OK || encode_hex(hash,32) != digest) {
		remember_uncommitted(route,original,"rejected","invalid_argument"); state->pending.reset(); return "invalid_argument";
	}
	edit.ready = true; prepared_reply(command,result); return nullptr;
}
const char *prepare_edit(const Dictionary &route, const Dictionary &command, const std::string &capability, Dictionary &result) {
	if (!shape(command,{"method","operation_id","params_digest","document_id","expected_version","permit","change"},{"history_material"}) ||
			!id(field(command,"operation_id")) || !hex(field(command,"params_digest"),64) || command["change"].get_type() != Variant::DICTIONARY ||
			!shape(command["permit"],{"permit_id","project_id","document_id","view_id","lease_id","document_version","instance_generation","lease_generation","expires_at"})) return "invalid_argument";
	const Dictionary permit = command["permit"], change = command["change"];
	const auto version = field(state->current->checkpoint,"version"), kind = field(change,"kind");
	if (field(command,"expected_version") != version || version == "18446744073709551615") return "version_conflict";
	if (field(command,"document_id") != field(route,"document_id") || !write_permit(route,permit)) return "permission_denied";
	Dictionary effective = change;
	if (state->gesture && !future_utc(field(state->gesture->permit,"expires_at"))) state->gesture.reset();
	if (state->gesture && kind != "gesture") return "version_conflict";
	if (kind == "undo" || kind == "redo") return capability == "scene.history" ? prepare_history(route,command,change,result) : "permission_denied";
	if (command.has("history_material")) return "invalid_argument";
	if ((kind != "gesture" && kind != "transform2d" && kind != "transform3d") || capability != "scene." + kind) return "not_supported";
	if (kind == "gesture") {
		if (!shape(change,{"kind","gesture_id","view_id"}) || field(change,"gesture_id") != field(permit,"permit_id") ||
				field(change,"view_id") != field(route,"view_id")) return "permission_denied";
		if (!state->pending) {
			const auto &open = state->gesture;
			if (!open || !open->lifted || !open->route.recursive_equal(route,0) || !open->permit.recursive_equal(permit,0) ||
					!gesture_scope_current(open->scope)) return "permission_denied";
			effective = Dictionary();
			if (const char *error = finish_scene_drag(open->target,open->end,effective)) return error;
		}
	} else if (!id(field(change,"node_id")) || !(kind == "transform2d" ? shape(change,{"kind","node_id","position","rotation","scale"}) :
			shape(change,{"kind","node_id","position","rotation_quaternion","scale"}))) return "invalid_argument";
	if (state->pending) {
		if (!pending_for(route,command) || !state->pending->command.recursive_equal(command,0)) return "version_conflict";
	} else {
		if (field(state->current->last_receipt,"operation_id") == field(command,"operation_id")) return "version_conflict";
		PreparedEdit edit;
		String candidate;
		if (const char *error = prepare_scene_transform(text(state->current->scene_bytes), route["document_id"], effective, edit.transform, candidate,kind == "gesture")) {
			if (std::string(error) == "invalid_argument") remember_uncommitted(route,command,"rejected",error);
			return error;
		}
		edit.body = bytes(candidate);
		const int cursor = static_cast<int>(std::stoull(field(state->current->checkpoint,"history_cursor")));
		if (edit.body.empty() || edit.body.size() > 1048576 || cursor >= 256) {
			remember_uncommitted(route,command,"rejected","budget_exceeded"); return "budget_exceeded";
		}
		edit.command = command; edit.route = route; edit.checkpoint = state->current->checkpoint.duplicate(true);
		Dictionary &cp = edit.checkpoint;
		Array history = cp["history"];
		history.resize(cursor);
		const String token = (String(route["document_id"]) + "\n" + String(cp["checkpoint_id"]) + "\n" + String(command["operation_id"]) + "\n" + String(command["params_digest"])).sha256_text();
		Dictionary row; row["before_checkpoint_id"] = cp["checkpoint_id"]; row["after_checkpoint_id"] = "checkpoint:" + token;
		row["history_id"] = "history:" + token; row["operation_id"] = command["operation_id"]; history.push_back(row);
		cp["checkpoint_id"] = row["after_checkpoint_id"]; cp["digest"] = candidate.sha256_text(); cp["size_bytes"] = String::num_uint64(edit.body.size());
		cp["version"] = String::num_uint64(std::stoull(version) + 1); cp["history_cursor"] = String::num_int64(history.size());
		state->pending = std::move(edit);
		if (kind == "gesture") state->gesture.reset();
	}
	prepared_reply(command,result);
	return nullptr;
}
/** 分片只来自内存中已冻结的候选。偏移按 UTF-8 字节计数。 */
const char *export_edit(const Dictionary &route, const Dictionary &command, Dictionary &result) {
	if (!shape(command,{"method","document_id","expected_version"},{"candidate"}) || field(command,"document_id") != field(route,"document_id")) return "invalid_argument";
	if (!command.has("candidate")) {
		if (field(command,"expected_version") != field(state->current->checkpoint,"version")) return "version_conflict";
		result["kind"] = "snapshot"; result["checkpoint"] = state->current->checkpoint; return nullptr;
	}
	if (!shape(command["candidate"],{"operation_id","params_digest","checkpoint_id","offset","length"})) return "invalid_argument";
	const Dictionary range = command["candidate"];
	if (!pending_for(route,range)) return "permission_denied";
	const auto &edit = *state->pending;
	if (!edit.ready) return "version_conflict";
	const Dictionary permit = edit.command["permit"];
	if (!future_utc(field(permit,"expires_at"))) return "scope_ended";
	if (field(range,"checkpoint_id") != field(edit.checkpoint,"checkpoint_id") || field(command,"expected_version") != field(edit.checkpoint,"version")) return "version_conflict";
	uint64_t offset = 0, length = 0;
	if (!integer(range["offset"],1048575,offset) || !integer(range["length"],32768,length) || !length || offset + length > edit.body.size()) return "invalid_argument";
	Dictionary chunk; chunk["range"] = range; chunk["hex"] = text(encode_hex(reinterpret_cast<const uint8_t *>(edit.body.data()+offset),length));
	result["kind"] = "snapshot"; result["checkpoint"] = edit.checkpoint; result["chunk"] = chunk;
	return nullptr;
}
/** 只接受 A09 原持久操作所对应的完整检查点和回执；重复确认不再推进事件或树。 */
const char *confirm_edit(const Dictionary &route, const Dictionary &command, Dictionary &result) {
	if (!shape(command,{"method","operation_id","params_digest","checkpoint","receipt"}) ||
			!shape(command["receipt"],{"operation_id","params_digest","status","commit_state","document_id","version","checkpoint_id","receipt_id","saved_version"}) ||
			command["checkpoint"].get_type() != Variant::DICTIONARY) return "invalid_argument";
	const Dictionary cp = command["checkpoint"], receipt = command["receipt"];
	if (!same_operation(command,receipt) || !id(field(receipt,"receipt_id")) || field(receipt,"commit_state") != "committed" ||
			field(receipt,"document_id") != field(route,"document_id")) return "version_conflict";
	for (const char *key : {"document_id","version","saved_version","checkpoint_id"}) if (field(cp,key) != field(receipt,key)) return "version_conflict";
	if (state->current->last_route.recursive_equal(route,0) && state->current->last_receipt.recursive_equal(receipt,0) && state->current->checkpoint.recursive_equal(cp,0)) {
		result["kind"] = "ack"; return nullptr;
	}
	if (field(receipt,"status") == "saved") {
		auto saved = state->current->checkpoint.duplicate(true); saved["saved_version"] = saved["version"];
		if (state->pending || !id(field(command,"operation_id")) || !hex(field(command,"params_digest"),64) || !saved.recursive_equal(cp,0)) return "version_conflict";
		state->current->checkpoint = saved;
	} else {
		if (field(receipt,"status") != "committed" || !pending_for(route,command) || !state->pending->ready || !state->pending->checkpoint.recursive_equal(cp,0)) return "version_conflict";
		const auto version = std::stoull(field(cp,"version"));
		const char *error = state->pending->history ? confirm_scene_history(state->pending->body,version) : confirm_scene_transform(state->pending->transform,version);
		if (error) return error;
		state->current->checkpoint = state->pending->checkpoint; state->current->scene_bytes = std::move(state->pending->body); state->pending.reset();
	}
	state->current->last_route = route; state->current->last_receipt = receipt; result["kind"] = "ack"; return nullptr;
}
}

std::string material_hello(const std::string &body, bool &compatible) {
	if (!state || !state->active || state->hello_digest.empty()) return {};
	std::uint8_t digest[32]; Variant parsed;
	if (CryptoCore::sha256(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), digest) != OK ||
			encode_hex(digest, 32) != state->hello_digest) return {};
	File request; request.bytes = body;
	if (!json(request, parsed) || !shape(parsed, {"protocol","binding","packet"})) return {};
	Dictionary message = parsed;
	if (field(message,"protocol") != "meton.engine.host/1" ||
			!shape(message["binding"], {"project_id","session_id","instance_id","lease_id","instance_generation","lease_generation","purpose","lease_expires_at"}) ||
			!shape(message["packet"], {"kind","request_id","deadline_at","capability","command"})) return {};
	Dictionary binding = message["binding"], packet = message["packet"];
	const auto lease_generation = field(binding,"lease_generation");
	if (field(binding,"project_id") != state->project || field(binding,"purpose") != state->purpose ||
			field(binding,"instance_generation") != state->generation || !decimal(lease_generation) || lease_generation == "0" ||
			!id(field(binding,"session_id")) || !id(field(binding,"instance_id")) || !id(field(binding,"lease_id")) ||
			!future_utc(field(binding,"lease_expires_at")) || !future_utc(field(packet,"deadline_at")) ||
			field(packet,"kind") != "request" || field(packet,"capability") != "session.control" || !id(field(packet,"request_id")) ||
			!shape(packet["command"], {"method","build","policy","capabilities"})) return {};
	Dictionary command = packet["command"];
	if (field(command,"method") != "session.hello" || command["capabilities"].get_type() != Variant::ARRAY ||
			command["build"].get_type() != Variant::DICTIONARY || command["policy"].get_type() != Variant::DICTIONARY ||
			!state->build.recursive_equal(command["build"], 0) || !state->policy.recursive_equal(command["policy"], 0)) return {};
	const Array requested = command["capabilities"];
	if (requested.size() > 9 || !requested.has("session.control")) return {};
	// 只公布本构建已接通的 G01 方法。后续文档命令落实后在它们的实现处扩充能力表。
	Array supported; supported.push_back("session.control");
	const bool frozen_preview = state->purpose == "preview" && !state->current->checkpoint.is_empty();
	if (frozen_preview && !requested.has("preview.run")) return {};
	if (frozen_preview) supported.push_back("preview.run");
	if (state->purpose == "editor" && !state->current->checkpoint.is_empty() && requested.has("scene.query")) supported.push_back("scene.query");
	if (supported.has("scene.query") && requested.has("scene.checkpoint")) {
		supported.push_back("scene.checkpoint");
		for (const char *cap : {"scene.transform2d","scene.transform3d","scene.gesture","scene.history"}) if (requested.has(cap)) supported.push_back(cap);
	}
	compatible = requested.size() == supported.size();
	for (int i = 0; i < requested.size(); ++i) if (!supported.has(requested[i]) || requested.count(requested[i]) != 1) compatible = false;
	state->session = field(binding,"session_id"); state->instance = field(binding,"instance_id");
	state->query_enabled = compatible && supported.has("scene.query");
	if (compatible) state->capabilities = supported;
	if (compatible && frozen_preview) state->preview_binding = binding;
	Dictionary result; result["kind"] = "hello"; result["build"] = state->build; result["capabilities"] = supported;
	Dictionary reply; reply["kind"] = "reply"; reply["request_id"] = packet["request_id"]; reply["method"] = "session.hello"; reply["result"] = result;
	Dictionary envelope; envelope["protocol"] = "meton.engine.host/1"; envelope["binding"] = binding; envelope["packet"] = reply;
	return bytes(JSON::stringify(envelope));
}

bool awaiting_preview_start() {
	return state && !state->preview_binding.is_empty() && state->current->last_receipt.is_empty();
}

namespace {
String scene_path(const DocumentMaterial &doc) { return doc.resource_root + text(doc.main_scene); }
std::string document_key(const std::string &type, const std::string &document) {
	if (!id(type) || !id(document)) return {};
	// 三个 ID 均使用 G01 的 ASCII 字符集，与 G02 的 JSON 数组编码逐字节相同。
	return bytes(text("[\"" + state->project + "\",\"" + type + "\",\"" + document + "\"]").sha256_text());
}
DocumentMaterial *document_for_slot(const std::string &slot) {
	if (slot == "launch") return state->startup_open ? &state->startup : nullptr;
	const auto found = state->documents.find(slot);
	return found == state->documents.end() ? nullptr : found->second.get();
}
/** 原进程按受信槽号重开材料，先核父、子封印及逐文件复制身份，再容许 Godot 解析。 */
std::shared_ptr<DocumentMaterial> read_document_slot(const std::string &slot, const std::string &key, const std::string &digest) {
	auto doc = std::make_shared<DocumentMaterial>();
	const auto parent = state->root + L"\\content\\.godot\\meton-documents";
	const auto root = joined(parent, slot);
	File seal_file;
	Variant parsed;
	if (!pin_directory(doc->pins, parent) || !pin_directory(doc->pins, root) ||
			!read_file(doc->pins, root + L"\\execution-seal.json", 1048576, seal_file, true) || seal_file.digest != digest ||
			!json(seal_file, parsed) || !shape(parsed, {"format_version","slot","generation","launch_seal_digest","document_key","admission_digest","files"}, {"import_receipt_digest"})) return {};
	Dictionary seal = parsed;
	if (!version(seal) || field(seal, "generation") != state->generation || field(seal, "slot") != slot ||
			field(seal, "launch_seal_digest") != state->startup.seal_digest || field(seal, "document_key") != key || !hex(field(seal,"admission_digest"),64)) return {};
	for (const char *name : {"host_digest","engine_digest","policy_digest"}) seal[name] = state->build[name];
	if (!verify_content(*doc, root + L"\\content", seal, "editor", false, false)) return {};
	doc->resource_root = text("res://.godot/meton-documents/" + slot + "/content/");
	doc->key = key; doc->seal_digest = digest;
	return doc;
}
const char *activate_document(DocumentMaterial &doc) {
	if (&doc != state->current && (state->pending || state->gesture)) return "version_conflict";
	if (const char *error = activate_document_tab(scene_path(doc), doc.viewport_size, std::stoull(field(doc.checkpoint, "version")))) return error;
	state->current = &doc;
	return nullptr;
}
/** E01 私有生命周期控制与 A09 的业务通道分开。正文和持久操作均不经过这个端点。 */
std::string document_lifecycle(const std::string &body) {
	if (!state->query_enabled || state->purpose != "editor" || state->startup.key.empty() || body.size() > 2048) return "denied";
	File request; request.bytes = body;
	Variant parsed;
	if (!json(request, parsed) || !shape(parsed, {"action","slot","document_key","seal_digest"}, {"document_type","document_id"})) return "denied";
	const Dictionary command = parsed;
	const auto action = field(command,"action"), slot = field(command,"slot"), key = field(command,"document_key"), digest = field(command,"seal_digest");
	if ((slot != "launch" && !hex(slot,32)) || !hex(key,64) || !hex(digest,64)) return "denied";
	DocumentMaterial *doc = document_for_slot(slot);
	if (doc && (doc->key != key || doc->seal_digest != digest)) return "denied";
	if (action == "close") {
		if (!doc) return "closed";
		if (doc == state->current) {
			if (state->pending || state->gesture) return "busy";
			DocumentMaterial *next = doc != &state->startup && state->startup_open ? &state->startup : nullptr;
			if (!next) for (const auto &item : state->documents) {
				if (item.second.get() != doc) { next = item.second.get(); break; }
			}
			if (next && activate_document(*next)) return "busy";
		}
		if (close_document_tab(scene_path(*doc))) return "busy";
		if (state->current == doc) state->current = &state->startup;
		if (slot == "launch") state->startup_open = false;
		else { std::unique_lock<std::shared_mutex> lock(state->documents_mutex); state->documents.erase(slot); }
		return "closed";
	}
	const std::string original = field(command, "document_id");
	if ((action != "open" && action != "activate") || document_key(field(command,"document_type"),original) != key) return "denied";
	bool inserted = false;
	if (!doc) {
		if (slot == "launch" || (state->startup_open && state->startup.key == key)) return "denied";
		for (const auto &item : state->documents) if (item.second->key == key) return "busy";
		auto opened = read_document_slot(slot, key, digest);
		if (!opened || field(opened->source,"document_id") != original) return "denied";
		doc = opened.get();
		std::unique_lock<std::shared_mutex> lock(state->documents_mutex);
		state->documents.emplace(slot, std::move(opened));
		inserted = true;
	}
	if (field(doc->source,"document_id") != original) return "denied";
	if (open_document_tab(scene_path(*doc))) {
		if (inserted) { std::unique_lock<std::shared_mutex> lock(state->documents_mutex); state->documents.erase(slot); }
		return "denied";
	}
	if (action == "activate" && activate_document(*doc)) return "busy";
	return action == "activate" ? "active" : "opened";
}
}
String material_editor_scene_path() { return state ? scene_path(*state->current) : String(); }

/** 对同一冻结快照只接受一次启动。重查同一操作只返回原回执，不能替换当前场景。 */
static const char *preview_command(const Dictionary &route, const Dictionary &packet, const Dictionary &command, Dictionary &result) {
	const auto method = field(command,"method"), capability = field(packet,"capability");
	if (method == "preview.start") {
		if (capability != "preview.run") return "permission_denied";
		if (!shape(command,{"method","operation_id","params_digest","snapshot"}) || !id(field(command,"operation_id")) ||
				!hex(field(command,"params_digest"),64)) return "invalid_argument";
		if (!same_operation(route["preview_operation"],command) || !state->current->checkpoint.recursive_equal(command["snapshot"],0)) return "version_conflict";
		if (state->current->last_receipt.is_empty()) {
			Dictionary receipt;
			receipt["operation_id"] = command["operation_id"]; receipt["params_digest"] = command["params_digest"];
			receipt["status"] = "accepted"; receipt["commit_state"] = "not_committed";
			for (const char *key : {"document_id","version","saved_version","checkpoint_id"}) receipt[key] = state->current->checkpoint[key];
			state->current->last_route = route; state->current->last_receipt = receipt;
		}
	} else if (method == "operation.query" || method == "operation.cancel") {
		if (capability != "session.control") return "permission_denied";
		if (!shape(command,{"method","operation_id","params_digest"}) || !id(field(command,"operation_id")) || !hex(field(command,"params_digest"),64)) return "invalid_argument";
	} else return "permission_denied";
	if (state->current->last_receipt.is_empty() || !state->current->last_route.recursive_equal(route,0)) return "recovery_required";
	if (!same_operation(state->current->last_receipt,command)) return method == "preview.start" ? "version_conflict" : "recovery_required";
	result["kind"] = "receipt"; result["receipt"] = state->current->last_receipt;
	return nullptr;
}

std::string material_document(const std::string &route_text, const std::string &body) {
	if (!materials_active() || (!state->query_enabled && state->preview_binding.is_empty())) return "{}";
	if (route_text == "meton.document.lifecycle/1") return document_lifecycle(body);
	File route_file, request; route_file.bytes = route_text; request.bytes = body;
	Variant parsed_route, parsed;
	if (!json(route_file, parsed_route) || !shape(parsed_route, {"binding","document_id","access"},{"view_id","preview_operation","document_type","document_slot"}) ||
			!json(request, parsed) || !shape(parsed, {"protocol","binding","packet"},{"view"})) return "{}";
	Dictionary route = parsed_route, message = parsed;
	if (!shape(route["binding"], {"project_id","session_id","instance_id","lease_id","instance_generation","lease_generation","purpose","lease_expires_at"}) ||
			message["packet"].get_type() != Variant::DICTIONARY) return "{}";
	Dictionary binding = route["binding"], packet = message["packet"];
	if (!id(field(packet,"request_id"))) return "{}";
	const auto access = field(route,"access"), document = field(route,"document_id");
	DocumentMaterial *target = &state->startup;
	if (!state->startup.key.empty() || route.has("document_type") || route.has("document_slot")) {
		target = document_for_slot(field(route,"document_slot"));
		if (target && (target->key.empty() || document_key(field(route,"document_type"),document) != target->key)) target = nullptr;
	}
	Dictionary result; const char *error = nullptr;
	if (message["binding"].get_type() != Variant::DICTIONARY || !binding.recursive_equal(message["binding"],0) ||
			field(binding,"project_id") != state->project || field(binding,"session_id") != state->session || field(binding,"instance_id") != state->instance ||
			field(binding,"instance_generation") != state->generation || field(binding,"purpose") != state->purpose ||
			!id(field(binding,"lease_id")) || !decimal(field(binding,"lease_generation")) || field(binding,"lease_generation") == "0" ||
			!target || field(target->checkpoint,"document_id") != document || (access != "observe" && access != "edit")) error = "permission_denied";
	else if (state->purpose == "preview" && (!state->preview_binding.recursive_equal(binding,0) || access != "observe" || route.has("view_id") ||
			!route.has("preview_operation") || !shape(route["preview_operation"],{"operation_id","params_digest"}))) error = "permission_denied";
	else if (state->purpose != "preview" && route.has("preview_operation")) error = "permission_denied";
	else if (!future_utc(field(binding,"lease_expires_at"))) error = "scope_ended";
	else if (!future_utc(field(packet,"deadline_at"))) error = "deadline_exceeded";
	if (field(message,"protocol") == "meton.engine.surface/1" && message.has("view") && message["view"].get_type() == Variant::DICTIONARY) {
		if (!error) error = state->purpose == "preview" || target != state->current ? "permission_denied" : gesture_input(route,message["view"],packet);
		Dictionary reply; reply["kind"] = "ack"; reply["request_id"] = packet["request_id"]; reply["status"] = error ? "rejected" : "accepted";
		if (error) { Dictionary detail; detail["code"] = error; reply["error"] = detail; }
		message["binding"] = binding; message["packet"] = reply;
		return bytes(JSON::stringify(message));
	}
	if (field(message,"protocol") != "meton.engine.host/1" || message.has("view") ||
			!shape(packet, {"kind","request_id","deadline_at","capability","command"}) || field(packet,"kind") != "request" ||
			packet["command"].get_type() != Variant::DICTIONARY) return "{}";
	Dictionary command = packet["command"];
	const auto method = field(command,"method");
	if (!error && target != state->current && method != "document.query" && method != "document.open") error = "permission_denied";
	if (state->purpose == "preview") {
		if (!error) error = preview_command(route,packet,command,result);
	} else if (!error && method == "document.open") {
		if (!state->capabilities.has("scene.checkpoint") || field(packet,"capability") != "scene.checkpoint" ||
				!shape(command,{"method","snapshot","access"}) || field(command,"access") != (access == "edit" ? "write" : "read")) error = "permission_denied";
		else if (!target->checkpoint.recursive_equal(command["snapshot"],0)) error = "version_conflict";
	} else if (!error && method != "document.query" && method != "document.select") {
		const auto cap = field(packet,"capability");
		if (!state->capabilities.has("scene.checkpoint")) error = "not_supported";
		else if (access != "edit" || !id(field(route,"view_id")) || !state->capabilities.has(text(cap))) error = "permission_denied";
		else if (method == "document.prepare") error = prepare_edit(route,command,cap,result);
		else if (method == "document.export" && cap == "scene.checkpoint") error = export_edit(route,command,result);
		else if (method == "document.confirm" && cap == "scene.checkpoint") error = confirm_edit(route,command,result);
		else if (method == "document.abort" && cap == "scene.checkpoint") {
			const auto reason = field(command,"reason");
			if (!shape(command,{"method","operation_id","params_digest","document_id","reason"}) ||
					(reason != "cancelled" && reason != "commit_failed" && reason != "scope_ended") || field(command,"document_id") != document ||
					!pending_for(route,command)) error = "version_conflict";
			else {
				remember_uncommitted(route,command,"cancelled",nullptr); state->pending.reset(); result["kind"] = "ack";
			}
		} else if ((method == "operation.query" || method == "operation.cancel") && cap == "session.control" &&
				shape(command,{"method","operation_id","params_digest"})) {
			if (method == "operation.cancel" && pending_for(route,command)) {
				remember_uncommitted(route,state->pending->command,"cancelled",nullptr); state->pending.reset();
			}
			if (state->current->last_route.recursive_equal(route,0) && same_operation(state->current->last_receipt,command)) {
				result["kind"] = "receipt"; result["receipt"] = state->current->last_receipt;
			} else error = "recovery_required";
		} else error = "not_supported";
	}
	else if (!error && (field(packet,"capability") != "scene.query" || field(command,"document_id") != document)) error = "permission_denied";
	else if (!error && method == "document.query" && (!shape(command, {"method","document_id","after_event"}) || !decimal(field(command,"after_event")))) error = "invalid_argument";
	else if (!error) {
		if (method == "document.select") {
			if (access != "edit" || !id(field(route,"view_id")) || !state->capabilities.has("scene.checkpoint")) error = "permission_denied";
			else if (!shape(command,{"method","document_id","expected_version","node_ids"}) || command["node_ids"].get_type() != Variant::ARRAY ||
					Array(command["node_ids"]).size() > 64) error = "invalid_argument";
			else if (field(command,"expected_version") != field(state->current->checkpoint,"version") || state->pending || state->gesture) error = "version_conflict";
		}
	}
	if (!error && state->purpose == "editor" && (method == "document.query" || method == "document.select" || method == "document.open")) {
		Dictionary projection, selection = command;
		if (method == "document.open") { selection = Dictionary(); selection["after_event"] = "0"; }
		if (target == state->current) error = scene_document_selection(selection, text(document), projection);
		else error = inactive_document_selection(scene_path(*target), text(document), selection["after_event"], projection);
		if (!error) {
			for (const char *key : {"document_id","version","saved_version","history_cursor"}) projection[key] = target->checkpoint[key];
			projection["checkpoint"] = target->checkpoint; projection["recovery"] = "ready";
			result["kind"] = "state"; result["state"] = projection;
		}
	}
	if (error) { Dictionary detail; detail["code"] = error; result["kind"] = "error"; result["error"] = detail; }
	Dictionary reply; reply["kind"] = "reply"; reply["request_id"] = packet["request_id"]; reply["method"] = command["method"]; reply["result"] = result;
	Dictionary envelope; envelope["protocol"] = "meton.engine.host/1"; envelope["binding"] = binding; envelope["packet"] = reply;
	return bytes(JSON::stringify(envelope));
}

bool admit_materials(const List<String> &arguments) {
	if (state) return false;
	if (arguments.size() == 1 && arguments.front()->get() == "--version") return true;
#ifdef METON_SURFACE_FIXTURE_ENABLED
	// 历史 B2 的固定原生场景仅存在于明确的测试构建；正式构建没有此放行分支。
	if (!OS::get_singleton()->has_environment("METON_ENGINE_LAUNCH_DIGEST")) return true;
#endif
	state = std::make_unique<State>();
	state->root = wide(OS::get_singleton()->get_environment("METON_ENGINE_ROOT"));
	if (state->root.size() < 4 || state->root.size() > 2048 || state->root[1] != L':' || state->root[2] != L'\\') return false;
	std::vector<wchar_t> normalized(32768); DWORD length = GetFullPathNameW(state->root.c_str(), normalized.size(), normalized.data(), nullptr);
	if (!length || length >= normalized.size() || state->root != normalized.data()) return false;
	Pin token(nullptr); DWORD app = 0, size = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) || !GetTokenInformation(token.value, TokenIsAppContainer, &app, sizeof(app), &size) || app != 1) return false;
	if (!pin_directory(state->startup.pins, state->root, true)) return false;
	bool accepted = verify(arguments, bytes(OS::get_singleton()->get_environment("METON_ENGINE_LAUNCH_DIGEST")));
	state->active = accepted;
	std::string control;
	if (accepted && !state->hello_digest.empty()) {
		uint64_t timeout = 0;
		// 材料、实际构建和受限身份已经核对；握手无需等待昂贵的冷着色器编译或首帧。
		const auto result = integer(state->policy.get("handshake_timeout_ms", Variant()), UINT32_MAX, timeout) && timeout
				? start_scene_control(*state->surface, static_cast<DWORD>(timeout)) : SceneHandshake::rejected;
		control = result == SceneHandshake::ready ? "reply_queued" : result == SceneHandshake::incompatible ? "incompatible" : "rejected";
		state->active = result == SceneHandshake::ready;
	}
	report(accepted, control); return state->active;
}
bool materials_active() { return state && state->active; }
const SceneSurfaceLaunch *material_surface() { return materials_active() && state->surface ? &*state->surface : nullptr; }
String material_user_directory() { return String(state->root.c_str()).replace("\\", "/").path_join("user"); }
String material_cache_directory() { return String(state->root.c_str()).replace("\\", "/").path_join("cache"); }
bool material_storage_matches() {
	const String root = String(state->root.c_str()).replace("\\", "/");
	const String user = ProjectSettings::get_singleton()->globalize_path("user://").trim_suffix("/");
	const String cache = OS::get_singleton()->get_cache_path(), settings = OS::get_singleton()->get_config_path();
	if (user == root.path_join("user") && cache == root.path_join("cache") && settings == root.path_join("settings")) return true;
	// 此时项目日志尚未创建。诊断只写代内相对目录，外部路径不进入日志。
	auto local = [&](const String &p) { return p.begins_with(root + "/") ? bytes(p.substr(root.length() + 1)) : std::string("outside"); };
	const std::string diagnostic = "user=" + local(user) + "\ncache=" + local(cache) + "\nsettings=" + local(settings) + "\n";
	Pin file(CreateFileW((state->root + L"\\logs\\storage.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (file.value != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file.value, diagnostic.data(), static_cast<DWORD>(diagnostic.size()), &written, nullptr); }
	return false;
}
bool preparing_material_textures() { return materials_active() && state->prepare_textures; }
bool checking_materials_only() { return materials_active() && state->check_only; }
#ifdef TOOLS_ENABLED
bool run_material_texture_preparation() { return preparing_material_textures() && prepare_texture_files(state->root, state->producer_json, state->startup.textures); }
#endif
void install_material_uids() {
	if (!materials_active()) return;
	auto registry = ResourceUID::get_singleton();
	for (const auto &entry : state->startup.uids) registry->add_id(registry->text_to_id(text(entry.first)), text("res://" + entry.second));
}
namespace {
const std::string documents_prefix = "res://.godot/meton-documents/";
/** 物理路径只能由宿主槽号和已验相对路径构成。读取者在完成本次查找前保持材料所有权。 */
std::shared_ptr<DocumentMaterial> mounted_path(const std::string &value, std::string &relative) {
	const size_t at = documents_prefix.size();
	if (value.compare(0, at, documents_prefix) != 0 || value.size() <= at + 40 || value.compare(at + 32, 9, "/content/") != 0) return {};
	const std::string slot = value.substr(at, 32);
	std::shared_lock<std::shared_mutex> lock(state->documents_mutex);
	const auto found = state->documents.find(slot);
	if (found == state->documents.end()) return {};
	relative = value.substr(at + 41);
	return found->second;
}
}
bool material_external_resource(const String &owner, const String &original, const String &type, const String &uid, String &resolved) {
	if (!materials_active()) return false;
	std::string source = bytes(owner), relative;
	const auto mounted = mounted_path(source, relative);
	const DocumentMaterial *doc = mounted.get();
	if (!doc) {
		if (source.compare(0, documents_prefix.size(), documents_prefix) == 0) return true;
		if (state->startup.key.empty() || source.rfind("res://", 0) != 0) return false;
		relative = source.substr(6);
		if (!state->startup.allowed.count(relative)) return false;
		doc = &state->startup;
	}
	if (!original.begins_with("res://")) return true;
	const std::string target = bytes(original.trim_prefix("res://"));
	const auto edges = doc->references.find(relative);
	const auto kind = doc->types.find(target);
	if (edges == doc->references.end() || kind == doc->types.end() || kind->second != bytes(type) ||
			std::find(edges->second.begin(), edges->second.end(), target) == edges->second.end()) return true;
	if (!uid.is_empty()) {
		const auto found = doc->uids.find(bytes(uid));
		if (found == doc->uids.end() || found->second != target) return true;
	}
	resolved = doc->resource_root + text(target);
	return true;
}
String material_mounted_texture(const String &resource) {
	if (!materials_active()) return {};
	std::string relative;
	const auto doc = mounted_path(bytes(resource), relative);
	if (!doc) return {};
	const auto found = doc->texture_remaps.find(relative);
	return found == doc->texture_remaps.end() ? String() : doc->resource_root + text(found->second);
}
bool material_resource_allowed(const String &resource) {
	if (!materials_active()) return true;
	auto name = bytes(resource);
	if (name.compare(0, documents_prefix.size(), documents_prefix) == 0) {
		std::string relative;
		const auto doc = mounted_path(name, relative);
		return doc && doc->allowed.count(relative);
	}
	if (name.rfind("uid://", 0) == 0) { auto found = state->startup.uids.find(name); return found != state->startup.uids.end() && state->startup.allowed.count(found->second); }
	if (name.rfind("res://", 0) == 0) name.erase(0, 6);
	return name != "project.godot" && path(name) && state->startup.allowed.count(name);
}
bool material_load_allowed(const String &resource, const String &original) {
	if (material_resource_allowed(resource)) return true;
	if (original.begins_with("res://.godot/meton-documents/")) {
		const auto cache = material_mounted_texture(original);
		return !cache.is_empty() && resource == cache;
	}
	const auto it = state->startup.texture_remaps.find(bytes(original.trim_prefix("res://")));
	return it != state->startup.texture_remaps.end() && resource == text("res://" + it->second);
}
}
