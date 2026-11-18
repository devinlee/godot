#include "meton_game_content.h"
#include "meton_game_resources.generated.h"
#include "core/io/resource_loader.h"
#include <vector>
void MetonGameContent::prune_resource_ids() {
    for (auto id = resource_ids.begin(); id != resource_ids.end();) {
        if (!ObjectDB::get_instance(*id)) id = resource_ids.erase(id); else ++id;
    }
}
/** 同步加载没有跨线程预留竞争；固定缓存的临时 Ref 防止核对过程中失去最后引用。 */
bool MetonGameContent::reserve_load(const String &p_name) {
    prune_resource_ids();
    std::vector<String> pending{p_name};
    std::set<String> seen;
    std::vector<Ref<Resource>> pins;
    std::vector<String> uncached;
    last_budget_error = ERR_OUT_OF_MEMORY;
    uint64_t slots = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        const String name = pending[i];
        if (!seen.insert(name).second) continue;
        Ref<Resource> cached = ResourceCache::get_ref(prefix + name);
        if (cached.is_valid()) { pins.push_back(cached); continue; }
        const auto &file = files.at(name);
        slots += file.resource_slots;
        uncached.push_back(name);
        for (const String &dependency : file.dependencies) pending.push_back(dependency);
    }
    last_reserved_slots = unsigned(slots);
    if (resource_ids.size() + slots > MetonResourceSupport::NATIVE_RESOURCES) { ++budget_refusals; return false; }
    last_budget_error = reserve_texture_load(uncached);
    return last_budget_error == OK;
}
size_t MetonGameContent::scene_nodes(const String &p_name) const {
    const String target = resolve(p_name);
    if (target.is_empty()) return 0;
    const auto &file = files.at(target.substr(prefix.length()));
    return file.kind == "scene" ? file.layout.size() : 0;
}
/** 每个展开节点最多创建一份各本地资源；共享和本地引用均不能越过闭包。 */
bool MetonGameContent::reserve_scene(const String &p_name) {
    const String target = resolve(p_name);
    if (target.is_empty()) return false;
    prune_resource_ids();
    std::vector<String> pending{target.substr(prefix.length())};
    std::set<String> seen;
    uint64_t local = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        const String name = pending[i];
        if (!seen.insert(name).second) continue;
        const auto &file = files.at(name);
        local += file.local_resources;
        for (const String &dependency : file.dependencies) pending.push_back(dependency);
    }
    const uint64_t slots = local * scene_nodes(p_name);
    last_reserved_slots = unsigned(slots);
    if (resource_ids.size() + slots > MetonResourceSupport::NATIVE_RESOURCES) { ++budget_refusals; return false; }
    return true;
}
