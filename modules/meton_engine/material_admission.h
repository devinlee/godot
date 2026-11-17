#pragma once
#include "core/string/ustring.h"
#include "core/templates/list.h"
#include <string>

namespace meton::bridge {
struct SceneSurfaceLaunch;
/** 仅已验证封印的正常场景返回表面上下文，其生命周期覆盖本进程渲染。 */
const SceneSurfaceLaunch *material_surface();
/** 仅在 Godot 主线程处理本代已封印的 G01 握手。空答复要求封锁原通道。 */
std::string material_hello(const std::string &body, bool &compatible);
std::string material_document(const std::string &route, const std::string &body);
/** 冻结试播在原租约确认精确检查点后才允许 Godot 加载项目。 */
bool awaiting_preview_start();
/** 在处理会产生副作用的命令行和读取项目配置之前核对复制封印与材料闭包。 */
bool admit_materials(const List<String> &arguments);
/** 开启后禁用自动 PCK 与来源端 UID 缓存，资源只能来自同一份准入。 */
bool materials_active();
/** 返回已复验代次持有的 user:// 目录，独立于个人配置及编辑器缓存。 */
String material_user_directory();
/** Godot 自身的缓存根，独立于 Windows 为该 AppContainer 派生的包存储目录。 */
String material_cache_directory();
/** 在 Godot 自身的路径解析已就绪时核对 user://、缓存与设置归属。 */
bool material_storage_matches();
/** UID 仅从已校验清单安装，不从可写缓存恢复另一份权威。 */
void install_material_uids();
/** 按来源文档的已验引用边和 UID 解析；已接管且结果为空表示拒绝。 */
bool material_external_resource(const String &owner, const String &original, const String &type, const String &uid, String &resolved);
/** 固定 PNG 的内部 CTEX 路径由原文档的导入回执导出。 */
String material_mounted_texture(const String &resource);
/** 主线程当前原稿的真实场景路径。 */
String material_editor_scene_path();
/** 加载前和重映射后都核对；缓存命中不能绕过本代资源闭包。 */
bool material_resource_allowed(const String &path);
/** 只允许经原 PNG 名称重映射到它自己的 CTEX。 */
bool material_load_allowed(const String &path, const String &original);
/** 准备标志与受信清单阶段同时成立时只运行内置 PNG 导入。 */
bool preparing_material_textures();
/** 独立内容工具的只读检查已结束，直接交还本次任务的退出码。 */
bool checking_materials_only();
#ifdef TOOLS_ENABLED
/** 在编辑器场景主循环建立之前交出导入结果。 */
bool run_material_texture_preparation();
#endif
}
