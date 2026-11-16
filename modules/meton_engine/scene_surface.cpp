#include "material_admission.h"
#include "core/object/callable_mp.h"
#include "core/string/print_string.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"
#ifdef TOOLS_ENABLED
#include "core/io/file_access_memory.h"
#include "scene/resources/resource_format_text.h"
#include "core/variant/variant_parser.h"
#include "scene/2d/node_2d.h"
#include "editor/editor_interface.h"
#include "editor/editor_data.h"
#include "editor/editor_undo_redo_manager.h"
#include "core/object/undo_redo.h"
#include "editor/editor_node.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "core/math/triangle_mesh.h"
#include "scene/gui/subviewport_container.h"
#endif
// 先解析 Godot 的枚举，之后才由 D3D12 接口引入同名的 Win32 宏。
#include "scene_surface.h"
#include "scene_input.h"
#include "surface_runtime.h"
#include <atomic>
#include <charconv>
#include <mutex>

#ifdef TOOLS_ENABLED
/** 只接受原持久检查点的内存，不注册额外资源路径。候选内部资源与活动场景分开。 */
class MetonSceneHistoryLoader {
public:
	static Node *instantiate(const std::string &source, const String &path) {
		Ref<FileAccessMemory> memory;
		memory.instantiate();
		if (memory->open_custom(reinterpret_cast<const uint8_t *>(source.data()),source.size()) != OK) return nullptr;
		ResourceLoaderText parser;
		parser.local_path = path; parser.res_path = path;
		parser.cache_mode = ResourceFormatLoader::CACHE_MODE_IGNORE;
		parser.cache_mode_for_external = ResourceFormatLoader::CACHE_MODE_REUSE;
		parser.open(memory);
		if (parser.load() != OK) return nullptr;
		Ref<PackedScene> packed = parser.get_resource();
		return packed.is_valid() ? packed->instantiate(PackedScene::GEN_EDIT_STATE_MAIN) : nullptr;
	}
};
#endif

namespace meton::bridge {
namespace {
enum class BindingState { waiting, queued, ready, failed };
std::atomic<BindingState> binding{BindingState::waiting};
std::uint64_t started_usec = 0;
ObjectID bound_viewport;
std::mutex scene_pipe_mutex;
std::shared_ptr<SurfaceTransport> startup_pipe;
std::weak_ptr<SurfaceTransport> document_pipe;
#ifdef TOOLS_ENABLED
ObjectID document_root;
uint64_t loaded_history_version = 0;
uint64_t document_event_sequence = 0;
#endif

/** 管道由 E01 创建并唯一继承。这里只还原句柄数值，类型及方向由传输层复核。 */
HANDLE inherited(const wchar_t *name) {
	wchar_t raw[32]{};
	const auto count = GetEnvironmentVariableW(name, raw, 32);
	if (!count || count >= 32) return nullptr;
	char digits[32]{};
	for (DWORD i = 0; i < count; ++i) {
		if (raw[i] < L'0' || raw[i] > L'9') return nullptr;
		digits[i] = static_cast<char>(raw[i]);
	}
	std::uintptr_t value = 0;
	const auto result = std::from_chars(digits, digits + count, value);
	return result.ec == std::errc{} && result.ptr == digits + count ? reinterpret_cast<HANDLE>(value) : nullptr;
}

/** 此回调运行在渲染线程。场景节点只由 main 访问，设备、纹理和捕获器只在此处建立。 */
void install_scene(RID viewport) {
	const auto &launch = *material_surface();
	std::shared_ptr<SurfaceTransport> transport;
	if (launch.transport.allow_scene_hello) {
		std::lock_guard<std::mutex> lock(scene_pipe_mutex);
		transport = std::move(startup_pipe);
	} else {
		transport = SurfaceTransport::open(inherited(L"METON_ENGINE_COMMAND_HANDLE"), inherited(L"METON_ENGINE_EVENT_HANDLE"),
				launch.capture.instance_generation, launch.transport);
	}
	auto *rs = RenderingServer::get_singleton();
	auto capture = launch.capture;
	capture.input_queue_limit = launch.transport.max_queue_messages;
	const bool installed = transport && install_surface(capture, transport, rs->viewport_get_texture(viewport), launch.document_version);
	if (!installed && transport) transport->stop();
	binding.store(installed ? BindingState::ready : BindingState::failed);
}

/** 编辑器取场景专属编辑视口，等待异步打开的目标场景就绪；绝不选编辑器根窗口。 */
Viewport *scene_viewport(SceneTree *tree, const SceneSurfaceLaunch &launch) {
#ifdef TOOLS_ENABLED
	auto *editor = EditorInterface::get_singleton();
	if (!EditorNode::get_singleton() || EditorNode::get_singleton()->is_changing_scene()) return nullptr;
	auto *scene = editor ? editor->get_edited_scene_root() : nullptr;
	if (!scene || !scene->is_inside_tree() || scene->get_scene_file_path() != launch.scene_path) return nullptr;
	const bool spatial = Object::cast_to<Node3D>(scene) != nullptr;
	editor->set_main_screen_editor(spatial ? "3D" : "2D");
	auto *viewport = spatial ? editor->get_editor_viewport_3d() : editor->get_editor_viewport_2d();
	if (!viewport) return nullptr;
	// 隐藏的编辑器外框布局不能决定宿主视口大小；仍使用原生编辑器的画布、相机和节点。
	if (auto *container = Object::cast_to<SubViewportContainer>(viewport->get_parent())) container->set_stretch(false);
	viewport->set_size(launch.viewport_size);
	if (!spatial) RenderingServer::get_singleton()->viewport_set_disable_2d(viewport->get_viewport_rid(), false);
	return viewport;
#else
	auto *scene = tree->get_current_scene();
	return scene && scene->is_inside_tree() && scene->get_scene_file_path() == launch.scene_path ? tree->get_root() : nullptr;
#endif
}
}

SceneHandshake start_scene_control(const SceneSurfaceLaunch &launch, DWORD timeout_ms) {
	auto transport = SurfaceTransport::open(inherited(L"METON_ENGINE_COMMAND_HANDLE"), inherited(L"METON_ENGINE_EVENT_HANDLE"),
			launch.capture.instance_generation, launch.transport);
	if (!transport) return SceneHandshake::rejected;
	const auto request = transport->wait_hello(timeout_ms);
	bool compatible = false;
	const auto reply = request.empty() ? std::string() : material_hello(request, compatible);
	if (reply.empty() || !transport->reply_hello(reply)) { transport->stop(); return SceneHandshake::rejected; }
	if (!compatible) {
		// 宿主收到实际支持范围后将关闭本代。让原应答写完，避免不兼容的进程进入图形初始化。
		transport->wait_stopped(launch.capture.shutdown_timeout_ms);
		transport->stop();
		return SceneHandshake::incompatible;
	}
	// 只读试播先等原快照的启动命令。未确认前不会加载场景、模拟或出帧。
	const auto start_deadline = GetTickCount64() + launch.capture.operation_timeout_ms;
	while (awaiting_preview_start()) {
		const auto now = GetTickCount64();
		SceneDocumentRequest command;
		if (now >= start_deadline || !transport->wait_document(command, static_cast<DWORD>(start_deadline - now)) ||
				!transport->reply_document(command, material_document(command.route,command.body))) {
			transport->stop(); return SceneHandshake::rejected;
		}
	}
	std::lock_guard<std::mutex> lock(scene_pipe_mutex);
	if (launch.transport.max_document_bytes) document_pipe = transport;
	startup_pipe = std::move(transport); return SceneHandshake::ready;
}

void bind_scene_viewport() {
	const auto *launch = material_surface();
	auto *tree = SceneTree::get_singleton();
	if (!launch || !tree || binding.load() == BindingState::ready) return;
	const auto now = OS::get_singleton()->get_ticks_usec();
	if (!started_usec) started_usec = now;
	if (binding.load() == BindingState::failed || now - started_usec >= std::uint64_t(launch->capture.operation_timeout_ms) * 1000) {
		ERR_PRINT_ONCE("Meton scene viewport binding failed.");
		tree->quit(EXIT_FAILURE);
		return;
	}
	if (binding.load() == BindingState::queued) return;
	auto *viewport = scene_viewport(tree, *launch);
	if (!viewport) return;
#ifdef TOOLS_ENABLED
	if (launch->transport.max_document_bytes) {
		auto *root = EditorInterface::get_singleton()->get_edited_scene_root();
		auto *undo = EditorUndoRedoManager::get_singleton()->get_history_for_object(root).undo_redo;
		document_root = root->get_instance_id();
		loaded_history_version = undo->get_version();
	}
#endif
	print_verbose("Meton scene viewport: " + String::num_int64(viewport->get_visible_rect().size.x) + "x" +
			String::num_int64(viewport->get_visible_rect().size.y));
	const RID rid = viewport->get_viewport_rid();
	bound_viewport = viewport->get_instance_id();
	auto *rs = RenderingServer::get_singleton();
#ifndef TOOLS_ENABLED
	// 试播根视口仍绘制到自己的纹理，解除本进程隐藏窗口的屏幕输出。
	tree->get_root()->set_content_scale_mode(Window::CONTENT_SCALE_MODE_VIEWPORT);
	rs->viewport_attach_to_screen(rid, Rect2(), DisplayServerEnums::INVALID_WINDOW_ID);
#endif
	rs->viewport_set_update_mode(rid, RenderingServerEnums::VIEWPORT_UPDATE_ALWAYS);
	binding.store(BindingState::queued);
	rs->call_on_render_thread(callable_mp_static(&install_scene).bind(rid));
}

void pump_scene_input() {
	dispatch_scene_input(Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport)));
	const auto pipe = document_pipe.lock();
	if (!pipe || binding.load() != BindingState::ready) return;
	SceneDocumentRequest request;
	// 每个主循环最多处理一个请求，图形和输入继续推进。队列受启动策略约束。
	if (pipe->take_document(request) && !pipe->reply_document(request, material_document(request.route, request.body))) pipe->stop();
}

#ifdef TOOLS_ENABLED
/** 场景切换、外部撤销历史变更或根节点换代时封锁原文档。 */
static const char *current_document_root(Node *&root) {
	auto *editor = EditorInterface::get_singleton();
	root = editor ? editor->get_edited_scene_root() : nullptr;
	const auto *launch = material_surface();
	if (!launch || !root || !root->is_inside_tree() || root->get_instance_id() != document_root ||
			root->get_scene_file_path() != launch->scene_path || EditorNode::get_singleton()->is_changing_scene()) return "recovery_required";
	auto *undo = EditorUndoRedoManager::get_singleton()->get_history_for_object(root).undo_redo;
	return undo->get_version() == loaded_history_version ? nullptr : "version_conflict";
}
static String scene_node_id(Node *root, Node *node, const String &document) {
	return "node:" + (document + "\n" + String(root->get_path_to(node))).sha256_text();
}
/** 先验证所有目标。混入未登记节点时，不能留下已经部分替换的选区。 */
static const char *select_nodes(Node *root, const String &document, const Array &requested, EditorSelection *selection) {
	Dictionary wanted;
	for (const Variant &key : requested) {
		if (key.get_type() != Variant::STRING || wanted.has(key)) return "invalid_argument";
		wanted[key] = true;
	}
	List<Node *> stack, targets;
	stack.push_back(root);
	while (!stack.is_empty() && targets.size() != requested.size()) {
		Node *node = stack.front()->get(); stack.pop_front();
		if ((node == root || node->get_owner() == root) && wanted.has(scene_node_id(root,node,document))) targets.push_back(node);
		for (int i = 0; i < node->get_child_count(); ++i) stack.push_back(node->get_child(i));
	}
	if (targets.size() != requested.size()) return "invalid_argument";
	const List<Node *> previous = selection->get_full_selected_node_list();
	bool same = previous.size() == requested.size();
	for (Node *node : previous) {
		if (node != root && !root->is_ancestor_of(node)) return "recovery_required";
		if (!wanted.has(scene_node_id(root,node,document))) same = false;
	}
	if (same) return nullptr;
	if (document_event_sequence == UINT64_MAX) return "budget_exceeded";
	selection->clear();
	for (Node *node : targets) selection->add_node(node);
	++document_event_sequence;
	if (selection->get_selection().size() != targets.size()) return "recovery_required";
	for (Node *node : targets) if (!selection->is_selected(node)) return "recovery_required";
	return nullptr;
}
#endif

const char *scene_document_selection(const Dictionary &command, const String &document, Dictionary &projection) {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	auto *selection = EditorInterface::get_singleton()->get_selection();
	if (command.has("node_ids")) {
		if (const char *error = select_nodes(root,document,command["node_ids"],selection)) return error;
	} else {
		const String cursor = command["after_event"], current = String::num_uint64(document_event_sequence);
		if (cursor.length() > current.length() || (cursor.length() == current.length() && current < cursor)) return "recovery_required";
	}
	Array selected;
	const auto nodes = selection->get_full_selected_node_list();
	if (nodes.size() > 64) return "budget_exceeded";
	for (Node *node : nodes) {
		if (node != root && !root->is_ancestor_of(node)) return "recovery_required";
		selected.push_back(scene_node_id(root,node,document));
	}
	selected.sort();
	projection["selected_node_ids"] = selected; projection["event_sequence"] = String::num_uint64(document_event_sequence);
	return nullptr;
#else
	return "not_supported";
#endif
}

#ifdef TOOLS_ENABLED
/** 以画布中实际可见节点及相机射线作命中检查，不接受调用者提供的节点标识。 */
static Node *drag_hit(Node *root, Viewport *viewport, Camera3D *camera, Point2 at) {
	List<Node *> nodes;
	nodes.push_back(root);
	Node *hit = nullptr;
	real_t closest = Math::INF;
	bool ambiguous = false;
	while (!nodes.is_empty()) {
		Node *node = nodes.front()->get();
		nodes.pop_front();
		for (int i = 0; i < node->get_child_count(); ++i) nodes.push_back(node->get_child(i));
		if ((node != root && node->get_owner() != root) || (!camera && node->get_viewport() != viewport)) continue;
		if (!camera) {
			auto *item = Object::cast_to<Node2D>(node);
			if (!item || !item->is_visible_in_tree()) continue;
			const Transform2D screen = viewport->get_global_canvas_transform() * item->get_global_transform_with_canvas();
			if (Math::is_zero_approx(screen.determinant())) continue;
			const Point2 local = screen.affine_inverse().xform(at);
			if (!item->_edit_is_selected_on_click(local, 8.0 / MAX(screen[0].length(), screen[1].length()))) continue;
			if (hit) ambiguous = true;
		} else {
			auto *item = Object::cast_to<MeshInstance3D>(node);
			if (!item || !item->is_visible_in_tree() || item->get_world_3d() != camera->get_world_3d() ||
					item->get_mesh().is_null() || !(item->get_layer_mask() & camera->get_cull_mask())) continue;
			const Transform3D world = item->get_global_transform();
			if (Math::is_zero_approx(world.basis.determinant())) continue;
			const Transform3D inverse = world.affine_inverse();
			const Vector3 ray = camera->project_ray_origin(at), from = inverse.xform(ray), dir = inverse.basis.xform(camera->project_ray_normal(at)).normalized();
			if (!item->get_aabb().intersects_ray(from, dir)) continue;
			const Ref<TriangleMesh> mesh = item->get_mesh()->generate_triangle_mesh();
			Vector3 point, normal;
			if (mesh.is_null() || !mesh->intersect_ray(from, dir, point, normal)) continue;
			const Vector3 intersection = world.xform(point);
			const real_t depth = -camera->get_camera_transform().affine_inverse().xform(intersection).z;
			if (depth < camera->get_near() || depth > camera->get_far()) continue;
			const real_t distance = ray.distance_squared_to(intersection);
			if (hit && Math::is_equal_approx(distance, closest)) { ambiguous = true; continue; }
			if (distance >= closest) continue;
			closest = distance;
			ambiguous = false;
		}
		hit = node;
	}
	return ambiguous ? nullptr : hit;
}
#endif

const char *begin_scene_drag(const String &document, Point2 at, SceneDragTarget &target) {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!viewport) return "scope_ended";
	auto *camera = Object::cast_to<Node3D>(root) ? viewport->get_camera_3d() : nullptr;
	if (!camera && Object::cast_to<Node3D>(root)) return "recovery_required";
	Node *node = drag_hit(root, viewport, camera, at);
	if (!node) return "invalid_argument";
	target.node = node->get_instance_id();
	target.node_id = scene_node_id(root,node,document);
	target.start = at;
	target.local = node->get("transform");
	if (camera) {
		auto *item = Object::cast_to<Node3D>(node);
		const Transform3D world = item->get_global_transform();
		target.screen = world;
		target.camera = camera->get_instance_id();
		target.depth = -camera->get_camera_transform().affine_inverse().xform(world.origin).z;
		if (target.depth <= 0) return "invalid_argument";
		target.plane_origin = camera->project_position(at, target.depth);
		target.plane_x = camera->project_position(at + Point2(1,0), target.depth);
		target.plane_y = camera->project_position(at + Point2(0,1), target.depth);
	} else {
		auto *item = Object::cast_to<Node2D>(node);
		target.screen = viewport->get_global_canvas_transform() * item->get_global_transform_with_canvas();
	}
	return nullptr;
#else
	return "not_supported";
#endif
}

const char *finish_scene_drag(const SceneDragTarget &target, Point2 at, Dictionary &change) {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	auto *node = Object::cast_to<Node>(ObjectDB::get_instance(target.node));
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!viewport || !node || (node != root && !root->is_ancestor_of(node)) ||
			node->get("transform") != target.local || at == target.start) return "invalid_argument";
	change["node_id"] = target.node_id;
	if (auto *item = Object::cast_to<Node2D>(node)) {
		const Transform2D screen = viewport->get_global_canvas_transform() * item->get_global_transform_with_canvas();
		if (screen != Transform2D(target.screen) || !item->is_visible_in_tree()) return "version_conflict";
		const Vector2 delta = item->get_transform().basis_xform(screen.affine_inverse().basis_xform(at-target.start));
		const Vector2 position = item->get_position()+delta;
		change["kind"] = "transform2d"; change["position"] = Array{position.x, position.y};
	} else {
		auto *spatial = Object::cast_to<Node3D>(node);
		auto *camera = viewport->get_camera_3d();
		if (!spatial || !spatial->is_visible_in_tree() || !camera || camera->get_instance_id() != target.camera ||
				spatial->get_global_transform() != Transform3D(target.screen) ||
				!camera->project_position(target.start, target.depth).is_equal_approx(target.plane_origin) ||
				!camera->project_position(target.start + Point2(1,0), target.depth).is_equal_approx(target.plane_x) ||
				!camera->project_position(target.start + Point2(0,1), target.depth).is_equal_approx(target.plane_y)) return "version_conflict";
		const Vector3 delta = camera->project_position(at, target.depth)-target.plane_origin;
		const Vector3 position = spatial->get_position() + spatial->get_transform().basis.xform(spatial->get_global_transform().basis.inverse().xform(delta));
		change["kind"] = "transform3d"; change["position"] = Array{position.x, position.y, position.z};
	}
	return nullptr;
#else
	return "not_supported";
#endif
}

#ifdef TOOLS_ENABLED
/** 数值受纯数据文本准入的相同上限约束。 */
static bool component(const Variant &value, real_t &out) {
	if (value.get_type() != Variant::FLOAT && value.get_type() != Variant::INT) return false;
	double number = value;
	if (!Math::is_finite(number) || Math::abs(number) > 1.0e9) return false;
	out = static_cast<real_t>(number);
	return true;
}

static bool components(const Dictionary &change, const char *key, int count, real_t *out) {
	const Variant value = change.get(key, Variant());
	if (value.get_type() != Variant::ARRAY) return false;
	const Array array = value;
	if (array.size() != count) return false;
	for (int i = 0; i < count; ++i) if (!component(array[i], out[i])) return false;
	return true;
}

static bool local_transform(Node *node, const Dictionary &change, SceneTransformEdit &edit, bool translation_only) {
	const bool two = change.get("kind", "") == Variant("transform2d");
	real_t pos[3]{}, scale[3]{}, rotation[4]{};
	if (!components(change, "position", two ? 2 : 3, pos) || (!translation_only && !components(change, "scale", two ? 2 : 3, scale))) return false;
	if (two) {
		auto *target = Object::cast_to<Node2D>(node);
		if (!target || (!translation_only && !component(change.get("rotation", Variant()), rotation[0]))) return false;
		edit.before = target->get_transform();
		Transform2D after = translation_only ? target->get_transform() : Transform2D(rotation[0], Size2(scale[0], scale[1]), target->get_skew(), Vector2());
		after.set_origin(Vector2(pos[0], pos[1]));
		edit.after = after;
	} else {
		auto *target = Object::cast_to<Node3D>(node);
		if (!target) return false;
		edit.before = target->get_transform();
		Transform3D after = target->get_transform();
		if (!translation_only) {
			if (!components(change, "rotation_quaternion", 4, rotation)) return false;
			const Quaternion q(rotation[0], rotation[1], rotation[2], rotation[3]);
			if (!q.is_normalized()) return false;
			after.basis = Basis(q.normalized(), Vector3(scale[0], scale[1], scale[2]));
		}
		after.origin = Vector3(pos[0], pos[1], pos[2]);
		edit.after = after;
	}
	edit.node = node->get_instance_id();
	return true;
}

/** 渲染线程在下一次绘制前切换已确认的帧版本。 */
static void publish_document_version(RID viewport, uint64_t version) {
	auto *rs = RenderingServer::get_singleton();
	if (!update_surface_source(rs->viewport_get_texture(viewport), version)) stop_surface();
}
#endif

const char *prepare_scene_transform(const String &source, const String &document, const Dictionary &change, SceneTransformEdit &edit, String &output, bool translation_only) {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	const String wanted = change.get("node_id", ""), newline = source.contains("\r\n") ? "\r" : "";
	const Vector<String> lines = source.split("\n", true);
	Vector<String> result;
	bool match = false, found = false;
	for (const String &line : lines) {
		const String stripped = line.strip_edges();
		if (stripped.begins_with("[")) {
			match = false;
			if (stripped.begins_with("[node ")) {
				VariantParser::StreamString stream;
				stream.s = stripped;
				VariantParser::Tag tag;
				int number = 1;
				String reason;
				if (VariantParser::parse_tag(&stream, number, reason, tag) != OK || !tag.fields.has("name") || !tag.fields.has("type")) return "recovery_required";
				const String name = tag.fields["name"], parent = tag.fields.has("parent") ? String(tag.fields["parent"]) : String();
				const String relative = parent.is_empty() ? "." : parent == "." ? name : parent + "/" + name;
				match = wanted == "node:" + (document + "\n" + relative).sha256_text();
				if (match) {
					Node *node = parent.is_empty() ? root : root->get_node_or_null(NodePath(relative));
					if (found || !node || node->get_name() != StringName(name) || node->get_class() != String(tag.fields["type"]) ||
							String(root->get_path_to(node)) != relative) return "recovery_required";
					if (!local_transform(node, change, edit,translation_only)) return "invalid_argument";
					String serialized;
					if (VariantWriter::write_to_string(edit.after, serialized) != OK) return "recovery_required";
					// 确认时采用从持久文本读回的变换，使本次编辑与冷启动的数值一致。
					VariantParser::StreamString roundtrip;
					roundtrip.s = serialized;
					Variant value;
					if (VariantParser::parse(&roundtrip, value, reason, number) != OK || value.get_type() != edit.after.get_type()) return "recovery_required";
					edit.after = value;
					result.push_back(line.trim_suffix("\r") + newline);
					result.push_back("transform = " + serialized + newline);
					found = true;
					continue;
				}
			}
		} else if (match && !stripped.begins_with(";") && stripped.contains("=")) {
			const String key = stripped.get_slice("=", 0).strip_edges();
			if (key == "transform" || key == "position" || key == "rotation" || key == "rotation_degrees" || key == "scale") continue;
		}
		result.push_back(line);
	}
	if (!found) return "invalid_argument";
	output = String("\n").join(result);
	return nullptr;
#else
	return "not_supported";
#endif
}

const char *confirm_scene_transform(const SceneTransformEdit &edit, uint64_t version) {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	if (document_event_sequence == UINT64_MAX) return "budget_exceeded";
	auto *node = Object::cast_to<Node>(ObjectDB::get_instance(edit.node));
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!node || !viewport || (node != root && !root->is_ancestor_of(node)) || node->get("transform") != edit.before) return "recovery_required";
	node->set("transform", edit.after);
	if (node->get("transform") != edit.after) return "recovery_required";
	++document_event_sequence;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&publish_document_version).bind(viewport->get_viewport_rid(), version));
	return nullptr;
#else
	return "not_supported";
#endif
}

const char *check_scene_history_target() {
#ifdef TOOLS_ENABLED
	Node *root = nullptr;
	if (const char *error = current_document_root(root)) return error;
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!viewport || !viewport->is_inside_tree()) return "recovery_required";
	return document_event_sequence == UINT64_MAX ? "budget_exceeded" : nullptr;
#else
	return "not_supported";
#endif
}

const char *confirm_scene_history(const std::string &source, uint64_t version) {
#ifdef TOOLS_ENABLED
	Node *previous = nullptr;
	if (const char *error = current_document_root(previous)) return error;
	if (document_event_sequence == UINT64_MAX) return "budget_exceeded";
	auto *editor = EditorInterface::get_singleton();
	auto *selection = editor->get_selection();
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!viewport || !selection) return "recovery_required";
	// 选区只沿原根内的稳定相对路径接续，被旧版本移除的节点自然离开选区。
	List<NodePath> selected;
	for (Node *node : selection->get_full_selected_node_list()) {
		if (node != previous && !previous->is_ancestor_of(node)) return "recovery_required";
		selected.push_back(previous->get_path_to(node));
	}
	const auto &launch = *material_surface();
	Node *next = MetonSceneHistoryLoader::instantiate(source,launch.scene_path);
	if (!next) return "recovery_required";
	next->set_scene_file_path(launch.scene_path);
	const Size2i size = viewport->get_visible_rect().size;
	const bool spatial = Object::cast_to<Node3D>(next) != nullptr;
	auto *target = spatial ? editor->get_editor_viewport_3d() : editor->get_editor_viewport_2d();
	if (!target || !target->is_inside_tree()) { memdelete(next); return "recovery_required"; }
	selection->clear();
	EditorNode::get_singleton()->set_edited_scene(next);
	if (editor->get_edited_scene_root() != next || !next->is_inside_tree()) {
		EditorNode::get_singleton()->set_edited_scene(previous);
		for (const NodePath &path : selected) if (Node *node = previous->get_node_or_null(path)) selection->add_node(node);
		memdelete(next); return "recovery_required";
	}
	if (target != viewport) {
		editor->set_main_screen_editor(spatial ? "3D" : "2D");
		if (auto *container = Object::cast_to<SubViewportContainer>(target->get_parent())) container->set_stretch(false);
		target->set_size(size);
		if (!spatial) RenderingServer::get_singleton()->viewport_set_disable_2d(target->get_viewport_rid(),false);
		RenderingServer::get_singleton()->viewport_set_update_mode(target->get_viewport_rid(),RenderingServerEnums::VIEWPORT_UPDATE_ALWAYS);
	}
	for (const NodePath &path : selected) {
		Node *node = next->get_node_or_null(path);
		if (node && (node == next || node->get_owner() == next)) selection->add_node(node);
	}
	bound_viewport = target->get_instance_id(); document_root = next->get_instance_id();
	loaded_history_version = EditorUndoRedoManager::get_singleton()->get_history_for_object(next).undo_redo->get_version();
	previous->queue_free();
	++document_event_sequence;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&publish_document_version).bind(target->get_viewport_rid(),version));
	return nullptr;
#else
	return "not_supported";
#endif
}

bool resize_scene_viewport(std::uint32_t width, std::uint32_t height) {
	auto *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(bound_viewport));
	if (!viewport || !viewport->is_inside_tree()) return false;
	const Size2i size(MAX(2U, width), MAX(2U, height));
#ifdef TOOLS_ENABLED
	static_cast<SubViewport *>(viewport)->set_size(size);
#else
	// 窗口标题栏的系统最小尺寸不能约束试播纹理。使用 Godot 自己的渲染分辨率，保持 HWND 隐藏。
	static_cast<Window *>(viewport)->set_content_scale_size(size);
	RenderingServer::get_singleton()->viewport_attach_to_screen(viewport->get_viewport_rid(), Rect2(), DisplayServerEnums::INVALID_WINDOW_ID);
#endif
	return true;
}
}
