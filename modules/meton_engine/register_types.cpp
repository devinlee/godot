#include "register_types.h"
#include "surface_runtime.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

/** 正式机制不注册项目脚本类；只有受信启动装配能建立表面会话。 */
void initialize_meton_engine_module(ModuleInitializationLevel level) {
#ifdef METON_SURFACE_FIXTURE_ENABLED
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) meton::bridge::initialize_surface_fixture();
#else
	(void)level;
#endif
}
void uninitialize_meton_engine_module(ModuleInitializationLevel level) {
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	meton::bridge::shutdown_surface_main();
	if (RenderingServer::get_singleton()) RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&meton::bridge::shutdown_surface));
}
