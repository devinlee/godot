#pragma once
#include "surface_capture.h"

class Viewport;

namespace meton::bridge {
/** 渲染线程发布真实出帧和显隐事实；main 不访问捕获器或 D3D 对象。 */
void update_input_scope(const CaptureStatus &status);
void record_input_frame(const SurfaceTransport::Packet &frame, std::size_t limit);
/** 私有文档端点的写输入也须在当前可见布局上命中真实已发布的帧。 */
bool gesture_frame(std::uint64_t frame, std::uint64_t version, std::uint32_t x, std::uint32_t y, CaptureStatus &current);
bool gesture_scope_current(const CaptureStatus &start);
/** 命令来自本代继承管道，待 main 再验后才能生成执行回执。 */
void queue_scene_input(const SurfaceTransport::Packet &packet, const std::shared_ptr<SurfaceTransport> &transport,
		std::size_t limit, std::uint32_t timeout_ms);
/** 只在主线程操作已绑定的场景视口。 */
void dispatch_scene_input(Viewport *viewport);
}
