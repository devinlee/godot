#include "register_types.h"
#include "meton_js_runtime.h"
#ifdef METON_JS_GAME_ENABLED
#include "meton_game_script.h"
#include "meton_game_content.h"
#endif
#include "core/object/class_db.h"

void initialize_meton_js_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        MetonJSRuntime::initialize_engine();
        GDREGISTER_CLASS(MetonJSRuntime);
#ifdef METON_JS_GAME_ENABLED
        MetonGameContent::initialize_loader();
        GDREGISTER_INTERNAL_CLASS(MetonGameScript);
#endif
    }
}
void uninitialize_meton_js_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        MetonJSRuntime::shutdown_engine();
#ifdef METON_JS_GAME_ENABLED
        MetonGameContent::shutdown_loader();
#endif
    }
}
