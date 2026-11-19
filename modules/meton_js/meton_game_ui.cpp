#include "meton_game_ui.h"
#include "meton_game_binding.h"
#include "meton_game_content.h"
#include "meton_game_support.generated.h"
#include "meton_game_ui.generated.h"
#include "scene/gui/label.h"
#include "scene/gui/button.h"
#include "scene/gui/color_rect.h"
#include "scene/main/viewport.h"
#include "scene/theme/theme_db.h"
#include "scene/theme/default_font.gen.h"
#ifdef METON_JS_LINUX_ENABLED
#include <meton_linux_game_profile.generated.h>
#ifdef METON_LINUX_OWNED_CJK_FONT
#include <meton_linux_cjk_font.generated.h>
#endif
#endif
#include "modules/text_server_fb/meton_font_cache.h"
#include "core/object/callable_mp.h"
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

/** 字体字节和光栅设置沿固定主题，同一游戏拥有自己的主字面及按系统规则选出的后备字面。 */
bool MetonGameUi::ensure_font() {
    if (private_font.is_valid()) return true;
    Ref<FontFile> theme_font = ThemeDB::get_singleton()->get_default_theme()->get_default_font();
    if (theme_font.is_null()) return false;
    Ref<FontFile> font;
    font.instantiate();
    font->set_data_ptr(_font_OpenSans_SemiBold, _font_OpenSans_SemiBold_size);
    font->set_antialiasing(theme_font->get_antialiasing());
    font->set_hinting(theme_font->get_hinting());
    font->set_subpixel_positioning(theme_font->get_subpixel_positioning());
    font->set_multichannel_signed_distance_field(theme_font->is_multichannel_signed_distance_field());
    font->set_generate_mipmaps(theme_font->get_generate_mipmaps());
    font->set_allow_system_fallback(theme_font->is_allow_system_fallback());
    auto owner = binding->content->new_font_atlas_owner();
    const RID face = meton_create_font_atlas(*font.ptr(), owner);
    if (!face.is_valid() || !meton_isolate_system_font_cache(face)) return false;
#ifdef METON_LINUX_OWNED_CJK_FONT
    // 离线中文字面与主字面共享本局图集所有者、计费和释放。
    Ref<FontFile> cjk;
    cjk.instantiate();
    cjk->set_data_ptr(MetonLinuxCjk::DATA, sizeof(MetonLinuxCjk::DATA));
    cjk->set_antialiasing(font->get_antialiasing());
    cjk->set_hinting(font->get_hinting());
    cjk->set_subpixel_positioning(font->get_subpixel_positioning());
    cjk->set_multichannel_signed_distance_field(font->is_multichannel_signed_distance_field());
    cjk->set_generate_mipmaps(font->get_generate_mipmaps());
    cjk->set_allow_system_fallback(font->is_allow_system_fallback());
    const RID cjk_face = meton_create_font_atlas(*cjk.ptr(), owner);
    if (!cjk_face.is_valid() || !meton_isolate_system_font_cache(cjk_face)) return false;
    TypedArray<Font> fallbacks;
    fallbacks.push_back(cjk);
    font->set_fallbacks(fallbacks);
#endif
    font_atlas = owner;
    private_font = font;
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
    if (binding->closed || binding->revoked) return nullptr;
    auto found = binding->objects.find(parent_token);
    if (found == binding->objects.end()) return nullptr;
    const auto &entry = found->second;
    if (!entry.owned || entry.retired || entry.closing || !entry.in_tree) return nullptr;
    if (epoch_bound && entry.epoch != parent_epoch) return nullptr;
    Node *value = Object::cast_to<Node>(ObjectDB::get_instance(entry.native));
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
    int pending = 0; bool overflow = false;
    for (const auto &pair : leases) { pending += int(pair.second->events.size()); overflow = overflow || pair.second->overflow; }
    value["pending_events"] = pending; value["overflow"] = overflow; return value;
}
Vector2 MetonGameUi::viewport() { Node *node = parent(); return node ? node->get_viewport()->get_visible_rect().size : Vector2(-1, -1); }

bool MetonGameBinding::configure_ui(Node *p_node) {
    if (closed || revoked || ui || !p_node || p_node->is_inside_tree()) return false;
    const uint32_t token = token_of(p_node); auto entry = objects.find(token);
    if (!token || entry == objects.end() || !entry->second.owned || entry->second.retired || entry->second.closing) return false;
    ui = std::make_unique<MetonGameUi>(this, token); return true;
}
size_t MetonGameBinding::private_ui_objects() const { return ui ? ui->live_objects() : 0; }
Dictionary MetonGameBinding::ui_state() const { return ui ? ui->state() : Dictionary(); }

namespace {
const char *UI_FUNCTIONS[] = {"open", "create", "begin", "container", "text", "button", "measure", "place", "appearance", "commit", "viewport", "poll", "close"};
/** 原语参数只接实际标量，不通过 ValueOf/getter/数组读取运行项目代码。 */
bool integer(v8::Local<v8::Value> value, uint64_t &out, uint64_t maximum = 9007199254740991ULL) {
    if (!value->IsNumber()) return false;
    const double number = value.As<v8::Number>()->Value();
    if (!std::isfinite(number) || number < 0 || number > double(maximum) || std::floor(number) != number) return false;
    out = uint64_t(number); return true;
}
bool ui_number(v8::Local<v8::Value> value, real_t &out) {
    if (!value->IsNumber()) return false;
    const double number = value.As<v8::Number>()->Value();
    if (!std::isfinite(number) || std::abs(number) > MetonUiSupport::COORDINATE) return false;
    out = real_t(number); return true;
}
bool color_string(const String &value) {
    if ((value.length() != 7 && value.length() != 9) || value[0] != '#') return false;
    for (int i = 1; i < value.length(); ++i) if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return false;
    return true;
}
}

/** 固定操作和精确参数表，不接受任意方法/属性名，也不从 UI 句柄返回原生对象。 */
void MetonGameBinding::ui_dispatch(const v8::FunctionCallbackInfo<v8::Value> &args) {
    auto *binding = self(args.GetIsolate()); const int op = args.DataV2().As<v8::Int32>()->Value();
    if (!binding || !binding->ui) { if (binding) binding->error("capability_unavailable", -1, "UI owner is not configured"); return; }
    auto *ui = binding->ui.get(); auto ctx = args.GetIsolate()->GetCurrentContext();
    auto invalid = [&]() { binding->error("invalid_argument", -1, "Invalid private UI primitive arguments"); };
    uint64_t lease_id = 0, node_id = 0;
    if (op == 12) {
        if (args.Length() != 1 || !integer(args[0], lease_id, UINT32_MAX)) { invalid(); return; }
        args.GetReturnValue().Set(ui->close(uint32_t(lease_id))); return;
    }
    if (binding->closed || binding->revoked || !binding->active_stamp()) { binding->error("scope_ended", -1, "UI game scope has ended"); return; }
    if (ui->font_fault() != OK) {
        binding->error(ui->font_fault() == ERR_OUT_OF_MEMORY ? "budget_exceeded" : "dependency_failed", -1, "Private font atlas was refused");
        return;
    }
    if (!(binding->phase & (MetonGameSupport::P_SAFE_POINT | MetonGameSupport::P_PROCESS | MetonGameSupport::P_PHYSICS | MetonGameSupport::P_READY))) {
        binding->error("phase_conflict", -1, "UI requires the original game safe point"); return;
    }
    if (!binding->charge_call(-1)) return;
    auto pair = [&](const Vector2 &value) {
        auto result = v8::Array::New(args.GetIsolate(), 2);
        if (result->Set(ctx, 0, v8::Number::New(args.GetIsolate(), value.x)).FromMaybe(false) &&
            result->Set(ctx, 1, v8::Number::New(args.GetIsolate(), value.y)).FromMaybe(false)) args.GetReturnValue().Set(result);
    };
    if (op == 0) {
        uint64_t generation, instance;
        if (args.Length() != 3 || !args[0]->IsString() || args[0].As<v8::String>()->Length() < 1 || args[0].As<v8::String>()->Length() > 128 ||
                !integer(args[1], generation) || !generation || !integer(args[2], instance) || !instance) { invalid(); return; }
        uint32_t id = ui->open(binding->string(args[0]), generation, instance);
        if (!id) { binding->error("permission_denied", -1, "UI lease ownership or capacity refused"); return; }
        args.GetReturnValue().Set(id); return;
    }
    if (op == 10) {
        if (args.Length()) { invalid(); return; }
        const Vector2 size = ui->viewport();
        if (size.x < 0) { binding->error("scope_ended", -1, "UI parent lifetime ended"); return; }
        pair(size); return;
    }
    if (args.Length() < 1 || !integer(args[0], lease_id, UINT32_MAX)) { invalid(); return; }
    auto *lease = ui->get(uint32_t(lease_id));
    if (!lease) { binding->error("scope_ended", -1, "UI lease or parent generation ended"); return; }
    if (lease->overflow) { binding->error("budget_exceeded", -1, "UI event queue exceeded its fixed limit"); return; }
    if (op == 1) {
        if (args.Length() != 4 || !args[1]->IsString() || !integer(args[2], node_id, UINT32_MAX) || !args[3]->IsString() ||
                args[3].As<v8::String>()->Length() < 1 || args[3].As<v8::String>()->Length() > 128) { invalid(); return; }
        const String kind = binding->string(args[1]);
        if (kind != "container" && kind != "text" && kind != "button") { invalid(); return; }
        uint32_t id = ui->create(lease, kind, uint32_t(node_id), binding->string(args[3]));
        if (!id) { binding->error("budget_exceeded", -1, "UI creation ownership or native object budget refused"); return; }
        args.GetReturnValue().Set(id); return;
    }
    if (op == 2) {
        uint64_t revision;
        if (args.Length() != 2 || !integer(args[1], revision)) { invalid(); return; }
        if (!ui->begin(lease, revision)) binding->error("scope_ended", -1, "UI update generation or tree changed"); return;
    }
    if (op == 9) {
        if (args.Length() != 1) { invalid(); return; }
        if (!ui->commit(lease)) binding->error("scope_ended", -1, "UI tree changed before commit"); return;
    }
    if (op == 11) {
        if (args.Length() != 1) { invalid(); return; }
        auto result = v8::Array::New(args.GetIsolate(), int(lease->events.size())); uint32_t index = 0;
        for (const auto &event : lease->events) {
            if (!ui->control(lease, event.node, "button")) { binding->error("scope_ended", -1, "UI event target ancestry changed"); return; }
            auto row = v8::Array::New(args.GetIsolate(), 4);
            v8::Local<v8::Value> cells[] = {v8::Uint32::New(args.GetIsolate(), event.node), binding->text(event.name),
                v8::Number::New(args.GetIsolate(), double(event.sequence)), v8::Number::New(args.GetIsolate(), double(event.revision))};
            for (uint32_t cell = 0; cell < 4; ++cell) if (!row->Set(ctx, cell, cells[cell]).FromMaybe(false)) return;
            if (!result->Set(ctx, index++, row).FromMaybe(false)) return;
        }
        lease->events.clear(); args.GetReturnValue().Set(result); return;
    }
    if (lease->active) { binding->error("phase_conflict", -1, "UI properties require an open update"); return; }
    if (args.Length() < 2 || !integer(args[1], node_id, UINT32_MAX)) { invalid(); return; }
    Control *node = ui->control(lease, uint32_t(node_id));
    if (!node) { binding->error("permission_denied", -1, "UI node does not belong to this lease"); return; }
    const String kind = lease->items.at(uint32_t(node_id)).kind;
    if (op == 3) {
        if (kind != "container" || args.Length() != 3 || !args[2]->IsString() || !color_string(binding->string(args[2]))) { invalid(); return; }
        static_cast<ColorRect *>(node)->set_color(Color::html(binding->string(args[2]))); return;
    }
    if (op == 4 || op == 5) {
        uint64_t size, lines = 0;
        if (kind != (op == 4 ? "text" : "button") || args.Length() != (op == 4 ? 8 : 6) || !args[2]->IsString() || args[2].As<v8::String>()->Length() > MetonUiSupport::TEXT_UNITS ||
                !integer(args[3], size, MetonUiSupport::FONT_SIZE) || !size || !args[4]->IsString() || !color_string(binding->string(args[4])) || !args[5]->IsString()) { invalid(); return; }
        const String mode = binding->string(args[5]);
        if (op == 4) {
            if (!args[6]->IsString() || !integer(args[7], lines) || (mode != "auto" && mode != "none" && mode != "word" && mode != "character")) { invalid(); return; }
            const String overflow = binding->string(args[6]);
            if (overflow != "clip" && overflow != "ellipsis" && overflow != "visible") { invalid(); return; }
            auto *label = static_cast<MetonUiLabel *>(node);
            label->wrapping = mode == "none" ? TextServer::AUTOWRAP_OFF : mode == "character" ? TextServer::AUTOWRAP_ARBITRARY : mode == "word" ? TextServer::AUTOWRAP_WORD : TextServer::AUTOWRAP_WORD_SMART;
            // 控件容不下六个字形时，Godot 默认省略模式也会隐藏本来放得下的省略号。
            label->overrun = overflow == "visible" ? TextServer::OVERRUN_NO_TRIMMING : overflow == "ellipsis" ? TextServer::OVERRUN_TRIM_ELLIPSIS_FORCE : TextServer::OVERRUN_TRIM_CHAR;
            // N 个换行可以产生 N+1 行；数值收窄不能截掉有效输入的最后一行。
            label->clipping = overflow != "visible"; label->lines = lines ? int(MIN(lines, uint64_t(MetonUiSupport::TEXT_UNITS) + 1)) : -1;
            label->set_text(binding->string(args[2])); label->set_autowrap_mode(label->wrapping); label->set_text_overrun_behavior(label->overrun);
            label->set_clip_text(label->clipping); label->set_clip_contents(label->clipping); label->set_max_lines_visible(label->lines);
            label->add_theme_color_override(SNAME("font_color"), Color::html(binding->string(args[4])));
        } else {
            if (mode != "press" && mode != "release") { invalid(); return; }
            auto *button = static_cast<MetonUiButton *>(node); button->set_text(binding->string(args[2]));
            button->set_action_mode(mode == "press" ? BaseButton::ACTION_MODE_BUTTON_PRESS : BaseButton::ACTION_MODE_BUTTON_RELEASE);
            for (const char *name : {"font_color", "font_hover_color", "font_pressed_color", "font_hover_pressed_color", "font_disabled_color", "font_focus_color"})
                button->add_theme_color_override(StringName(name), Color::html(binding->string(args[4])));
        }
        node->add_theme_font_size_override(SNAME("font_size"), int(size)); return;
    }
    if (op == 6) {
        real_t width;
        if (args.Length() != 3 || !ui_number(args[2], width) || (width < 0 && width != -1) || kind == "container") { invalid(); return; }
        pair(kind == "text" ? static_cast<MetonUiLabel *>(node)->measure(width) : static_cast<MetonUiButton *>(node)->measure()); return;
    }
    if (op == 7) {
        real_t x, y, width, height;
        if (args.Length() != 6 || !ui_number(args[2], x) || !ui_number(args[3], y) || !ui_number(args[4], width) || !ui_number(args[5], height) || width < 0 || height < 0) { invalid(); return; }
        node->set_position(Vector2(x, y)); node->set_size(Vector2(width, height)); return;
    }
    if (op == 8) {
        real_t opacity;
        if (args.Length() != 5 || !args[2]->IsBoolean() || !args[3]->IsBoolean() || !ui_number(args[4], opacity) || opacity < 0 || opacity > 1) { invalid(); return; }
        node->set_visible(args[2].As<v8::Boolean>()->Value()); node->set_modulate(Color(1, 1, 1, opacity));
        if (kind == "button") {
            static_cast<MetonUiButton *>(node)->enabled = args[3].As<v8::Boolean>()->Value();
            const bool focusable = args[2].As<v8::Boolean>()->Value() && args[3].As<v8::Boolean>()->Value();
            // BaseButton.disabled 只拒绝按钮动作，Control 仍可能取得焦点；两种资格须一起封锁。
            if (!focusable) node->release_focus();
            node->set_focus_mode(focusable ? Control::FOCUS_ALL : Control::FOCUS_NONE);
        }
        return;
    }
    invalid();
}

v8::MaybeLocal<v8::Promise> MetonGameBinding::evaluate_ui(v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
    auto *binding = self(v8::Isolate::GetCurrent()); auto *isolate = binding->isolate;
    for (int index = 0; index < 13; ++index) {
        v8::Local<v8::Function> function;
        if (!v8::Function::New(context, ui_dispatch, v8::Integer::New(isolate, index), 0, v8::ConstructorBehavior::kThrow).ToLocal(&function) ||
            !module->SetSyntheticModuleExport(isolate, binding->text(UI_FUNCTIONS[index]), function).FromMaybe(false)) return {};
    }
    auto limits = v8::Object::New(isolate);
    const char *names[] = {"coordinate", "font_size", "events"};
    const unsigned values[] = {MetonUiSupport::COORDINATE, MetonUiSupport::FONT_SIZE, MetonUiSupport::EVENTS};
    for (int i = 0; i < 3; ++i) if (!limits->CreateDataProperty(context, binding->text(names[i]), v8::Uint32::New(isolate, values[i])).FromMaybe(false)) return {};
    if (!limits->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen).FromMaybe(false) ||
        !module->SetSyntheticModuleExport(isolate, binding->text("limits"), limits).FromMaybe(false) ||
        !module->SetSyntheticModuleExport(isolate, binding->text("profile"), binding->text("meton.engine.ui/1")).FromMaybe(false)) return {};
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) || !resolver->Resolve(context, v8::Undefined(isolate)).FromMaybe(false)) return {};
    return resolver->GetPromise();
}
v8::Local<v8::Module> MetonGameBinding::ui_module() {
    std::vector<v8::Local<v8::String>> names;
    for (const char *name : UI_FUNCTIONS) names.push_back(text(name));
    names.push_back(text("profile")); names.push_back(text("limits"));
    return v8::Module::CreateSyntheticModule(isolate, text("meton.internal.ui"), names, evaluate_ui);
}
