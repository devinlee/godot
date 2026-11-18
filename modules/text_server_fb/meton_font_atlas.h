#pragma once

#include "core/error/error_list.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include <memory>

class Image;
class ImageTexture;
class FontFile;

/** 每个字形图集页持有自己的额度凭据，尚未绘制的页也占用额度。 */
class MetonFontAtlasPage {
public:
	virtual ~MetonFontAtlasPage() = default;
	virtual void image_created(const Ref<Image> &p_image) = 0;
	virtual bool can_upload() = 0;
	virtual bool uploaded(const Ref<ImageTexture> &p_texture) = 0;
};

class MetonFontAtlasOwner {
public:
	virtual ~MetonFontAtlasOwner() = default;
	virtual std::shared_ptr<MetonFontAtlasPage> reserve(int p_size, int p_channels, bool p_mipmaps) = 0;
	virtual Error fault() const = 0;
	virtual void seal() = 0;
	virtual void *enter_heap() const = 0;
	virtual void leave_heap(void *p_previous) const = 0;
	virtual void *allocate(size_t p_bytes) const = 0;
	virtual void *resize(void *p_block, size_t p_bytes) const = 0;
	virtual void deallocate(void *p_block) const = 0;
};

class MetonFontMemoryScope {
	const MetonFontAtlasOwner *owner;
	void *previous;

public:
	explicit MetonFontMemoryScope(const MetonFontAtlasOwner *p_owner) :
			owner(p_owner), previous(owner ? owner->enter_heap() : nullptr) {}
	~MetonFontMemoryScope() {
		if (owner) {
			owner->leave_heap(previous);
		}
	}
	MetonFontMemoryScope(const MetonFontMemoryScope &) = delete;
	MetonFontMemoryScope &operator=(const MetonFontMemoryScope &) = delete;
};

/** 原生宿主在字形缓存初始化前登记所属游戏，主字体和选中的后备字体共用该归属。 */
RID meton_create_font_atlas(FontFile &p_font, const std::shared_ptr<MetonFontAtlasOwner> &p_owner);
