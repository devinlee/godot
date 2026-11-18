#pragma once
#include "modules/text_server_fb/meton_font_atlas.h"
#include "core/math/vector2.h"
#include "core/object/object_id.h"
#include "core/variant/dictionary.h"
#include "scene/resources/font.h"
#include "core/string/ustring.h"
#include <map>
#include <memory>
#include <vector>

class Node;
class Control;
class MetonGameBinding;
class MetonUiButton;
class MetonUiLabel;

/** 原组拥有控件和事件，外部只能保有本实例单调签发的数值句柄。 */
class MetonGameUi {
public:
    struct Event { uint32_t node; String name; uint64_t sequence; uint64_t revision; };
    struct Item { ObjectID object; uint32_t parent = 0; String kind; String logical; };
    struct Lease {
        uint32_t id = 0;
        String scope;
        uint64_t generation = 0, instance = 0, revision = 0, sequence = 0;
        bool active = false, overflow = false, closing = false;
        std::map<uint32_t, Item> items;
        std::vector<Event> events;
        /** 原生信号只写本组有界队列，不能重入 JS 或业务提交。 */
        void emit(uint32_t p_node, const String &p_event);
    };
private:
    MetonGameBinding *binding;
    uint32_t parent_token;
    uint64_t parent_epoch = 0;
    bool epoch_bound = false;
    uint32_t next_lease = 1, next_node = 1;
    String scope;
    uint64_t generation = 0;
    std::map<uint32_t, std::unique_ptr<Lease>> leases;
    unsigned released = 0;
    Ref<FontFile> private_font;
    std::shared_ptr<MetonFontAtlasOwner> font_atlas;
    bool ensure_font();
    Node *parent();
public:
    MetonGameUi(MetonGameBinding *p_binding, uint32_t p_parent) : binding(p_binding), parent_token(p_parent) {}
    ~MetonGameUi();
    Error font_fault() const { return font_atlas ? font_atlas->fault() : OK; }
    uint32_t open(const String &p_scope, uint64_t p_generation, uint64_t p_instance);
    Lease *get(uint32_t p_lease);
    Control *control(Lease *p_lease, uint32_t p_node, const char *p_kind = nullptr);
    uint32_t create(Lease *p_lease, const String &p_kind, uint32_t p_parent, const String &p_logical);
    bool begin(Lease *p_lease, uint64_t p_revision);
    bool commit(Lease *p_lease);
    bool close(uint32_t p_lease);
    void close_all();
    size_t live_objects() const;
    Dictionary state() const;
    Vector2 viewport();
};
