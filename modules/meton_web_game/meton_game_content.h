#pragma once
#include "core/io/resource.h"
#include "core/variant/dictionary.h"
#include "core/templates/rid.h"
#include <map>
#include <set>
#include <vector>
#include <atomic>
#include <memory>

class MetonGameMaterialLoader;
class MetonFontAtlasOwner;
class Node;
class Image;

/** G02 已检查的固定内存内容；私有缓存路径属于这一游戏代次，不接受任意磁盘路径。 */
class MetonGameContent {
    friend class MetonGameMaterialLoader;
    struct File {
        String kind;
        String type;
        String uid;
        PackedByteArray source;
        PackedByteArray parser;
        PackedStringArray dependencies;
        Array layout;
        unsigned resource_slots = 1;
        unsigned local_resources = 0;
        uint64_t pixels = 0;
        uint64_t texture_bytes = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::map<String, File> files;
    std::map<String, String> uids;
    std::map<String, Dictionary> descriptors;
    String prefix;
    String digest;
    String version;
    String main_scene;
    unsigned parses = 0;
    std::set<ObjectID> resource_ids;
    unsigned budget_refusals = 0;
    unsigned last_reserved_slots = 0;
    struct TextureCharge { ObjectID object; RID rid; uint64_t pixels; uint64_t bytes; bool observed = false; std::shared_ptr<std::atomic<int>> disposed; };
    std::map<uint64_t, TextureCharge> texture_charges;
    struct AtlasHome;
    struct AtlasCharge;
    class AtlasOwner;
    std::shared_ptr<AtlasHome> atlas_home;
    std::vector<std::shared_ptr<AtlasCharge>> atlas_pages;
    void detach_font_atlas();
    void prune_atlas_pages();
    void add_pending_atlas_usage(uint64_t &r_pixels, uint64_t &r_bytes) const;
    void texture_usage(uint64_t &r_pixels, uint64_t &r_bytes) const;
    void update_texture_peak();
    Error reserve_texture_space(uint64_t p_pixels, uint64_t p_bytes, uint64_t p_decode);
    Array atlas_state() const;
    Array backend_textures;
    Error last_budget_error = ERR_OUT_OF_MEMORY;
    uint64_t peak_texture_bytes = 0;
    uint64_t peak_texture_pixels = 0;
    uint64_t decoded_payload_peak = 0;
    unsigned texture_uploads = 0;
    unsigned retired_textures = 0;
    unsigned texture_refusals = 0;
    bool texture_backend_available() const;
    void reap_textures();
    Error reserve_texture_load(const std::vector<String> &p_files);
    void remember_texture(const Ref<Resource> &p_texture, const File &p_file);
    Dictionary texture_state();
    /** 弱身份不能无限积累；只在原生主线程上核对 ObjectDB。 */
    void prune_resource_ids();
    bool reserve_load(const String &p_name);
    static std::map<String, MetonGameContent *> stores;
    /** 在安装前检查整份闭包，解析器尚未运行时拒绝未知依赖、环与深度超限。 */
    bool closure(const String &p_name, std::set<String> &p_stack, std::set<String> &p_seen, std::map<String, unsigned> &p_depths, unsigned p_depth) const;
public:
    /** 加载器只在模块初始化时登记一次，多个游戏共享机制而不共享可写缓存键。 */
    static void initialize_loader();
    static void shutdown_loader();
    MetonGameContent();
    ~MetonGameContent();
    std::shared_ptr<MetonFontAtlasOwner> new_font_atlas_owner();
    /** 摘要与版本来自受信装配；本代已经接入后不能替换。 */
    bool install(const String &p_json, const String &p_digest, const String &p_version);
    /** 逻辑路径/UID 只能解析到固定文件，失败不回退到 Godot 的全局索引。 */
    String resolve(const String &p_name) const;
    /** 原生 ResourceLoader 的 REUSE 语义保持，加载前拒绝全局重映射。 */
    Ref<Resource> load(const String &p_name, Error *r_error = nullptr);
    /** 只用于原生宿主复核，不向游戏交出缓存路径或原始字节。 */
    Dictionary state();
    /** 固定展开布局用于原生实例化之前的节点额度检查。 */
    size_t scene_nodes(const String &p_name) const;
    /** 场景本地资源按固定闭包的保守副本上界预留，未通过不实例化。 */
    bool reserve_scene(const String &p_name);
    /** 只观察真实后端故障；未知释放不能变成可用额度。 */
    bool gpu_healthy() const;
    /** 只由真实加载/实例化登记出处，资源路径本身不能伪造所属游戏。 */
    void remember(const Ref<Resource> &p_resource);
    void remember_scene(Node *p_root);
    bool owns(const Ref<Resource> &p_resource) const;
    /** 描述只能来自同一次固定快照；对外不接受任意节点映射。 */
    const Dictionary *descriptor(const String &p_id) const;
    /** 装配时对照真实继承展开结果，静态布局不能替代原生实例事实。 */
    bool matches_scene(Node *p_root, const String &p_name) const;
    bool has_resource_path(const String &p_path) const { return !digest.is_empty() && p_path.begins_with(prefix); }
};
