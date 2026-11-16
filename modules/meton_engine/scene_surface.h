#pragma once
#include "core/math/vector2i.h"
#include "core/object/object_id.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "surface_capture.h"
#include <string>

namespace meton::bridge {
struct SceneSurfaceLaunch {
	CaptureContext capture;
	TransportLimits transport;
	std::uint64_t document_version = 0;
	String scene_path;
	Size2i viewport_size;
};

/** main 取当前场景的视口，仅将 RID 交给渲染线程建立捕获。 */
void bind_scene_viewport();
/** 配对控制在受限启动线程完成，随后把同一传输交给渲染线程。 */
enum class SceneHandshake { ready, incompatible, rejected };
SceneHandshake start_scene_control(const SceneSurfaceLaunch &launch, DWORD timeout_ms);
/** main 消费已经按当前代次和真实生产帧准入的有界输入。 */
void pump_scene_input();
/** 原场景上的实际选择。只有经过逐请求租约及文档复验的调用可进入。 */
const char *scene_document_selection(const Dictionary &command, const String &document, Dictionary &projection);
/** 在已绑定的真实编辑树中定位目标并序列化候选，持久确认前保留当前树。 */
struct SceneTransformEdit {
	ObjectID node;
	Variant before, after;
};
struct SceneDragTarget {
	ObjectID node, camera;
	String node_id;
	Variant local, screen;
	Point2 start;
	real_t depth = 0;
	Vector3 plane_origin, plane_x, plane_y;
};
/** 命中与坐标变换都取自展示这份文档的真实 Godot 视口。 */
const char *begin_scene_drag(const String &document, Point2 at, SceneDragTarget &target);
const char *finish_scene_drag(const SceneDragTarget &target, Point2 at, Dictionary &change);
const char *prepare_scene_transform(const String &source, const String &document, const Dictionary &change, SceneTransformEdit &edit, String &output, bool translation_only = false);
const char *confirm_scene_transform(const SceneTransformEdit &edit, uint64_t version);
/** 准备历史材料前复验实际编辑树、原视口及剩余事件序号。 */
const char *check_scene_history_target();
/** A09 持久确认后，在编辑线程用已核对的原始字节恢复场景。 */
const char *confirm_scene_history(const std::string &source, uint64_t version);
/** main 按当前布局调整已绑定的同一 Godot 视口，保留场景与文档版本。 */
bool resize_scene_viewport(std::uint32_t width, std::uint32_t height);
}
