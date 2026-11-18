#pragma once
#if defined(METON_JS_GAME_ENABLED) == defined(METON_JS_TASK_ENABLED)
#error "Meton V8 requires exactly one game or isolated task role"
#endif
#include <meton_gw3_identity.generated.h>
#ifndef METON_GW3_BINDING_DIGEST
#error "Meton GW3 binding identity is required"
#endif
#ifdef METON_JS_GAME_ENABLED
#if METON_GW3_ROLE != 1
#error "Game role cannot consume another role identity"
#endif
#else
#if METON_GW3_ROLE != 2
#error "Isolated task role cannot consume another role identity"
#endif
#endif
