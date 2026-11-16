#include "core/input/input_event.h"
#include "scene/gui/control.h"
#include "scene/main/viewport.h"
#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#endif
#include "scene_input.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

namespace meton::bridge {
namespace wire = meton::engine::surface;
namespace {
struct Pending {
	wire::Packet packet;
	std::shared_ptr<SurfaceTransport> transport;
};
std::mutex input_mutex;
CaptureStatus scope;
std::deque<std::uint64_t> published;
std::deque<Pending> pending;
std::uint64_t last_request = 0;

std::uint64_t unix_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool in_scope(const wire::Packet &p) {
	return scope.visible && !scope.stopping && p.surface_generation == scope.surface_generation && p.control_sequence == scope.control_sequence &&
			p.extent_sequence == scope.extent_sequence && p.document_version == scope.document_version && p.input_x < scope.width && p.input_y < scope.height;
}
/** 回执完整回显原绑定；如果执行后无法入队，让宿主按断线未知收尾。 */
void answer(Pending &request, wire::InputResult result) {
	request.packet.kind = wire::Kind::input_ack;
	request.packet.input_result = result;
	if (!request.transport->send(request.packet)) request.transport->stop();
}
wire::InputResult apply(Viewport *viewport, const wire::Packet &p) {
	if (!viewport || !viewport->is_inside_tree()) return wire::input_stale;
	const Point2 pos(p.input_x, p.input_y);
#ifdef TOOLS_ENABLED
	if (p.input_action != wire::wheel_up && p.input_action != wire::wheel_down) return wire::input_denied;
	auto *editor = EditorInterface::get_singleton();
	if (editor && editor->get_editor_viewport_2d() == viewport) {
		CanvasItemEditor::get_singleton()->zoom_scene_view(p.input_action == wire::wheel_up, pos);
		return wire::camera_handled;
	}
	auto *spatial = Node3DEditor::get_singleton();
	auto *target = spatial ? spatial->get_editor_viewport(0) : nullptr;
	if (target && target->get_viewport_node() == viewport && target->zoom_scene_view(p.input_action == wire::wheel_up, pos)) return wire::camera_handled;
#else
	if (p.input_action != wire::pointer_motion) return wire::input_denied;
	Ref<InputEventMouseMotion> motion;
	motion.instantiate();
	motion->set_position(pos);
	motion->set_global_position(pos);
	const auto before = viewport->get_processed_events_count();
	viewport->push_input(motion, true);
	if (viewport->get_processed_events_count() != before) return viewport->gui_get_hovered_control() ? wire::motion_hover : wire::motion_empty;
	// Godot 也可能在嵌入窗口提前消费事件。已经交给引擎，缺乏处理证明时不能报告未执行。
	return wire::input_uncertain;
#endif
	return wire::input_denied;
}
}
void update_input_scope(const CaptureStatus &status) {
	std::lock_guard<std::mutex> lock(input_mutex);
	if (scope.control_sequence != status.control_sequence || scope.extent_sequence != status.extent_sequence ||
			scope.surface_generation != status.surface_generation || scope.document_version != status.document_version || status.stopping) published.clear();
	scope = status;
}
void record_input_frame(const wire::Packet &frame, std::size_t limit) {
	std::lock_guard<std::mutex> lock(input_mutex);
	if (!limit) return;
	if (published.size() >= limit) published.pop_front();
	published.push_back(frame.sequence);
}
bool gesture_frame(std::uint64_t frame, std::uint64_t version, std::uint32_t x, std::uint32_t y, CaptureStatus &current) {
	std::lock_guard<std::mutex> lock(input_mutex);
	current = scope;
	return scope.visible && !scope.stopping && scope.document_version == version && x < scope.width && y < scope.height &&
			std::find(published.begin(), published.end(), frame) != published.end();
}
bool gesture_scope_current(const CaptureStatus &start) {
	std::lock_guard<std::mutex> lock(input_mutex);
	return scope.visible && !scope.stopping && start.document_version == scope.document_version && start.surface_generation == scope.surface_generation &&
			start.control_sequence == scope.control_sequence && start.extent_sequence == scope.extent_sequence;
}
void queue_scene_input(const wire::Packet &packet, const std::shared_ptr<SurfaceTransport> &transport, std::size_t limit, std::uint32_t timeout_ms) {
	Pending request{packet, transport};
	wire::InputResult result = wire::input_stale;
	{
		std::lock_guard<std::mutex> lock(input_mutex);
		const auto now = unix_ms();
		if (packet.sequence > last_request) {
			last_request = packet.sequence;
			if (packet.deadline_ms <= now || packet.deadline_ms - now > timeout_ms) result = wire::input_expired;
			else if (in_scope(packet) && std::find(published.begin(), published.end(), packet.frame_sequence) != published.end()) {
				if (pending.size() < limit) { pending.push_back(std::move(request)); return; }
				result = wire::input_busy;
			}
		}
	}
	answer(request, result);
}
void dispatch_scene_input(Viewport *viewport) {
	std::deque<Pending> batch;
	{ std::lock_guard<std::mutex> lock(input_mutex); batch.swap(pending); }
	for (auto &request : batch) {
		if (request.transport->status().stopping) continue;
		bool current = false;
		{ std::lock_guard<std::mutex> lock(input_mutex); current = in_scope(request.packet); }
		// 不在锁内执行 Godot 的 GUI 回调，以免其向渲染线程发起同步请求时互等。
		const auto result = unix_ms() >= request.packet.deadline_ms ? wire::input_expired :
				current ? apply(viewport, request.packet) : wire::input_stale;
		answer(request, result);
	}
}
}
