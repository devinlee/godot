#include "web_ui.h"
#include "web_game.h"
#include "web_support.generated.h"
#include "web_ui.generated.h"
#include "scene/gui/label.h"
#include "scene/gui/button.h"
#include "scene/gui/color_rect.h"
#include "scene/main/viewport.h"
#include "scene/theme/theme_db.h"
#include "core/object/callable_mp.h"
#include "core/config/engine.h"
#include "core/io/json.h"
#include "core/io/file_access.h"
#include "core/crypto/crypto_core.h"
#include "scene/theme/default_font.gen.h"
#include "meton_native_memory_abi.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_TRUETYPE_TABLES_H
#include <emscripten.h>
#include <cmath>
#include <set>

/** 原生最小尺寸只用于测量，不能抢占 U07 的明确布局结果。 */
class MetonUiLabel : public Label {
public:
    TextServer::AutowrapMode wrapping = TextServer::AUTOWRAP_WORD_SMART;
    TextServer::OverrunBehavior overrun = TextServer::OVERRUN_TRIM_CHAR;
    bool clipping = true;
    int lines = -1;
    Size2 get_minimum_size() const override { return Size2(); }
    /** 排版查询临时切换原生测量模式，随后恢复当前显示配置。 */
    Size2 measure(real_t p_width) {
        const Size2 previous = get_size();
        set_autowrap_mode(p_width < 0 ? TextServer::AUTOWRAP_OFF : wrapping);
        set_text_overrun_behavior(TextServer::OVERRUN_NO_TRIMMING);
        set_clip_text(false);
        set_max_lines_visible(-1);
        if (p_width >= 0) set_size(Size2(p_width, previous.y));
        Size2 value = Label::get_minimum_size();
        int count = get_line_count();
        if (lines > 0) count = MIN(count, lines);
        value.y = 0;
        for (int line = 0; line < count; ++line) value.y += get_line_height(line);
        if (count > 1) value.y += get_theme_constant(SNAME("line_spacing")) * (count - 1);
        set_autowrap_mode(wrapping); set_clip_text(clipping);
        set_text_overrun_behavior(overrun); set_max_lines_visible(lines); set_size(previous);
        return value;
    }
};

/** 保留 Godot 原输入/焦点行为，仅在真实输入栈内把原生信号归一成有界语义事件。 */
class MetonUiButton : public Button {
    bool in_input = false, pending = false;
    uint64_t pressed_revision = 0;
    void down() {
        if (!in_input || !lease || !lease->active || is_disabled() || !is_visible_in_tree()) return;
        pending = true; pressed_revision = lease->revision; lease->emit(token, "press");
    }
    void up() {
        if (in_input && pending && lease && lease->active && lease->revision == pressed_revision && !is_disabled() && is_visible_in_tree()) lease->emit(token, "release");
        pending = false;
    }
protected:
    void gui_input(const Ref<InputEvent> &p_event) override {
        if (!lease || !lease->active) return;
        in_input = true; Button::gui_input(p_event); in_input = false;
    }
    void pressed() override {
        if (in_input && pending && lease && lease->active && pressed_revision == lease->revision && !is_disabled() && is_visible_in_tree()) lease->emit(token, "activate");
    }
public:
    MetonGameUi::Lease *lease = nullptr;
    uint32_t token = 0;
    bool enabled = false;
    Size2 get_minimum_size() const override { return Size2(); }
    Size2 measure() {
        const bool clip = get_clip_text(); set_clip_text(false);
        Size2 value = Button::get_minimum_size(); set_clip_text(clip); return value;
    }
    /** 设置禁用会发 button_up；先清原尝试和输入标记，不能误报一次释放完成。 */
    void cancel() { pending = false; in_input = false; set_disabled(true); }
    MetonUiButton() {
        connect(SNAME("button_down"), callable_mp(this, &MetonUiButton::down));
        connect(SNAME("button_up"), callable_mp(this, &MetonUiButton::up));
        set_clip_text(true); set_clip_contents(true); set_focus_mode(FOCUS_ALL);
    }
};

namespace {
/** 长度先于读取/解码；指纹把资源绑到构建时固定的两份原字体。 */
bool matches_font(const uint8_t *p_data, size_t p_size, const char *p_sha) {
    unsigned char bytes[32];
    return p_data && CryptoCore::sha256(p_data, p_size, bytes) == OK && String::hex_encode_buffer(bytes, 32) == p_sha;
}

/** 解码器的临时内存也走本局原分配器；离开装配后只保留有上限的 SFNT 字节。 */
bool unpack_fallback(const PackedByteArray &p_woff, PackedByteArray &r_sfnt) {
    FT_MemoryRec_ memory{
        nullptr,
        [](FT_Memory, long size) -> void * { return meton_resource_alloc(size); },
        [](FT_Memory, void *block) { meton_resource_free(block); },
        [](FT_Memory, long, long size, void *block) -> void * { return meton_resource_realloc(block, size); },
    };
    FT_Library library = nullptr;
    if (FT_New_Library(&memory, &library) != 0) return false;
    FT_Add_Default_Modules(library);
    FT_Face face = nullptr;
    bool ready = false;
    if (FT_New_Memory_Face(library, p_woff.ptr(), p_woff.size(), 0, &face) == 0) {
        FT_ULong length = 0;
        // FreeType 的 tag=0 返回解压后的完整 SFNT，保留原字体所有表和字形。
        if (FT_Load_Sfnt_Table(face, 0, 0, nullptr, &length) == 0 && length > 0 &&
                length <= MetonUiSupport::FALLBACK_SFNT_LIMIT && r_sfnt.resize(length) == OK) {
            const FT_ULong expected = length;
            ready = FT_Load_Sfnt_Table(face, 0, 0, r_sfnt.ptrw(), &length) == 0 && length == expected;
        }
        FT_Done_Face(face);
    }
    FT_Done_Library(library);
    return ready;
}
}

/** 私有运行包持有 CJK 字节，关掉系统回退，避免不同机器掩盖缺字。 */
bool MetonGameUi::prepare_font() {
    if (_font_OpenSans_SemiBold_size != MetonUiSupport::PRIMARY_BYTES ||
            !matches_font(_font_OpenSans_SemiBold, _font_OpenSans_SemiBold_size, MetonUiSupport::PRIMARY_SHA256)) return false;
    Ref<FileAccess> file = FileAccess::open(MetonUiSupport::FALLBACK_PATH, FileAccess::READ);
    if (file.is_null() || file->get_length() != MetonUiSupport::FALLBACK_BYTES) return false;
    PackedByteArray bytes = file->get_buffer(int64_t(MetonUiSupport::FALLBACK_BYTES));
    if (bytes.size() != MetonUiSupport::FALLBACK_BYTES ||
            !matches_font(bytes.ptr(), bytes.size(), MetonUiSupport::FALLBACK_SHA256)) return false;
    return unpack_fallback(bytes, fallback_sfnt);
}

/** 每局只给私有控件创建两张字面；不改引擎主题或其他控件的字体缓存。 */
bool MetonGameUi::ensure_font() {
    if (private_font.is_valid()) return true;
    auto owner = binding->content.new_font_atlas_owner();
    if (!owner) return false;
    Ref<FontFile> fallback; fallback.instantiate(); fallback->set_data(fallback_sfnt);
    fallback->set_allow_system_fallback(false);
    Ref<FontFile> primary; primary.instantiate(); primary->set_data_ptr(_font_OpenSans_SemiBold, _font_OpenSans_SemiBold_size);
    primary->set_allow_system_fallback(false);
    if (!meton_create_font_atlas(*primary.ptr(), owner).is_valid() || !meton_create_font_atlas(*fallback.ptr(), owner).is_valid()) return false;
    TypedArray<Font> chain; chain.push_back(fallback); primary->set_fallbacks(chain);
    font_atlas = owner;
    private_font = primary;
    return true;
}

/** 本最小配置的换行、制表属于排版指令；可见字形缺失须在更新原控件前拒绝。 */
bool MetonGameUi::covers_text(const String &p_text) const {
    if (private_font.is_null()) return false;
    for (int i = 0; i < p_text.length(); ++i) {
        const char32_t ch = p_text[i];
        if (ch != '\n' && ch != '\r' && ch != '\t' && !private_font->has_char(ch)) return false;
    }
    return true;
}

void MetonGameUi::Lease::emit(uint32_t p_node, const String &p_event) {
    if (!active || overflow) return;
    if (events.size() >= MetonUiSupport::EVENTS || sequence >= 9007199254740991ULL) {
        overflow = true; active = false; events.clear(); return;
    }
    events.push_back(Event{p_node, p_event, ++sequence, revision});
}

/** 父节点必须还是交接的同一入树代次；离树/同名替换不能复活 UI 原租约。 */
Node *MetonGameUi::parent() {
    if (binding->revoked) return nullptr;
    auto found = binding->objects.find(parent_token);
    if (found == binding->objects.end()) return nullptr;
    const auto &entry = found->second;
    if (!entry.owned || entry.retired || entry.closing || !entry.active) return nullptr;
    if (epoch_bound && entry.epoch != parent_epoch) return nullptr;
    Node *value = Object::cast_to<Node>(ObjectDB::get_instance(entry.object));
    if (!value || !value->is_inside_tree() || value->is_queued_for_deletion()) return nullptr;
    if (!epoch_bound) { epoch_bound = true; parent_epoch = entry.epoch; }
    return value;
}
uint32_t MetonGameUi::open(const String &p_scope, uint64_t p_generation, uint64_t p_instance) {
    if (!parent() || !next_lease || leases.size() >= MetonUiSupport::LEASES) return 0;
    if (!scope.is_empty() && (scope != p_scope || generation != p_generation)) return 0;
    for (const auto &pair : leases) if (pair.second->instance == p_instance) return 0;
    scope = p_scope; generation = p_generation;
    auto value = std::make_unique<Lease>(); value->id = next_lease++;
    value->scope = p_scope; value->generation = p_generation; value->instance = p_instance;
    const uint32_t id = value->id; leases[id] = std::move(value); return id;
}
MetonGameUi::Lease *MetonGameUi::get(uint32_t p_lease) {
    if (!parent()) return nullptr;
    auto found = leases.find(p_lease); return found == leases.end() || found->second->closing ? nullptr : found->second.get();
}
Control *MetonGameUi::control(Lease *p_lease, uint32_t p_node, const char *p_kind) {
    if (!p_lease || !parent()) return nullptr;
    auto found = p_lease->items.find(p_node);
    if (found == p_lease->items.end() || (p_kind && found->second.kind != p_kind)) return nullptr;
    auto *node = Object::cast_to<Control>(ObjectDB::get_instance(found->second.object));
    if (!node || node->is_queued_for_deletion() || node->get_script_instance() || !node->is_inside_tree()) return nullptr;
    Node *expected = parent();
    if (found->second.parent) {
        expected = control(p_lease, found->second.parent, "container");
        if (!expected) return nullptr;
    }
    return node->get_parent() == expected ? node : nullptr;
}
uint32_t MetonGameUi::create(Lease *p_lease, const String &p_kind, uint32_t p_parent, const String &p_logical) {
    if (p_lease->active || p_lease->overflow || !next_node || p_lease->items.size() >= MetonUiSupport::NODES ||
            binding->live_objects() + live_objects() >= MetonGameSupport::MAX_OBJECTS) return 0;
    Node *attach = p_parent ? control(p_lease, p_parent, "container") : parent();
    if (!attach || (!p_parent && !p_lease->items.empty())) return 0;
    for (const auto &pair : p_lease->items) if (pair.second.logical == p_logical) return 0;
    const bool uses_font = p_kind == "text" || p_kind == "button";
    if (uses_font && !ensure_font()) return 0;
    Control *node = nullptr;
    if (p_kind == "container") node = memnew(ColorRect);
    else if (p_kind == "text") node = memnew(MetonUiLabel);
    else if (p_kind == "button") node = memnew(MetonUiButton);
    else return 0;
    const uint32_t id = next_node++;
    // 在挂树和任何通知前登记所属，使部分创建失败仍能按原组清理。
    p_lease->items[id] = Item{node->get_instance_id(), p_parent, p_kind, p_logical};
    node->set_name("Ui" + String::num_uint64(id));
    node->set_theme(ThemeDB::get_singleton()->get_default_theme());
    if (uses_font) node->add_theme_font_override(SNAME("font"), private_font);
    node->set_mouse_filter(p_kind == "button" ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
    node->set_visible(false);
    if (p_kind == "button") { auto *button = static_cast<MetonUiButton *>(node); button->lease = p_lease; button->token = id; button->cancel(); }
    attach->add_child(node, false, Node::INTERNAL_MODE_BACK);
    return id;
}
bool MetonGameUi::begin(Lease *p_lease, uint64_t p_revision) {
    if (p_lease->overflow || p_revision < p_lease->revision) return false;
    p_lease->active = false; p_lease->events.clear(); p_lease->revision = p_revision;
    for (const auto &pair : p_lease->items) {
        Control *node = control(p_lease, pair.first); if (!node) return false;
        if (pair.second.kind == "button") static_cast<MetonUiButton *>(node)->cancel();
    }
    return true;
}
bool MetonGameUi::commit(Lease *p_lease) {
    if (p_lease->overflow) return false;
    for (const auto &pair : p_lease->items) {
        Control *node = control(p_lease, pair.first); if (!node) return false;
        if (pair.second.kind == "button") { auto *button = static_cast<MetonUiButton *>(node); button->set_disabled(!button->enabled); }
    }
    p_lease->active = true; return true;
}
/** 释放只认原对象集合，实际树若混入外来后代则报告残留，不夺取他方所有权。 */
bool MetonGameUi::close(uint32_t p_lease) {
    auto found = leases.find(p_lease);
    if (found == leases.end()) return p_lease > 0 && p_lease < next_lease;
    Lease *value = found->second.get(); value->closing = true; value->active = false; value->events.clear();
    std::set<ObjectID> owned;
    for (const auto &pair : value->items) owned.insert(pair.second.object);
    std::vector<Node *> roots;
    bool safe = true;
    for (const auto &pair : value->items) {
        Node *node = Object::cast_to<Node>(ObjectDB::get_instance(pair.second.object));
        if (!node) continue;
        if (pair.second.kind == "button") { auto *button = static_cast<MetonUiButton *>(node); button->cancel(); button->release_focus(); button->lease = nullptr; }
        if (node->get_script_instance()) safe = false;
        for (int index = 0; index < node->get_child_count(true); ++index) if (!owned.count(node->get_child(index, true)->get_instance_id())) safe = false;
        if (!node->get_parent() || !owned.count(node->get_parent()->get_instance_id())) roots.push_back(node);
    }
    if (!safe) return false;
    for (ObjectID id : owned) released += ObjectDB::get_instance(id) != nullptr;
    for (Node *root : roots) memdelete(root);
    for (ObjectID id : owned) if (ObjectDB::get_instance(id)) return false;
    leases.erase(found);
    if (leases.empty()) {
        if (font_atlas) font_atlas->seal();
        private_font.unref();
        font_atlas.reset();
    }
    return true;
}
void MetonGameUi::close_all() {
    std::vector<uint32_t> ids;
    for (const auto &pair : leases) ids.push_back(pair.first);
    for (uint32_t id : ids) close(id);
}
MetonGameUi::~MetonGameUi() { close_all(); }
size_t MetonGameUi::live_objects() const {
    size_t count = 0;
    for (const auto &lease : leases) for (const auto &pair : lease.second->items) count += ObjectDB::get_instance(pair.second.object) != nullptr;
    return count;
}
Dictionary MetonGameUi::state() const {
    Dictionary value; value["leases"] = int(leases.size()); value["live_objects"] = int(live_objects()); value["released"] = int(released);
    value["private_font_active"] = private_font.is_valid();
    value["fallback_bytes"] = MetonUiSupport::FALLBACK_BYTES;
    value["font_decoded_bytes"] = int(fallback_sfnt.size());
    int pending = 0; bool overflow = false;
    for (const auto &pair : leases) { pending += int(pair.second->events.size()); overflow = overflow || pair.second->overflow; }
    value["pending_events"] = pending; value["overflow"] = overflow; return value;
}
Vector2 MetonGameUi::viewport() { Node *node = parent(); return node ? node->get_viewport()->get_visible_rect().size : Vector2(-1, -1); }

/** 同原生目标只交接已登记的所属父节点，交接发生在挂树之前且不可替换。 */
bool MetonWebGame::configure_ui(Node *p_node) {
    if (revoked || ui || !p_node || p_node->is_inside_tree()) return false;
    const uint32_t token = token_of(p_node); auto entry = objects.find(token);
    if (!token || entry == objects.end() || !entry->second.owned || entry->second.retired || entry->second.closing) return false;
    MetonNativeMemoryScope memory_scope(native_memory);
    auto candidate = std::make_unique<MetonGameUi>(this, token);
    if (!candidate->prepare_font()) return false;
    ui = std::move(candidate); return true;
}
size_t MetonWebGame::private_ui_objects() const { return ui ? ui->live_objects() : 0; }
Dictionary MetonWebGame::ui_state() const { return ui ? ui->state() : Dictionary(); }

namespace {
Dictionary ui_ok(const Variant &p_value = Variant()) { Dictionary result; result["ok"] = true; result["value"] = p_value; return result; }
bool ui_integer(const Variant &value, uint64_t &out, uint64_t maximum = 9007199254740991ULL) {
    if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) return false;
    const double number = value;
    if (!std::isfinite(number) || number < 0 || number > double(maximum) || std::floor(number) != number) return false;
    out = uint64_t(number); return true;
}
bool ui_number(const Variant &value, real_t &out) {
    if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) return false;
    const double number = value;
    if (!std::isfinite(number) || std::abs(number) > MetonUiSupport::COORDINATE) return false;
    out = real_t(number); return true;
}
/** Godot String 按码点计数；端口仍沿 JS 的 UTF-16 单元上限，不能放宽非 BMP 文本。 */
bool ui_string(const Variant &value, unsigned minimum = 0, unsigned maximum = MetonUiSupport::TEXT_UNITS) {
    if (value.get_type() != Variant::STRING) return false;
    const String text = value; unsigned length = 0;
    for (int index = 0; index < text.length(); ++index) {
        length += text[index] > 0xffff ? 2 : 1;
        if (length > maximum) return false;
    }
    return length >= minimum;
}
bool ui_color(const Variant &value) {
    if (value.get_type() != Variant::STRING) return false;
    const String color = value;
    if ((color.length() != 7 && color.length() != 9) || color[0] != '#') return false;
    for (int i = 1; i < color.length(); ++i) if (!((color[i] >= '0' && color[i] <= '9') || (color[i] >= 'a' && color[i] <= 'f') || (color[i] >= 'A' && color[i] <= 'F'))) return false;
    return true;
}
Dictionary ui_pair(const Vector2 &value) { Array pair; pair.push_back(value.x); pair.push_back(value.y); return ui_ok(pair); }
}

/** 固定私有标量 ABI；解析前限定字节，解析后限定形状，不接原生对象或方法名。 */
extern "C" EMSCRIPTEN_KEEPALIVE const char *meton_web_ui_request(int p_operation, const char *p_json) {
    static CharString output;
    Dictionary result;
    Ref<JSON> json; json.instantiate();
    if (!MetonWebGame::current) { result["ok"] = false; result["code"] = "scope_ended"; }
    else if (p_operation < 0 || p_operation > 12 || !p_json || strlen(p_json) > 32768 ||
            json->parse(String::utf8(p_json)) != OK || json->get_data().get_type() != Variant::ARRAY) {
        result = MetonWebGame::current->failure("invalid_argument");
    } else result = MetonWebGame::current->ui_call(p_operation, json->get_data());
    output = JSON::stringify(result).utf8(); return output.get_data();
}

EM_JS(int, meton_web_ui_attach_private, (), {
    try {
        globalThis.metonE3UiNativeAttach(Object.freeze({
            request: function(operation, ...args) {
                if (!Number.isInteger(operation) || operation < 0 || operation > 12 || args.length > 8) return {ok: false, code: 'invalid_argument'};
                // rest 数组由此函数创建；仅实际标量能进入 JSON，不调用项目的转换方法。
                for (let index = 0; index < args.length; ++index) {
                    const value = args[index], type = typeof value;
                    if (type === 'number' ? !Number.isFinite(value) : type === 'string' ? value.length > 4096 : type !== 'boolean') return {ok: false, code: 'invalid_argument'};
                }
                const input = stringToNewUTF8(JSON.stringify(args));
                try { return JSON.parse(UTF8ToString(_meton_web_ui_request(operation, input))); }
                finally { _free(input); }
            }
        }));
        return 1;
    } catch (error) { globalThis.metonC1Fault(error); return 0; }
});
bool MetonWebGame::attach_ui_port() { return meton_web_ui_attach_private() != 0; }

/** 原执行上下文、租约和真实父树共同复验；只有 close 可以在封锁后继续收尾。 */
Dictionary MetonWebGame::ui_call(int op, const Array &args) {
    MetonNativeMemoryScope memory_scope(native_memory);
    if (!ui) return failure("capability_unavailable");
    if (op < 0 || op > 12 || args.size() > 8) return failure("invalid_argument");
    for (int index = 0; index < args.size(); ++index) {
        const Variant::Type type = args[index].get_type();
        if (type != Variant::BOOL && type != Variant::INT && type != Variant::FLOAT && type != Variant::STRING) return failure("invalid_argument");
    }
    uint64_t lease_id = 0, node_id = 0;
    if (op == 12) {
        if (args.size() != 1 || !ui_integer(args[0], lease_id, UINT32_MAX)) return failure("invalid_argument");
        return ui_ok(ui->close(uint32_t(lease_id)));
    }
    if (revoked || !execution_depth || !active_stamp()) return failure("scope_ended");
    if (ui->font_fault() != OK) return failure(ui->font_fault() == ERR_OUT_OF_MEMORY ? "budget_exceeded" : "dependency_failed");
    if (!(phase & (MetonGameSupport::P_SAFE_POINT | MetonGameSupport::P_PROCESS | MetonGameSupport::P_PHYSICS | MetonGameSupport::P_READY))) return failure("phase_conflict");
    auto *engine = Engine::get_singleton();
    if (process_frame != engine->get_process_frames() || physics_frame != engine->get_physics_frames()) {
        process_frame = engine->get_process_frames(); physics_frame = engine->get_physics_frames(); calls = 0;
    }
    if (++calls > MetonGameSupport::MAX_CALLS_PER_ENTRY) return failure("budget_exceeded");
    if (op == 0) {
        uint64_t generation, instance;
        if (args.size() != 3 || !ui_string(args[0], 1, 128) || !ui_integer(args[1], generation) || !generation || !ui_integer(args[2], instance) || !instance) return failure("invalid_argument");
        const uint32_t id = ui->open(args[0], generation, instance);
        return id ? ui_ok(id) : failure("permission_denied");
    }
    if (op == 10) {
        if (!args.is_empty()) return failure("invalid_argument");
        const Vector2 size = ui->viewport(); return size.x < 0 ? failure("scope_ended") : ui_pair(size);
    }
    if (args.size() < 1 || !ui_integer(args[0], lease_id, UINT32_MAX)) return failure("invalid_argument");
    auto *lease = ui->get(uint32_t(lease_id));
    if (!lease) return failure("scope_ended");
    if (lease->overflow) return failure("budget_exceeded");
    if (op == 1) {
        if (args.size() != 4 || !ui_string(args[1], 1, 16) || !ui_integer(args[2], node_id, UINT32_MAX) || !ui_string(args[3], 1, 128)) return failure("invalid_argument");
        const String kind = args[1];
        if (kind != "container" && kind != "text" && kind != "button") return failure("invalid_argument");
        const uint32_t id = ui->create(lease, kind, uint32_t(node_id), args[3]);
        return id ? ui_ok(id) : failure("budget_exceeded");
    }
    if (op == 2) {
        uint64_t revision;
        if (args.size() != 2 || !ui_integer(args[1], revision)) return failure("invalid_argument");
        return ui->begin(lease, revision) ? ui_ok() : failure("scope_ended");
    }
    if (op == 9) {
        if (args.size() != 1) return failure("invalid_argument");
        return ui->commit(lease) ? ui_ok() : failure("scope_ended");
    }
    if (op == 11) {
        if (args.size() != 1) return failure("invalid_argument");
        Array events;
        for (const auto &event : lease->events) {
            if (!ui->control(lease, event.node, "button")) return failure("scope_ended");
            Array row; row.push_back(event.node); row.push_back(event.name); row.push_back(int64_t(event.sequence)); row.push_back(int64_t(event.revision)); events.push_back(row);
        }
        lease->events.clear(); return ui_ok(events);
    }
    if (lease->active) return failure("phase_conflict");
    if (args.size() < 2 || !ui_integer(args[1], node_id, UINT32_MAX)) return failure("invalid_argument");
    Control *node = ui->control(lease, uint32_t(node_id));
    if (!node) return failure("permission_denied");
    const String kind = lease->items.at(uint32_t(node_id)).kind;
    if (op == 3) {
        if (kind != "container" || args.size() != 3 || !ui_color(args[2])) return failure("invalid_argument");
        static_cast<ColorRect *>(node)->set_color(Color::html(args[2])); return ui_ok();
    }
    if (op == 4 || op == 5) {
        uint64_t size, lines = 0;
        if (kind != (op == 4 ? "text" : "button") || args.size() != (op == 4 ? 8 : 6) || !ui_string(args[2]) ||
                !ui_integer(args[3], size, MetonUiSupport::FONT_SIZE) || !size || !ui_color(args[4]) || !ui_string(args[5], 1, 16)) return failure("invalid_argument");
        if (!ui->covers_text(args[2])) return failure("capability_unavailable");
        const String mode = args[5];
        if (op == 4) {
            if (!ui_string(args[6], 1, 16) || !ui_integer(args[7], lines) || (mode != "auto" && mode != "none" && mode != "word" && mode != "character")) return failure("invalid_argument");
            const String overflow = args[6];
            if (overflow != "clip" && overflow != "ellipsis" && overflow != "visible") return failure("invalid_argument");
            auto *label = static_cast<MetonUiLabel *>(node);
            label->wrapping = mode == "none" ? TextServer::AUTOWRAP_OFF : mode == "character" ? TextServer::AUTOWRAP_ARBITRARY : mode == "word" ? TextServer::AUTOWRAP_WORD : TextServer::AUTOWRAP_WORD_SMART;
            // 控件容不下六个字形时，Godot 默认省略模式也会隐藏本来放得下的省略号。
            label->overrun = overflow == "visible" ? TextServer::OVERRUN_NO_TRIMMING : overflow == "ellipsis" ? TextServer::OVERRUN_TRIM_ELLIPSIS_FORCE : TextServer::OVERRUN_TRIM_CHAR;
            label->clipping = overflow != "visible"; label->lines = lines ? int(MIN(lines, uint64_t(MetonUiSupport::TEXT_UNITS) + 1)) : -1;
            label->set_text(args[2]); label->set_autowrap_mode(label->wrapping); label->set_text_overrun_behavior(label->overrun);
            label->set_clip_text(label->clipping); label->set_clip_contents(label->clipping); label->set_max_lines_visible(label->lines);
            label->add_theme_color_override(SNAME("font_color"), Color::html(args[4]));
        } else {
            if (mode != "press" && mode != "release") return failure("invalid_argument");
            auto *button = static_cast<MetonUiButton *>(node); button->set_text(args[2]);
            button->set_action_mode(mode == "press" ? BaseButton::ACTION_MODE_BUTTON_PRESS : BaseButton::ACTION_MODE_BUTTON_RELEASE);
            for (const char *name : {"font_color", "font_hover_color", "font_pressed_color", "font_hover_pressed_color", "font_disabled_color", "font_focus_color"})
                button->add_theme_color_override(StringName(name), Color::html(args[4]));
        }
        node->add_theme_font_size_override(SNAME("font_size"), int(size)); return ui_ok();
    }
    if (op == 6) {
        real_t width;
        if (args.size() != 3 || !ui_number(args[2], width) || (width < 0 && width != -1) || kind == "container") return failure("invalid_argument");
        return ui_pair(kind == "text" ? static_cast<MetonUiLabel *>(node)->measure(width) : static_cast<MetonUiButton *>(node)->measure());
    }
    if (op == 7) {
        real_t x, y, width, height;
        if (args.size() != 6 || !ui_number(args[2], x) || !ui_number(args[3], y) || !ui_number(args[4], width) || !ui_number(args[5], height) || width < 0 || height < 0) return failure("invalid_argument");
        node->set_position(Vector2(x, y)); node->set_size(Vector2(width, height)); return ui_ok();
    }
    if (op == 8) {
        real_t opacity;
        if (args.size() != 5 || args[2].get_type() != Variant::BOOL || args[3].get_type() != Variant::BOOL || !ui_number(args[4], opacity) || opacity < 0 || opacity > 1) return failure("invalid_argument");
        node->set_visible(args[2]); node->set_modulate(Color(1, 1, 1, opacity));
        if (kind == "button") {
            static_cast<MetonUiButton *>(node)->enabled = args[3];
            const bool focusable = bool(args[2]) && bool(args[3]);
            if (!focusable) node->release_focus();
            node->set_focus_mode(focusable ? Control::FOCUS_ALL : Control::FOCUS_NONE);
        }
        return ui_ok();
    }
    return failure("invalid_argument");
}

