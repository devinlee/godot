#include "meton_game_content.h"
#include "meton_game_resources.generated.h"
#include "meton_game_support.generated.h"
#include "meton_game_identity.generated.h"
#include "meton_game_scenes.generated.h"
#include "meton_game_impact.generated.h"
#include "meton_game_counts.generated.h"
#include "meton_game_textures.generated.h"
#include "meton_game_memory.generated.h"
#include "meton_native_game_profile.h"
#include "core/crypto/crypto_core.h"
#include "core/io/file_access_memory.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/os/thread.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/resource_format_text.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/atlas_texture.h"
#include "scene/2d/sprite_2d.h"
#include <atomic>
#include <vector>

std::map<String, MetonGameContent *> MetonGameContent::stores;
namespace {
std::atomic<uint64_t> next_content{1};
/** JSON 的字段集合必须精确，避免静默接受未来格式或旁路配置。 */
bool fields(const Dictionary &p_value, std::initializer_list<const char *> p_required, std::initializer_list<const char *> p_optional = {}) {
    std::set<String> known;
    for (const char *key : p_required) { if (!p_value.has(key)) return false; known.insert(key); }
    for (const char *key : p_optional) known.insert(key);
    for (const Variant &key : p_value.keys()) if (key.get_type() != Variant::STRING || !known.count(key)) return false;
    return true;
}
/** G02 路径保持 ASCII 相对段；URI、父目录和反斜线不得进入私有缓存。 */
bool path(const String &p_value) {
    if (p_value.is_empty() || p_value.length() > 240 || p_value.begins_with(".") || p_value.ends_with(".")) return false;
    for (int i = 0; i < p_value.length(); ++i) {
        char32_t c = p_value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/')) return false;
    }
    for (const String &part : p_value.split("/", true)) if (part.is_empty() || part == "." || part == ".." || part.begins_with(".godot")) return false;
    return true;
}
/** 解码后再次核对固定原始字节，不把 base64 外观当作来源身份。 */
bool decoded(const Dictionary &p_file, PackedByteArray &r_bytes) {
    if (p_file["base64"].get_type() != Variant::STRING || p_file["digest"].get_type() != Variant::STRING || p_file["size_bytes"].get_type() != Variant::FLOAT) return false;
    const double size = p_file["size_bytes"];
    if (size < 1 || size > MetonResourceSupport::CONTENT_BYTES || size != int64_t(size)) return false;
    const String encoded = p_file["base64"];
    if (encoded.length() != ((int64_t(size) + 2) / 3) * 4) return false;
    CharString data = encoded.utf8();
    r_bytes.resize(int64_t(size));
    size_t actual = 0;
    if (CryptoCore::b64_decode(r_bytes.ptrw(), r_bytes.size(), &actual, reinterpret_cast<const uint8_t *>(data.get_data()), data.length()) != OK || actual != size_t(size)) return false;
    uint8_t sha[32];
    return CryptoCore::sha256(r_bytes.ptr(), r_bytes.size(), sha) == OK && String::hex_encode_buffer(sha, 32) == String(p_file["digest"]);
}
}

/** 私有路径只服务已固定的内存文件；最终仍使用 Godot 的资源解析器和缓存。 */
class MetonGameMaterialLoader : public ResourceFormatLoader {
    /** 加载发生在所属引擎线程，绝不把工作交给未知的全局外部加载器。 */
    static MetonGameContent *owner(const String &p_path) {
        if (!Thread::is_main_thread()) return nullptr;
        for (const auto &entry : MetonGameContent::stores) if (p_path.begins_with(entry.first)) return entry.second;
        return nullptr;
    }
public:
    bool recognize_path(const String &p_path, const String &p_type = String()) const override { return p_path.begins_with("res://__meton_games/"); }
    bool exists(const String &p_path) const override {
        auto *content = owner(p_path);
        return content && content->files.count(p_path.substr(content->prefix.length()));
    }
    bool handles_type(const String &p_type) const override { return p_type == "PackedScene" || p_type == "Texture2D" || p_type == "AtlasTexture"; }
    String get_resource_type(const String &p_path) const override {
        auto *content = owner(p_path);
        if (!content) return {};
        auto found = content->files.find(p_path.substr(content->prefix.length()));
        return found == content->files.end() ? String() : found->second.type;
    }
    Ref<Resource> load(const String &p_path, const String &p_original = "", Error *r_error = nullptr, bool p_threads = false,
            float *r_progress = nullptr, CacheMode p_cache = CACHE_MODE_REUSE) override {
        if (r_error) *r_error = ERR_UNAUTHORIZED;
        auto *content = owner(p_path);
        if (!content || p_threads || p_cache != CACHE_MODE_REUSE || (!p_original.is_empty() && p_original != p_path)) return {};
        auto found = content->files.find(p_path.substr(content->prefix.length()));
        if (found == content->files.end() || content->digest.is_empty()) return {};
        auto &file = found->second;
        ++content->parses;
        if (file.kind == "png") {
            Ref<Image> image; image.instantiate();
            Error result = image->load_png_from_buffer(file.source);
            if (r_error) *r_error = result;
            if (result != OK) return {};
            // 已固定尺寸只授权对应载荷；解码结果或格式变化不能扩大上传预算。
            const auto format = image->get_format();
            if (uint32_t(image->get_width()) != file.width || uint32_t(image->get_height()) != file.height || image->has_mipmaps() ||
                    (format != Image::FORMAT_L8 && format != Image::FORMAT_LA8 && format != Image::FORMAT_RGB8 && format != Image::FORMAT_RGBA8)) {
                if (r_error) *r_error = ERR_FILE_CORRUPT;
                return {};
            }
            image->convert(Image::FORMAT_RGBA8);
            if (uint64_t(image->get_data().size()) != file.texture_bytes) { if (r_error) *r_error = ERR_FILE_CORRUPT; return {}; }
            Ref<Resource> result_texture = ImageTexture::create_from_image(image);
            content->remember_texture(result_texture, file);
            content->remember(result_texture);
            return result_texture;
        }
        Ref<FileAccessMemory> memory; memory.instantiate();
        if (memory->open_custom(file.parser.ptr(), file.parser.size()) != OK) return {};
        ResourceLoaderText parser;
        parser.local_path = p_path; parser.res_path = p_path;
        parser.cache_mode = CACHE_MODE_REUSE; parser.cache_mode_for_external = CACHE_MODE_REUSE;
        parser.use_sub_threads = false; parser.progress = r_progress;
        parser.open(memory);
        const Error result = parser.load();
        if (r_error) *r_error = result;
        Ref<Resource> resource = result == OK ? parser.get_resource() : Ref<Resource>();
        if (resource.is_valid()) content->remember(resource);
        return resource;
    }
};
static Ref<MetonGameMaterialLoader> material_loader;
void MetonGameContent::initialize_loader() { material_loader.instantiate(); ResourceLoader::add_resource_format_loader(material_loader, true); }
void MetonGameContent::shutdown_loader() { ResourceLoader::remove_resource_format_loader(material_loader); material_loader.unref(); }
MetonGameContent::MetonGameContent() { prefix = "res://__meton_games/" + String::num_uint64(next_content.fetch_add(1)) + "/"; stores[prefix] = this; }
MetonGameContent::~MetonGameContent() { detach_font_atlas(); stores.erase(prefix); }
bool MetonGameContent::closure(const String &p_name, std::set<String> &p_stack, std::set<String> &p_seen, std::map<String, unsigned> &p_depths, unsigned p_depth) const {
    if (p_depth > MetonResourceSupport::CONTENT_DEPTH || p_stack.count(p_name)) return false;
    auto file = files.find(p_name);
    if (file == files.end()) return false;
    if (p_depths[p_name] >= p_depth) return true;
    p_depths[p_name] = p_depth;
    p_stack.insert(p_name); p_seen.insert(p_name);
    for (const String &dependency : file->second.dependencies) if (!closure(dependency, p_stack, p_seen, p_depths, p_depth + 1)) return false;
    p_stack.erase(p_name);
    return true;
}
bool MetonGameContent::install(const String &p_json, const String &p_digest, const String &p_version) {
    if (!digest.is_empty() || !files.empty() || p_json.utf8().length() > MetonResourceSupport::CONTENT_BYTES * 2 || p_digest.length() != 64 || p_json.sha256_text() != p_digest) return false;
    JSON json;
    if (json.parse(p_json) != OK || json.get_data().get_type() != Variant::DICTIONARY) return false;
    Dictionary snapshot = json.get_data();
    Dictionary consumer_limits;
    const bool project_content = snapshot.get("profile", Variant()) == Variant("gw3-native-content-v1");
    if (project_content) {
        // 消费身份与生产者分开。原内容 JSON 保持原字节，不能把真实宿主摘要改写成绑定摘要。
        if (!fields(snapshot, {"format_version", "profile", "material_digest", "content_digest", "consumer", "content_json"}) ||
                snapshot["format_version"] != Variant(1.0) || snapshot["consumer"].get_type() != Variant::DICTIONARY ||
                snapshot["content_json"].get_type() != Variant::STRING || snapshot["material_digest"].get_type() != Variant::STRING ||
                String(snapshot["material_digest"]).length() != 64 || snapshot["content_digest"].get_type() != Variant::STRING) return false;
        Dictionary consumer = snapshot["consumer"];
        if (!fields(consumer, {"binding_digest", "description_digest", "runtime_profile_digest", "target", "limits"}) ||
                consumer["binding_digest"] != Variant(METON_GAME_BINDING_DIGEST) || consumer["description_digest"] != Variant(MetonMemorySupport::INPUT_DIGEST) ||
                consumer["runtime_profile_digest"] != Variant(MetonGameRuntimeProfile::INPUT_DIGEST) ||
                consumer["target"] != Variant(MetonGameRuntimeProfile::TARGET) || consumer["limits"].get_type() != Variant::DICTIONARY) return false;
        consumer_limits = consumer["limits"];
        const String content_json = snapshot["content_json"];
        if (Variant(content_json.sha256_text()) != snapshot["content_digest"] || json.parse(content_json) != OK || json.get_data().get_type() != Variant::DICTIONARY) return false;
        snapshot = json.get_data();
    }
    if (!fields(snapshot, {"format_version", "profile", "source", "main_scene", "limits", "files", "content_digest", "scenes"}) || snapshot["format_version"] != Variant(2.0) ||
            snapshot["profile"] != Variant("jr2-scenes-v1") || snapshot["content_digest"].get_type() != Variant::STRING || String(snapshot["content_digest"]).length() != 64 ||
            snapshot["scenes"].get_type() != Variant::ARRAY || snapshot["source"].get_type() != Variant::DICTIONARY ||
            snapshot["limits"].get_type() != Variant::DICTIONARY || snapshot["files"].get_type() != Variant::ARRAY || snapshot["main_scene"].get_type() != Variant::STRING) return false;
    Dictionary source = snapshot["source"], limits = snapshot["limits"];
    if (!fields(source, {"project_id", "document_id", "source_version", "purpose", "build"}) || source["source_version"] != Variant(p_version) || source["build"].get_type() != Variant::DICTIONARY) return false;
    Dictionary build = source["build"];
    // 旧候选格式仍核对原身份；GW3 消费封套已核对实际绑定，不混用生产者和执行者字段。
    if (!fields(build, {"source_sha", "build_id", "engine_digest", "host_digest", "policy_digest", "protocol_version", "platform", "graphics"}) ||
            build["platform"] != Variant(MetonGameRuntimeProfile::TARGET) || build["protocol_version"] != Variant("0.1.0") ||
            !fields(limits, {"files", "bytes", "depth", "pixels"})) return false;
#ifdef METON_JS_LINUX_ENABLED
    if (build["graphics"] != Variant(MetonGameRuntimeProfile::DRIVER)) return false;
#endif
    if (!project_content && (build["source_sha"] != Variant(MetonGameRuntimeProfile::ENGINE_REVISION) || build["host_digest"] != Variant(METON_GAME_BINDING_DIGEST) ||
            build["policy_digest"] != Variant(MetonGameRuntimeProfile::CONTENT_POLICY))) return false;
    if (project_content && !fields(consumer_limits, {"files", "bytes", "depth", "pixels"})) return false;
    const Dictionary applied_limits = project_content ? consumer_limits : limits;
    const std::pair<const char *, int> maxima[] = {{"files", MetonResourceSupport::CONTENT_FILES}, {"bytes", MetonResourceSupport::CONTENT_BYTES},
        {"depth", MetonResourceSupport::CONTENT_DEPTH}, {"pixels", MetonResourceSupport::GPU_PIXELS}};
    for (const auto &maximum : maxima) {
        if (applied_limits[maximum.first].get_type() != Variant::FLOAT || limits[maximum.first].get_type() != Variant::FLOAT) return false;
        const double value = applied_limits[maximum.first], original = limits[maximum.first];
        if (!Math::is_finite(value) || value < 1 || value > maximum.second || value != int64_t(value) ||
                !Math::is_finite(original) || original < value || original > INT32_MAX || original != int64_t(original) ||
                (!project_content && value != maximum.second)) return false;
    }
    const Array entries = snapshot["files"];
    if (entries.is_empty() || entries.size() > int(applied_limits["files"])) return false;
    uint64_t bytes = 0, pixels = 0;
    bool valid = true;
    std::set<String> folded;
    for (const Variant &entry : entries) {
        if (entry.get_type() != Variant::DICTIONARY) { valid = false; break; }
        Dictionary item = entry;
        if (!fields(item, {"path", "kind", "digest", "size_bytes", "identity", "type", "dependencies", "base64", "layout"}, {"uid"}) ||
                item["path"].get_type() != Variant::STRING || item["kind"].get_type() != Variant::STRING || item["type"].get_type() != Variant::STRING ||
                item["dependencies"].get_type() != Variant::ARRAY || item["layout"].get_type() != Variant::ARRAY) { valid = false; break; }
        String name = item["path"];
        File file; file.kind = item["kind"]; file.type = item["type"]; file.layout = item["layout"];
        // PackedScene 包括 SceneState，PNG 解码时同时持有临时 Image 与纹理。
        file.resource_slots = file.kind == "resource" ? 1 : 2;
        if (!path(name) || !folded.insert(name.to_lower()).second || !decoded(item, file.source) ||
                !((file.kind == "scene" && file.type == "PackedScene" && name.ends_with(".tscn")) ||
                  (file.kind == "resource" && file.type == "AtlasTexture" && name.ends_with(".tres")) ||
                  (file.kind == "png" && file.type == "Texture2D" && name.ends_with(".png")))) { valid = false; break; }
        bytes += file.source.size();
        if (bytes > uint64_t(int(applied_limits["bytes"]))) { valid = false; break; }
        // 场景节点不能借 GW1 的纯数据 3D 集合在 JR2 支持门之外先实例化。
        if (file.kind == "scene") {
            if (file.layout.is_empty() || file.layout.size() > MetonGameSupport::MAX_OBJECTS) { valid = false; break; }
            std::set<String> node_paths;
            for (const Variant &entry_node : file.layout) {
                if (entry_node.get_type() != Variant::DICTIONARY) { valid = false; break; }
                Dictionary node = entry_node;
                if (!fields(node, {"path", "type"}) || node["path"].get_type() != Variant::STRING || node["type"].get_type() != Variant::STRING) { valid = false; break; }
                const String node_path = node["path"], node_type = node["type"];
                bool supported = false;
                for (const char *type : MetonResourceSupport::NODE_TYPES) supported |= node_type == type;
                if (!supported || (node_path != "." && !path(node_path)) || !node_paths.insert(node_path).second) { valid = false; break; }
            }
            if (!valid || !node_paths.count(".")) { valid = false; break; }
        } else if (!file.layout.is_empty()) { valid = false; break; }
        if (file.kind == "png") {
            if (file.source.size() < 45) { valid = false; break; }
            const uint8_t *data = file.source.ptr();
            const auto size = [&](int at) { return (uint64_t(data[at]) << 24) | (uint64_t(data[at + 1]) << 16) | (uint64_t(data[at + 2]) << 8) | data[at + 3]; };
            const uint64_t width = size(16), height = size(20);
            file.pixels = width * height; file.texture_bytes = file.pixels * 4;
            file.width = uint32_t(width); file.height = uint32_t(height);
            pixels += file.pixels;
            if (!width || !height || width > 4096 || height > 4096 || pixels > uint64_t(int(applied_limits["pixels"]))) { valid = false; break; }
        }
        if (item.has("uid")) {
            if (item["uid"].get_type() != Variant::STRING || !String(item["uid"]).begins_with("uid://") || !uids.emplace(item["uid"], name).second) { valid = false; break; }
            file.uid = item["uid"];
        }
        const Array dependencies = item["dependencies"];
        if (dependencies.size() > MetonResourceSupport::CONTENT_FILES) { valid = false; break; }
        for (const Variant &dependency : dependencies) {
            if (dependency.get_type() != Variant::STRING || !path(dependency)) { valid = false; break; }
            file.dependencies.push_back(dependency);
        }
        if (!valid) break;
        files[name] = file;
    }
    main_scene = snapshot["main_scene"];
    std::set<String> stack, seen;
    std::map<String, unsigned> depths;
    valid = valid && files.count(main_scene) && files.at(main_scene).kind == "scene" && closure(main_scene, stack, seen, depths, 1) && seen.size() == files.size();
    if (valid) for (const auto &depth : depths) if (depth.second > unsigned(int(applied_limits["depth"]))) { valid = false; break; }
    if (valid) for (auto &entry : files) if (entry.second.kind != "png") {
        auto &file = entry.second;
        String original;
        if (original.append_utf8(reinterpret_cast<const char *>(file.source.ptr()), file.source.size()) != OK) { valid = false; break; }
        String rewritten;
        // 只重写节头的引用属性；普通资源名称即使含 URI，也不能被误当作引用改写。
        for (String line : original.split("\n", true)) {
            String header = line.strip_edges();
            if (header.begins_with("[sub_resource ")) ++file.resource_slots;
            // G02 已限定为单行 bool 属性；按键和值识别，空白写法不改变预留。
            if (header.begins_with("resource_local_to_scene") && header.ends_with("true")) ++file.local_resources;
            const bool external = header.begins_with("[ext_resource ");
            if (external || header.begins_with("[gd_scene ") || header.begins_with("[gd_resource ")) {
                for (const auto &uid : uids) line = line.replace(" uid=\"" + uid.first + "\"", "");
                if (line.contains(" uid=")) { valid = false; break; }
            }
            if (external) {
                int begin = line.find(" path=\"");
                int end = begin < 0 ? -1 : line.find("\"", begin + 7);
                if (begin < 0 || end < 0) { valid = false; break; }
                String logical = line.substr(begin + 7, end - begin - 7);
                if (!logical.begins_with("res://") || !file.dependencies.has(logical.substr(6))) { valid = false; break; }
                line = line.substr(0, begin + 7) + prefix + logical.substr(6) + line.substr(end);
            }
            rewritten += line + "\n";
        }
        if (!valid) break;
        const CharString encoded = rewritten.utf8();
        file.parser.resize(encoded.length());
        if (encoded.length()) memcpy(file.parser.ptrw(), encoded.get_data(), encoded.length());
    }
    const Array scenes = snapshot["scenes"];
    std::set<String> scene_keys;
    if (scenes.size() > MetonResourceSupport::CONTENT_FILES) valid = false;
    if (valid) for (const Variant &item : scenes) {
        if (item.get_type() != Variant::DICTIONARY) { valid = false; break; }
        Dictionary scene = item;
        if (!fields(scene, {"id", "key", "path", "root_type", "nodes"}) || scene["id"].get_type() != Variant::STRING || String(scene["id"]).length() != 64 ||
                scene["key"].get_type() != Variant::STRING || !String(scene["key"]).is_valid_identifier() || !scene_keys.insert(scene["key"]).second ||
                scene["path"].get_type() != Variant::STRING || scene["root_type"].get_type() != Variant::STRING || scene["nodes"].get_type() != Variant::ARRAY) { valid = false; break; }
        auto file = files.find(scene["path"]);
        if (file == files.end() || file->second.kind != "scene") { valid = false; break; }
        std::map<String, String> layout;
        for (const Dictionary &node : file->second.layout) layout[node["path"]] = node["type"];
        if (layout["."] != String(scene["root_type"])) { valid = false; break; }
        const Array nodes = scene["nodes"];
        std::set<String> roles;
        if (nodes.size() > MetonGameSupport::MAX_OBJECTS) { valid = false; break; }
        for (const Variant &entry_node : nodes) {
            if (entry_node.get_type() != Variant::DICTIONARY) { valid = false; break; }
            Dictionary node = entry_node;
            if (!fields(node, {"id", "role", "path", "type", "optional"}) || node["id"].get_type() != Variant::STRING || String(node["id"]).length() != 64 ||
                    node["role"].get_type() != Variant::STRING || !String(node["role"]).is_valid_identifier() || !roles.insert(node["role"]).second ||
                    node["path"].get_type() != Variant::STRING || node["type"].get_type() != Variant::STRING || node["optional"].get_type() != Variant::BOOL) { valid = false; break; }
            String role = node["role"], name = node["path"], type = node["type"];
            bool supported = false; for (const char *candidate : MetonResourceSupport::NODE_TYPES) supported |= type == candidate;
            auto found = layout.find(name);
            if (role == "__proto__" || role == "prototype" || role == "constructor" || !supported || (name != "." && !path(name)) ||
                    (found == layout.end() ? !bool(node["optional"]) : found->second != type)) { valid = false; break; }
        }
        if (!valid || !descriptors.emplace(scene["id"], scene).second) { valid = false; break; }
    }
    if (!valid) { files.clear(); uids.clear(); descriptors.clear(); main_scene = String(); return false; }
    digest = p_digest; version = p_version;
    return true;
}
String MetonGameContent::resolve(const String &p_name) const {
    if (digest.is_empty()) return {};
    String name = p_name;
    if (name.begins_with("uid://")) {
        auto found = uids.find(name);
        if (found == uids.end()) return {};
        name = found->second;
    } else if (name.begins_with("res://")) name = name.substr(6);
    else return {};
    return files.count(name) ? prefix + name : String();
}
Ref<Resource> MetonGameContent::load(const String &p_name, Error *r_error) {
    const String target = resolve(p_name);
    if (r_error) *r_error = ERR_UNAUTHORIZED;
    if (target.is_empty()) return {};
    for (const auto &entry : files) if (ResourceLoader::path_remap(prefix + entry.first) != prefix + entry.first) return {};
    if (!reserve_load(target.substr(prefix.length()))) { if (r_error) *r_error = last_budget_error; return {}; }
    return ResourceLoader::load(target, "", ResourceLoader::CACHE_MODE_REUSE, r_error);
}
Dictionary MetonGameContent::state() {
    Dictionary state; state["digest"] = digest; state["source_version"] = version; state["files"] = int(files.size()); state["native_parses"] = int(parses);
    int live = 0;
    for (const ObjectID &id : resource_ids) live += ObjectDB::get_instance(id) != nullptr;
    state["live_native_resources"] = live;
    state["resource_identity_records"] = int(resource_ids.size());
    state["resource_limit"] = MetonResourceSupport::NATIVE_RESOURCES;
    state["budget_refusals"] = int(budget_refusals);
    state["last_reserved_slots"] = int(last_reserved_slots);
    state["textures"] = texture_state();
    return state;
}
/** 登记原生解析器实际产生的资源与已支持的内嵌/传递资源，路径不承担授权。 */
void MetonGameContent::remember(const Ref<Resource> &p_resource) {
    prune_resource_ids();
    if (p_resource.is_null() || !resource_ids.insert(p_resource->get_instance_id()).second) return;
    Ref<AtlasTexture> atlas = p_resource;
    if (atlas.is_valid()) remember(atlas->get_atlas());
    Ref<PackedScene> scene = p_resource;
    if (scene.is_valid()) {
        Ref<SceneState> state = scene->get_state();
        // SceneState 是 RefCounted 而非 Resource，但同属场景原生资源占用。
        resource_ids.insert(state->get_instance_id());
        for (int i = 0; i < state->get_node_count(); ++i) for (int j = 0; j < state->get_node_property_count(i); ++j) {
            Variant value = state->get_node_property_value(i, j);
            if (value.get_type() == Variant::OBJECT) { Ref<Resource> child = value; if (child.is_valid()) remember(child); }
        }
    }
}
/** 只在无脚本的固定 PackedScene 刚完成原生实例化、尚未交给游戏前登记本地副本。 */
void MetonGameContent::remember_scene(Node *p_root) {
    if (auto *sprite = Object::cast_to<Sprite2D>(p_root)) remember(sprite->get_texture());
    for (int i = 0; i < p_root->get_child_count(true); ++i) remember_scene(p_root->get_child(i, true));
}
bool MetonGameContent::owns(const Ref<Resource> &p_resource) const { return p_resource.is_valid() && resource_ids.count(p_resource->get_instance_id()); }
const Dictionary *MetonGameContent::descriptor(const String &p_id) const {
    auto found = descriptors.find(p_id);
    return digest.is_empty() || found == descriptors.end() ? nullptr : &found->second;
}
bool MetonGameContent::matches_scene(Node *p_root, const String &p_name) const {
    const String resolved = resolve(p_name);
    if (!p_root || resolved.is_empty()) return false;
    const auto &file = files.at(resolved.substr(prefix.length()));
    if (file.kind != "scene") return false;
    std::vector<Node *> pending{p_root};
    size_t count = 0;
    while (!pending.empty()) {
        Node *node = pending.back(); pending.pop_back();
        if (++count > size_t(file.layout.size())) return false;
        for (int i = 0; i < node->get_child_count(true); ++i) pending.push_back(node->get_child(i, true));
    }
    if (count != size_t(file.layout.size())) return false;
    for (const Dictionary &entry : file.layout) {
        Node *node = p_root->get_node_or_null(NodePath(entry["path"]));
        if (!node || node->get_class_name() != StringName(entry["type"])) return false;
    }
    return true;
}
