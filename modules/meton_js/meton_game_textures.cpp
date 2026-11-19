#include "meton_game_content.h"
#include "meton_game_binding.h"
#include "meton_game_textures.generated.h"
#include "meton_native_game_profile.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "scene/resources/image_texture.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_device.h"
#include <mutex>

#include "meton_content_atlas.inc"

namespace {
std::mutex retirement_mutex;
std::map<uint64_t, std::weak_ptr<std::atomic<bool>>> registered_textures;
std::map<std::pair<uintptr_t, uint64_t>, std::shared_ptr<std::atomic<bool>>> retiring_textures;
}
/** RID 退表时转接到同一设备分配身份；不保存 VM/内容对象指针。 */
void meton_game_texture_retiring(uint64_t p_rid, const void *p_driver, uint64_t p_texture) {
    std::lock_guard<std::mutex> lock(retirement_mutex);
    auto found = registered_textures.find(p_rid);
    if (found == registered_textures.end()) return;
    if (auto state = found->second.lock()) retiring_textures[{reinterpret_cast<uintptr_t>(p_driver), p_texture}] = state;
    registered_textures.erase(found);
}
/** 仅由帧完成后的真实驱动释放交回确认，跨线程只写独立原子状态。 */
void meton_game_texture_disposed(const void *p_driver, uint64_t p_texture) {
    std::lock_guard<std::mutex> lock(retirement_mutex);
    auto found = retiring_textures.find({reinterpret_cast<uintptr_t>(p_driver), p_texture});
    if (found == retiring_textures.end()) return;
    found->second->store(true);
    retiring_textures.erase(found);
}
/** 受信清理在渲染线程等待帧和上传完成，再收集设备已失效资源；不推进游戏脚本。 */
void meton_game_texture_gpu_fence() {
    auto *device = RenderingDevice::get_singleton();
    if (!device || !RenderingServer::get_singleton()->is_on_render_thread() || device->frames.is_empty()) return;
    device->_flush_and_stall_for_all_frames(false);
    device->_submit_transfer_workers();
    device->_wait_for_transfer_workers();
    for (uint32_t frame = 0; frame < device->frames.size(); ++frame) device->_free_pending_resources(frame);
    device->_begin_frame();
}

/** 空渲染器不能提供实际 GPU 资源事实，也不能用空表冒充全部释放。 */
bool MetonGameContent::texture_backend_available() const {
    return DisplayServer::get_singleton() && String(DisplayServer::get_singleton()->get_name()) != "headless" &&
        RenderingServer::get_singleton()->get_current_rendering_driver_name() == MetonGameRuntimeProfile::DRIVER;
}
/** 只有原生引用结束且渲染队列已处理删除才能归还载荷额度。 */
void MetonGameContent::reap_textures() {
    prune_atlas_pages();
    if (texture_charges.empty() || !texture_backend_available()) return;
    auto *server = RenderingServer::get_singleton();
    List<RenderingServerTypes::TextureInfo> usage;
    server->texture_debug_usage(&usage);
    // 观察调用在独立渲染线程模式下会入队；必须在局部 List 失效前等待该查询完成。
    server->sync();
    std::set<uint64_t> live;
    backend_textures.clear();
    for (const auto &info : usage) {
        if (info.texture.is_valid()) live.insert(info.texture.get_id());
        // 后端观察与本方计费分别交回，宿主可核对字节、格式及残留；不复制其他游戏资源。
        if (!info.path.begins_with(prefix) && !texture_charges.count(info.texture.get_id())) continue;
        Dictionary item;
        item["rid"] = String::num_uint64(info.texture.get_id()); item["width"] = int64_t(info.width); item["height"] = int64_t(info.height);
        item["bytes"] = info.bytes; item["format"] = int(info.format);
        backend_textures.push_back(item);
    }
    for (auto entry = texture_charges.begin(); entry != texture_charges.end();) {
        auto &charge = entry->second;
        if (live.count(charge.rid.get_id())) { charge.observed = true; ++entry; continue; }
        // 先前从未被后端识别时，缺失可能是观察接口不可用，不能猜测已经释放。
        if (!ObjectDB::get_instance(charge.object) && charge.observed && charge.disposed->load()) { ++retired_textures; entry = texture_charges.erase(entry); }
        else ++entry;
    }
    prune_atlas_pages();
}
/** 整个未缓存闭包在解码前预留，上传过程中没有脚本回调能改变这份资格。 */
Error MetonGameContent::reserve_texture_load(const std::vector<String> &p_files) {
    uint64_t pixels = 0, bytes = 0, decode = 0;
    for (const String &name : p_files) {
        const auto &file = files.at(name);
        pixels += file.pixels; bytes += file.texture_bytes;
        if (file.pixels * 8 > decode) decode = file.pixels * 8;
    }
    return reserve_texture_space(pixels, bytes, decode);
}
/** 绑定的是已创建的原生纹理与实际 RID；Atlas/缓存别名不会再次调用这里。 */
void MetonGameContent::remember_texture(const Ref<Resource> &p_texture, const File &p_file) {
    Ref<ImageTexture> texture = p_texture;
    if (texture.is_null()) return;
    const RID rid = texture->get_rid();
    const RID device_rid = RenderingServer::get_singleton()->texture_get_rd_texture(rid, false);
    auto disposed = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(retirement_mutex);
        registered_textures[device_rid.get_id()] = disposed;
    }
    texture_charges.emplace(rid.get_id(), TextureCharge{texture->get_instance_id(), rid, p_file.pixels, p_file.texture_bytes, false, disposed});
    ++texture_uploads;
    update_texture_peak();
    // 创建仍有强引用，立即确认一次后端身份，快速创建/释放也不能丢失删除依据。
    reap_textures();
}
/** 受信宿主观察；不向游戏 JS 暴露 RID 或渲染器管理入口。 */
Dictionary MetonGameContent::texture_state() {
    reap_textures();
    Dictionary report;
    uint64_t bytes = 0, pixels = 0;
    int pending = 0, observed = 0;
    Array entries;
    for (const auto &entry : texture_charges) {
        const auto &charge = entry.second;
        const bool alive = ObjectDB::get_instance(charge.object) != nullptr;
        bytes += charge.bytes; pixels += charge.pixels; pending += !alive; observed += charge.observed;
        Dictionary item;
        item["rid"] = String::num_uint64(charge.rid.get_id()); item["object_id"] = String::num_uint64(uint64_t(charge.object));
        item["bytes"] = int64_t(charge.bytes); item["pixels"] = int64_t(charge.pixels); item["native_alive"] = alive; item["backend_observed"] = charge.observed;
        item["device_disposed"] = charge.disposed->load();
        entries.push_back(item);
    }
    auto *server = RenderingServer::get_singleton();
    report["backend_available"] = texture_backend_available(); report["driver"] = server->get_current_rendering_driver_name();
    report["device"] = server->get_video_adapter_name(); report["api"] = server->get_video_adapter_api_version();
    report["device_type"] = int(server->get_video_adapter_type()); report["backend_textures"] = backend_textures.duplicate(true);
    report["separate_render_thread"] = !server->is_on_render_thread();
    add_pending_atlas_usage(pixels, bytes);
    report["atlas_pages"] = atlas_state();
    report["bytes"] = int64_t(bytes); report["pixels"] = int64_t(pixels); report["pending_release"] = pending; report["backend_observed"] = observed;
    report["peak_bytes"] = int64_t(peak_texture_bytes); report["peak_pixels"] = int64_t(peak_texture_pixels); report["decoded_payload_peak"] = int64_t(decoded_payload_peak);
    report["uploads"] = int(texture_uploads); report["retired"] = int(retired_textures); report["refusals"] = int(texture_refusals); report["entries"] = entries;
    report["limit_bytes"] = int64_t(MetonTextureSupport::TEXTURE_BYTES); report["limit_pixels"] = int64_t(MetonGameRuntimeProfile::GPU_PIXELS);
    return report;
}
void MetonGameContent::drain_textures() {
    if (texture_charges.empty() || !texture_backend_available()) return;
    auto *server = RenderingServer::get_singleton();
    server->call_on_render_thread(callable_mp_static(&meton_game_texture_gpu_fence));
    server->sync();
    reap_textures();
}
Dictionary MetonGameBinding::drain_content_textures() {
    if (!content || closed || revoked) return Dictionary();
    content->drain_textures();
    return content->state();
}
