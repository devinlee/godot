#include "web_runtime_profile.generated.h"
#include "meton_game_content.h"
#include "meton_game_textures.generated.h"
#include "core/io/image.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"
#include <GLES3/gl3.h>
#include <emscripten/html5.h>

#include "meton_content_atlas.inc"

namespace {
struct Pending { GLsync fence; std::shared_ptr<std::atomic<int>> state; };
std::map<uint64_t, std::weak_ptr<std::atomic<int>>> registered;
std::map<uint64_t, Pending> pending;
}
/** 后端调用真实 glDeleteTextures 后登记；不保存游戏、VM 或内容对象指针。 */
void meton_web_texture_deleted(uint64_t p_rid) {
    auto found = registered.find(p_rid);
    if (found == registered.end()) return;
    auto state = found->second.lock(); registered.erase(found);
    if (!state) return;
    const auto context = emscripten_webgl_get_current_context();
    if (!context || emscripten_is_webgl_context_lost(context)) { state->store(3); return; }
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!fence) { state->store(3); return; }
    state->store(1); pending.emplace(p_rid, Pending{fence, state});
    glFlush();
}
/** 浏览器必须先让出事件循环；每帧只作零超时查询，不阻塞或忙等。 */
void meton_web_texture_poll() {
    const auto context = emscripten_webgl_get_current_context();
    const bool lost = !context || emscripten_is_webgl_context_lost(context);
    for (auto item = pending.begin(); item != pending.end();) {
        const GLenum result = lost ? GL_WAIT_FAILED : glClientWaitSync(item->second.fence, 0, 0);
        if (result == GL_TIMEOUT_EXPIRED) { ++item; continue; }
        const bool completed = result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
        item->second.state->store(completed ? 2 : 3);
        if (!lost) glDeleteSync(item->second.fence);
        item = pending.erase(item);
    }
}
/** 固定单线程 WebGL2 目标；空渲染器或失效上下文没有释放证明。 */
bool MetonGameContent::texture_backend_available() const {
    const auto context = emscripten_webgl_get_current_context();
    EmscriptenWebGLContextAttributes attributes;
    return context && !emscripten_is_webgl_context_lost(context) &&
        emscripten_webgl_get_context_attributes(context, &attributes) == EMSCRIPTEN_RESULT_SUCCESS && attributes.majorVersion == 2 &&
        RenderingServer::get_singleton()->is_on_render_thread();
}
bool MetonGameContent::gpu_healthy() const {
    if (!texture_backend_available()) return false;
    for (const auto &entry : texture_charges) if (entry.second.disposed->load() == 3) return false;
    return true;
}
/** 只有原生引用结束且渲染队列已处理删除才能归还载荷额度。 */
void MetonGameContent::reap_textures() {
    prune_atlas_pages();
    meton_web_texture_poll();
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
        if (!ObjectDB::get_instance(charge.object) && charge.observed && charge.disposed->load() == 2) { ++retired_textures; entry = texture_charges.erase(entry); }
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
    auto disposed = std::make_shared<std::atomic<int>>(0);
    registered[rid.get_id()] = disposed;
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
        item["device_disposed"] = charge.disposed->load() == 2; item["retirement_state"] = charge.disposed->load();
        entries.push_back(item);
    }
    auto *server = RenderingServer::get_singleton();
    report["healthy"] = gpu_healthy(); report["retirement"] = "webgl2_delete_then_fence_poll_zero_timeout";
    report["backend_available"] = texture_backend_available(); report["driver"] = server->get_current_rendering_driver_name();
    report["device"] = server->get_video_adapter_name(); report["api"] = server->get_video_adapter_api_version();
    report["device_type"] = int(server->get_video_adapter_type()); report["backend_textures"] = backend_textures.duplicate(true);
    report["separate_render_thread"] = !server->is_on_render_thread();
    add_pending_atlas_usage(pixels, bytes);
    report["atlas_pages"] = atlas_state();
    report["bytes"] = int64_t(bytes); report["pixels"] = int64_t(pixels); report["pending_release"] = pending; report["backend_observed"] = observed;
    report["peak_bytes"] = int64_t(peak_texture_bytes); report["peak_pixels"] = int64_t(peak_texture_pixels); report["decoded_payload_peak"] = int64_t(decoded_payload_peak);
    report["uploads"] = int(texture_uploads); report["retired"] = int(retired_textures); report["refusals"] = int(texture_refusals); report["entries"] = entries;
    report["limit_bytes"] = int64_t(MetonTextureSupport::TEXTURE_BYTES); report["limit_pixels"] = int64_t(MetonWebRuntimeProfile::GPU_PIXELS);
    return report;
}
