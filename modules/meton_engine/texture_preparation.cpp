#include "texture_preparation.h"
#include "core/string/ustring.h"
#ifdef TOOLS_ENABLED
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/import/resource_importer_texture.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace meton::bridge {
std::string texture_cache_path(const std::string &source) {
	return ".godot/imported/" + std::string(String::utf8(source.c_str()).sha256_text().utf8().get_data()) + ".ctex";
}
std::string texture_sidecar(const std::string &source, const TextureSource &fact, const std::string &cache) {
	std::string result = "[remap]\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\n";
	if (!fact.uid.empty()) result += "uid=\"" + fact.uid + "\"\n";
	return result + "path=\"res://" + cache + "\"\n\n[deps]\nsource_file=\"res://" + source +
			"\"\ndest_files=[\"res://" + cache + "\"]\n";
}
#ifdef TOOLS_ENABLED
bool prepare_texture_files(const std::wstring &root, const std::string &producer_json, const TextureSources &sources) {
	const String output = String(root.c_str()).path_join("cache/texture-imports").replace("\\", "/");
	// 只写全新子目录。失败时没有完成回执，E01 按原代次回收所有部分结果。
	if (DirAccess::make_dir_absolute(output) != OK) return false;
	Ref<ResourceImporterTexture> importer;
	importer.instantiate();
	Array files;
	uint64_t total = 0;
	for (const auto &entry : sources) {
		const String input = "res://" + String::utf8(entry.first.c_str());
		const String target = output.path_join(String::utf8(texture_cache_path(entry.first).c_str()).get_file());
		List<ResourceImporter::ImportOption> defaults;
		importer->get_import_options(input, &defaults, ResourceImporterTexture::PRESET_2D);
		HashMap<StringName, Variant> options;
		for (const auto &option : defaults) options[option.option.name] = option.default_value;
		List<String> variants, generated;
		const auto uid = entry.second.uid.empty() ? ResourceUID::INVALID_ID : ResourceUID::get_singleton()->text_to_id(String::utf8(entry.second.uid.c_str()));
		if (importer->import(uid, input, target.get_basename(), options, &variants, &generated, nullptr) != OK || !variants.is_empty() || !generated.is_empty()) return false;
		const uint64_t size = FileAccess::get_size(target);
		const String digest = FileAccess::get_sha256(target);
		if (!size || size > 67108864 - total || digest.length() != 64) return false;
		total += size;
		Dictionary file;
		file["path"] = String::utf8(entry.first.c_str());
		file["source_digest"] = String::utf8(entry.second.digest.c_str());
		file["digest"] = digest;
		file["size_bytes"] = int64_t(size);
		files.push_back(file);
	}
	Dictionary receipt;
	receipt["format_version"] = 1;
	receipt["recipe"] = "godot-texture2d-lossless-v1";
	receipt["producer"] = JSON::parse_string(String::utf8(producer_json.c_str()));
	receipt["files"] = files;
	const String content = JSON::stringify(receipt);
	Ref<FileAccess> result = FileAccess::open(output.path_join("meton-imports.json"), FileAccess::WRITE);
	if (result.is_null() || !result->store_string(content) || result->get_error() != OK) return false;
	result->flush();
	result.unref();
	// 摘要取自刚生成的规范字节，经本代创建时继承的事件端交付，磁盘回执无法给自身签发来源。
	const auto events = OS::get_singleton()->get_environment("METON_ENGINE_EVENT_HANDLE").to_int();
	const HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(events));
	const auto digest = content.sha256_text().utf8();
	DWORD written = 0;
	return events > 0 && GetFileType(pipe) == FILE_TYPE_PIPE && digest.length() == 64 &&
			WriteFile(pipe, digest.get_data(), 64, &written, nullptr) && written == 64;
}
#endif
}
