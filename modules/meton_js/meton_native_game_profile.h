#pragma once

#ifdef METON_JS_LINUX_ENABLED
#include <meton_linux_game_profile.generated.h>
#else
#include "meton_game_runtime_profile.generated.h"
#include "meton_game_support.generated.h"
#include "meton_game_memory.generated.h"
#include "meton_game_ui.generated.h"

namespace MetonGameRuntimeProfile {
inline constexpr const char *TARGET = "windows-x64";
inline constexpr const char *ENGINE_REVISION = MetonGameSupport::GODOT_REVISION;
inline constexpr const char *TARGET_SUPPORT = MetonUiSupport::INPUT_DIGEST;
inline constexpr const char *CONTENT_POLICY = MetonMemorySupport::INPUT_DIGEST;
}
#endif
