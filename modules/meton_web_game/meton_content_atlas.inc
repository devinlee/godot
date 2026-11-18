/** 在各目标的纹理实现中复用额度计算，实际上传及设备释放由对应后端确认。 */
#include "meton_native_memory.h"
#include "modules/text_server_fb/meton_font_atlas.h"
#include "core/os/thread.h"
#include <algorithm>

struct MetonGameContent::AtlasHome {
    MetonGameContent *content;
};

struct MetonGameContent::AtlasCharge {
    uint64_t pixels = 0, bytes = 0, cpu_bytes = 0;
    ObjectID image;
    RID texture;
    std::atomic<bool> shelf_alive{true};
    bool image_alive() const { return image.is_valid() && ObjectDB::get_instance(image) != nullptr; }
};

class MetonGameContent::AtlasOwner final : public MetonFontAtlasOwner, public std::enable_shared_from_this<AtlasOwner> {
    std::shared_ptr<AtlasHome> home;
    MetonNativeMemory *memory;
    std::atomic<Error> error{OK};

    class Page final : public MetonFontAtlasPage {
        std::shared_ptr<AtlasOwner> owner;
        std::shared_ptr<AtlasCharge> charge;
    public:
        Page(std::shared_ptr<AtlasOwner> p_owner, std::shared_ptr<AtlasCharge> p_charge) : owner(p_owner), charge(p_charge) {}
        ~Page() override { charge->shelf_alive.store(false); }
        void image_created(const Ref<Image> &p_image) override {
            charge->image = p_image->get_instance_id();
            charge->cpu_bytes = uint64_t(p_image->get_data_size());
        }
        bool can_upload() override { return owner->active_content() != nullptr; }
        bool uploaded(const Ref<ImageTexture> &p_texture) override {
            auto *content = owner->active_content();
            if (!content || p_texture.is_null() || !p_texture->get_rid().is_valid()) {
                owner->set_fault(ERR_UNAVAILABLE);
                return false;
            }
            // 预留额度转入该实际 RID 的设备记录。后端确认释放前继续计费。
            charge->texture = p_texture->get_rid();
            File material;
            material.pixels = charge->pixels;
            material.texture_bytes = charge->bytes;
            content->remember_texture(p_texture, material);
            return true;
        }
    };

    void set_fault(Error p_error) {
        Error previous = OK;
        error.compare_exchange_strong(previous, p_error);
    }
    MetonGameContent *active_content() {
        if (fault() != OK) return nullptr;
        // 内容加载和宿主账目都在引擎原游戏线程访问。
        if (!Thread::is_main_thread()) { set_fault(ERR_UNAVAILABLE); return nullptr; }
        if (!home->content) { set_fault(ERR_UNCONFIGURED); return nullptr; }
        return home->content;
    }
public:
    AtlasOwner(std::shared_ptr<AtlasHome> p_home, MetonNativeMemory *p_memory) : home(p_home), memory(p_memory) { memory->retain(); }
    ~AtlasOwner() override { memory->release(); }
    Error fault() const override { return error.load(); }
    void seal() override { set_fault(ERR_UNCONFIGURED); }
    void *enter_heap() const override { return MetonNativeMemoryScope::exchange(memory); }
    void leave_heap(void *p_previous) const override { MetonNativeMemoryScope::exchange(static_cast<MetonNativeMemory *>(p_previous)); }
    void *allocate(size_t p_bytes) const override {
        MetonNativeMemoryScope scope(memory);
        return meton_resource_alloc(p_bytes);
    }
    void *resize(void *p_block, size_t p_bytes) const override {
        MetonNativeMemoryScope scope(memory);
        return meton_resource_realloc(p_block, p_bytes);
    }
    void deallocate(void *p_block) const override { meton_resource_free(p_block); }
    std::shared_ptr<MetonFontAtlasPage> reserve(int p_size, int p_channels, bool p_mipmaps) override {
        auto *content = active_content();
        if (!content) return {};
        if (p_size <= 0 || (p_channels != 2 && p_channels != 4)) { set_fault(ERR_UNAVAILABLE); return {}; }
        const uint64_t side = uint64_t(p_size);
        if (side > MetonWebRuntimeProfile::GPU_PIXELS / side) {
            ++content->texture_refusals;
            set_fault(ERR_OUT_OF_MEMORY);
            return {};
        }
        // 两端按含各级 mip 的 RGBA 上限预留；CPU 图像字节取自实际分配。
        const uint64_t bytes = uint64_t(Image::get_image_data_size(p_size, p_size, Image::FORMAT_RGBA8, p_mipmaps));
        const uint64_t pixels = bytes / 4;
        const Error admitted = content->reserve_texture_space(pixels, bytes, 0);
        if (admitted != OK) { set_fault(admitted); return {}; }
        auto charge = std::make_shared<AtlasCharge>();
        charge->pixels = pixels;
        charge->bytes = bytes;
        content->atlas_pages.push_back(charge);
        content->update_texture_peak();
        return std::make_shared<Page>(shared_from_this(), charge);
    }
};

std::shared_ptr<MetonFontAtlasOwner> MetonGameContent::new_font_atlas_owner() {
    auto *memory = MetonNativeMemoryScope::current();
    if (!memory || !Thread::is_main_thread()) return {};
    if (!atlas_home) atlas_home = std::make_shared<AtlasHome>(AtlasHome{this});
    return std::make_shared<AtlasOwner>(atlas_home, memory);
}

void MetonGameContent::detach_font_atlas() {
    if (atlas_home) atlas_home->content = nullptr;
}

void MetonGameContent::prune_atlas_pages() {
    atlas_pages.erase(std::remove_if(atlas_pages.begin(), atlas_pages.end(), [&](const auto &page) {
        return !page->shelf_alive.load() && !page->image_alive() && !texture_charges.count(page->texture.get_id());
    }), atlas_pages.end());
}

void MetonGameContent::add_pending_atlas_usage(uint64_t &r_pixels, uint64_t &r_bytes) const {
    for (const auto &page : atlas_pages) {
        if (!texture_charges.count(page->texture.get_id())) {
            r_pixels += page->pixels;
            r_bytes += page->bytes;
        }
    }
}

void MetonGameContent::texture_usage(uint64_t &r_pixels, uint64_t &r_bytes) const {
    r_pixels = 0;
    r_bytes = 0;
    for (const auto &entry : texture_charges) {
        r_pixels += entry.second.pixels;
        r_bytes += entry.second.bytes;
    }
    add_pending_atlas_usage(r_pixels, r_bytes);
}

void MetonGameContent::update_texture_peak() {
    uint64_t pixels, bytes;
    texture_usage(pixels, bytes);
    peak_texture_pixels = std::max(peak_texture_pixels, pixels);
    peak_texture_bytes = std::max(peak_texture_bytes, bytes);
}

Error MetonGameContent::reserve_texture_space(uint64_t p_pixels, uint64_t p_bytes, uint64_t p_decode) {
    if (!p_pixels) return OK;
    if (!texture_backend_available()) return ERR_UNAVAILABLE;
    reap_textures();
    uint64_t pixels, bytes;
    texture_usage(pixels, bytes);
    if (pixels > MetonWebRuntimeProfile::GPU_PIXELS || p_pixels > MetonWebRuntimeProfile::GPU_PIXELS - pixels ||
            bytes > MetonTextureSupport::TEXTURE_BYTES || p_bytes > MetonTextureSupport::TEXTURE_BYTES - bytes ||
            p_decode > MetonTextureSupport::NATIVE_BYTES) {
        ++texture_refusals;
        return ERR_OUT_OF_MEMORY;
    }
    decoded_payload_peak = std::max(decoded_payload_peak, p_decode);
    return OK;
}

Array MetonGameContent::atlas_state() const {
    Array pages;
    for (const auto &page : atlas_pages) {
        Dictionary item;
        item["rid"] = String::num_uint64(page->texture.get_id());
        item["image_id"] = String::num_uint64(uint64_t(page->image));
        item["pixels"] = int64_t(page->pixels);
        item["bytes"] = int64_t(page->bytes);
        item["cpu_image_bytes"] = int64_t(page->cpu_bytes);
        item["image_alive"] = page->image_alive();
        item["shelf_alive"] = page->shelf_alive.load();
        item["uploaded"] = page->texture.is_valid();
        pages.push_back(item);
    }
    return pages;
}
