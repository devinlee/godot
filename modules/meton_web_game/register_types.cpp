#include "register_types.h"
#include "web_game.h"
#include "web_script.h"
#include "core/object/class_db.h"
void initialize_meton_web_game_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        GDREGISTER_CLASS(MetonWebGame);
        GDREGISTER_INTERNAL_CLASS(MetonWebScript);
        MetonGameContent::initialize_loader();
    }
}
void uninitialize_meton_web_game_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) MetonGameContent::shutdown_loader();
}
