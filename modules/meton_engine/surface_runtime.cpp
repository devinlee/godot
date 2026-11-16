#include "surface_runtime.h"
#include "material_admission.h"
#include "scene_surface.h"
#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "main/main.h"
#include "scene/main/scene_tree.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"
#include <atomic>

namespace meton::bridge {
namespace {
// current 仅由渲染线程访问；main 只读固定大小的事实，不持有 GPU 或管道对象。
SurfaceCapture *current = nullptr;
std::atomic<bool> active{false}, tick_queued{false}, visible{false}, stopping{false}, stopped{false};
std::atomic<long> failure{S_OK};
std::atomic<std::uint32_t> target_fps{60}, shutdown_ms{10000};
std::atomic<std::uint64_t> requested_size{0};
std::uint64_t applied_size = 0; // 只有 main 更新已落实的大小。
Object *additional_output = nullptr; // 只有 main 创建/销毁。
std::uint64_t next_draw = 0, shutdown_started = 0;
bool quit_requested = false;

/** 事实发布不包含原始句柄，main 的限帧/退出不进入渲染对象的锁。 */
void publish_status() {
	if (!current) return;
	const auto status = current->status();
	// 先发布结论，再发布完成；main 看见 stopped 后不能还读到上一轮的成功码。
	requested_size.store((std::uint64_t(status.width) << 32) | status.height);
	failure.store(status.failure); visible.store(status.visible); stopping.store(status.stopping); stopped.store(status.stopped);
}
/** 隐藏时仍执行短轮询；最多一个在途，避免渲染繁忙时不断堆积回调。 */
void runtime_tick() {
	if (current) {
		current->poll();
#ifdef METON_SURFACE_FIXTURE_ENABLED
		fixture_runtime_poll();
#endif
		publish_status();
	}
	tick_queued.store(false);
}
}
bool install_surface(const CaptureContext &context, std::shared_ptr<SurfaceTransport> transport, RID texture, std::uint64_t version) {
	if (current || active.load()) return false;
	auto capture = SurfaceCapture::create(context, std::move(transport));
	if (!capture || !capture->source(texture, version)) return false;
	current = capture.release(); target_fps.store(context.max_fps); shutdown_ms.store(context.shutdown_timeout_ms);
	active.store(true); return true;
}
bool update_surface_source(RID texture, std::uint64_t version) { return current && current->source(texture, version); }
CaptureStatus surface_status() { return current ? current->status() : CaptureStatus{false, true}; }
void stop_surface() { if (current) { current->stop(); publish_status(); } }
bool surface_main_iteration(bool wants_present) {
	const auto *scene = material_surface();
	if (!active.load() && !scene) return wants_present;
	auto *os = OS::get_singleton();
	if (!additional_output) {
		additional_output = memnew(Object);
		DisplayServer::get_singleton()->register_additional_output(additional_output);
		DisplayServer::get_singleton()->window_set_vsync_mode(DisplayServerEnums::VSYNC_DISABLED);
		const auto handle = DisplayServer::get_singleton()->window_get_native_handle(DisplayServerEnums::WINDOW_HANDLE);
		if (handle) ShowWindow(reinterpret_cast<HWND>(handle), SW_HIDE);
		Engine::get_singleton()->set_max_fps(scene ? scene->capture.max_fps : target_fps.load());
	}
	if (scene && !active.load()) {
		bind_scene_viewport();
		// 编辑器需执行异步文件扫描才能给出正确的视口，等待期间没有可发布的帧。
		return true;
	}
	// 隐藏窗口本就走低耗轮询；上限防止编辑器的 unfocused 设置把 EOF 检查推迟很久。
	if (os->get_low_processor_usage_mode_sleep_usec() > 10000 || os->get_low_processor_usage_mode_sleep_usec() <= 0)
		os->set_low_processor_usage_mode_sleep_usec(10000);
	if (!tick_queued.exchange(true)) RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&runtime_tick));
	const auto now = os->get_ticks_usec();
	if (stopping.load()) {
		if (!shutdown_started) shutdown_started = now;
		const bool expired = now - shutdown_started >= std::uint64_t(shutdown_ms.load()) * 1000;
		if (!quit_requested && (stopped.load() || expired) && SceneTree::get_singleton()) {
			quit_requested = true;
			// 超时只请求失败退出；模块析构保留未知 GPU 引用，E01 仍观察最终进程和材料清理。
			SceneTree::get_singleton()->quit(expired || FAILED(failure.load()) ? EXIT_FAILURE : EXIT_SUCCESS);
		}
		return false;
	}
	if (scene) pump_scene_input();
	if (!visible.load() || !RenderingServer::get_singleton()->is_render_loop_enabled()) { next_draw = 0; return false; }
	const auto size = requested_size.load();
	if (scene && size && size != applied_size) {
		if (!resize_scene_viewport(static_cast<std::uint32_t>(size >> 32), static_cast<std::uint32_t>(size))) {
			SceneTree::get_singleton()->quit(EXIT_FAILURE); return false;
		}
		applied_size = size;
	}
	if (now < next_draw) return false;
	const auto interval = 1000000 / target_fps.load();
	// 沿原目标时刻推进，避免与 Godot 自身的动态帧延迟叠加而隔帧跳过；长时间落后则丢掉积欠，不追赶多帧。
	next_draw = next_draw ? next_draw + interval : now + interval;
	if (next_draw <= now) next_draw = now + interval;
	Main::force_redraw(); return true;
}
void shutdown_surface_main() {
	active.store(false);
	if (additional_output) {
		DisplayServer::get_singleton()->unregister_additional_output(additional_output);
		memdelete(additional_output); additional_output = nullptr;
	}
}
void surface_frame_begin() {
#ifdef METON_SURFACE_FIXTURE_ENABLED
	fixture_frame_begin();
#endif
	if (current) current->poll();
}
void surface_frame_capture() { if (current) current->capture(); }
void surface_frame_submitted() {
	if (current) { current->submitted(); publish_status(); }
#ifdef METON_SURFACE_FIXTURE_ENABLED
	fixture_frame_submitted();
#endif
}
void shutdown_surface() {
	if (current) { current->stop(); current->poll(); delete current; current = nullptr; }
#ifdef METON_SURFACE_FIXTURE_ENABLED
	fixture_render_shutdown();
#endif
}
}
