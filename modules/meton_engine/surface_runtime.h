#pragma once
#include "surface_capture.h"

namespace meton::bridge {
/** 仅受信 G03 启动/场景所有者在渲染线程调用；同一进程只装配一个当前表面会话。 */
bool install_surface(const CaptureContext &context, std::shared_ptr<SurfaceTransport> transport, RID viewport_texture, std::uint64_t version);
bool update_surface_source(RID viewport_texture, std::uint64_t version);
CaptureStatus surface_status();
void stop_surface();
/** main 每次迭代调用，决定本次是否允许绘制；控制与收尾不依赖绘制。 */
bool surface_main_iteration(bool wants_present);
/** main 模块退出时解除自身附加输出，不跨线程销毁渲染对象。 */
void shutdown_surface_main();
/** 三个明确的原生渲染边界，由 fork 的 RenderingServerDefault 调用。 */
void surface_frame_begin();
void surface_frame_capture();
void surface_frame_submitted();
/** 模块退出时排入原渲染线程；不在 main 同步等待外部线程或 GPU。 */
void shutdown_surface();
#ifdef METON_SURFACE_FIXTURE_ENABLED
void initialize_surface_fixture();
void fixture_frame_begin();
void fixture_frame_submitted();
void fixture_render_shutdown();
void fixture_runtime_poll();
#endif
}
