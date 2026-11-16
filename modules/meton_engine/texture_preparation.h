#pragma once
#include <map>
#include <string>

namespace meton::bridge {
struct TextureSource {
	std::string digest;
	std::string uid;
};
using TextureSources = std::map<std::string, TextureSource>;
/** 文件名由受控源路径派生，同名但目录不同的 PNG 有独立目标。 */
std::string texture_cache_path(const std::string &source);
/** 为同一源/UID/CTEX 生成唯一可接受的 Godot sidecar 字节。 */
std::string texture_sidecar(const std::string &source, const TextureSource &fact, const std::string &cache);
#ifdef TOOLS_ENABLED
/** 使用已初始化的锁定版内置导入器，将结果写到本代新建的专用缓存。 */
bool prepare_texture_files(const std::wstring &root, const std::string &producer_json, const TextureSources &sources);
#endif
}
