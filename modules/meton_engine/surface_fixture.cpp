#ifdef METON_SURFACE_FIXTURE_ENABLED
// 仅显式测试配置编译；固定原生场景，不加载项目脚本，不成为正式启动或权限例外。
#include "surface_runtime.h"
#include "meton_fixture_config.generated.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"
#include <cstdio>

namespace meton::bridge {
namespace {
RID viewport, canvas, item;
bool attempted = false, initialized = false;
std::uint64_t ticks = 0, version = 1, polls = 0, last_report = 0;
std::wstring log_file;
/** 测试上下文只认固定的十进制值，缺失/溢出不猜测默认实例或适配器。 */
std::uint64_t decimal(const String &text) {
	if (text.is_empty() || text[0] == '0') return 0;
	std::uint64_t value = 0;
	for (int i = 0; i < text.length(); ++i) {
		if (text[i] < '0' || text[i] > '9' || value > (UINT64_MAX - (text[i] - '0')) / 10) return 0;
		value = value * 10 + text[i] - '0';
	}
	return value;
}
std::uint64_t argument(const String &key) {
	for (const auto &value : OS::get_singleton()->get_cmdline_user_args()) if (value.begins_with(key + "=")) return decimal(value.substr(key.length() + 1));
	return 0;
}
void report(const char *state) {
	if (log_file.empty()) return;
	const auto status = surface_status();
	char json[512]{};
	std::snprintf(json, sizeof(json), "{\"format_version\":1,\"state\":\"%s\",\"ticks\":\"%llu\",\"published\":\"%llu\",\"generation\":\"%llu\",\"document_version\":\"%llu\",\"failure\":%lu,\"stopped\":%s,\"retained_bytes\":\"%llu\",\"visible\":%s,\"control_sequence\":\"%llu\",\"polls\":\"%llu\"}\n",
		state, ticks, status.published, status.surface_generation, status.document_version, static_cast<unsigned long>(status.failure), status.stopped ? "true" : "false", status.retained_bytes,
		status.visible ? "true" : "false", status.control_sequence, polls);
	HANDLE file = CreateFileW(log_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (file != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file, json, static_cast<DWORD>(std::char_traits<char>::length(json)), &written, nullptr); CloseHandle(file); }
}
void scene(unsigned width, unsigned height, bool second) {
	auto *rs = RenderingServer::get_singleton();
	rs->viewport_set_size(viewport, width, height); rs->canvas_item_clear(item);
	// 蓝色底与中央矩形来自 Godot 的 Canvas 绘制，可区分真实视口和独立 D3D 清屏夹具。
	rs->canvas_item_add_rect(item, Rect2(0, 0, width, height), Color(0, 0, 1));
	rs->canvas_item_add_rect(item, Rect2(width / 8, height / 8, width * 3 / 4, height * 3 / 4), second ? Color(0, 1, 0) : Color(1, 0, 0));
}
}
void initialize_surface_fixture() {
	const auto path = OS::get_singleton()->get_environment("METON_ENGINE_ROOT") + "/logs/godot-surface.json";
	const auto utf16 = path.utf16(); log_file.assign(reinterpret_cast<const wchar_t *>(utf16.get_data()), utf16.length());
}
void fixture_frame_begin() {
	++ticks;
	if (!attempted) {
		attempted = true;
		const auto instance = argument("meton-instance"), adapter = argument("meton-adapter");
		const auto commands = decimal(OS::get_singleton()->get_environment("METON_ENGINE_COMMAND_HANDLE"));
		const auto events = decimal(OS::get_singleton()->get_environment("METON_ENGINE_EVENT_HANDLE"));
		if (!instance || !adapter || !commands || !events) { report("context_rejected"); return; }
		auto transport = SurfaceTransport::open(reinterpret_cast<HANDLE>(commands), reinterpret_cast<HANDLE>(events), instance,
			{fixture_config::max_queue_messages, fixture_config::operation_timeout_ms});
		if (!transport) { report("transport_rejected"); return; }
		auto *rs = RenderingServer::get_singleton();
		viewport = rs->viewport_create(); canvas = rs->canvas_create(); item = rs->canvas_item_create();
		rs->canvas_item_set_parent(item, canvas); rs->viewport_attach_canvas(viewport, canvas);
		rs->viewport_set_update_mode(viewport, RenderingServerEnums::VIEWPORT_UPDATE_ALWAYS); rs->viewport_set_active(viewport, true);
		scene(64, 32, false);
		CaptureContext context{instance, 7, adapter, {fixture_config::max_frame_width, fixture_config::max_frame_height,
			fixture_config::max_frame_slots, fixture_config::max_gpu_bytes}, fixture_config::operation_timeout_ms};
		context.max_fps = 30; context.shutdown_timeout_ms = fixture_config::shutdown_timeout_ms;
		initialized = install_surface(context, transport, rs->viewport_get_texture(viewport), version);
		if (!initialized) { transport->stop(); report("capture_rejected"); return; }
		report("running");
	}
	if (!initialized) return;
	if (ticks == 120) { version = 2; scene(96, 48, true); update_surface_source(RenderingServer::get_singleton()->viewport_get_texture(viewport), version); }
	if (ticks == 300 && argument("meton-auto-stop") == 1) stop_surface();
}
void fixture_frame_submitted() { if (initialized && (ticks % 30 == 0 || FAILED(surface_status().failure))) report("running"); }
/** 只写测试观察；隐藏时的控制/EOF 处理由正式 runtime_tick 执行。 */
void fixture_runtime_poll() {
	if (!initialized) return;
	++polls; const auto now = GetTickCount64();
	if (now - last_report >= 100 || surface_status().stopped) { last_report = now; report(surface_status().stopped ? "stopped" : "running"); }
}
void fixture_render_shutdown() {
	auto *rs = RenderingServer::get_singleton();
	if (item.is_valid()) rs->free_rid(item);
	if (viewport.is_valid()) rs->free_rid(viewport);
	if (canvas.is_valid()) rs->free_rid(canvas);
	item = RID(); viewport = RID(); canvas = RID();
}
}
#endif
