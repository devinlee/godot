#pragma once
namespace meton::bridge {
/** 固定渲染边界；接口不向 RenderingServer 引入 Windows、协议或场景业务依赖。 */
void surface_frame_begin();
void surface_frame_capture();
void surface_frame_submitted();
/** 只把是否绘制带回 main；Godot/Windows 对象仍封装在模块内部。 */
bool surface_main_iteration(bool wants_present);
}
