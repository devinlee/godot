#pragma once
// 身份由实际 GW3 组合生成；封存候选的摘要不能代表整合后的绑定。
#include "meton_js_role.h"
#ifndef METON_JS_GAME_ENABLED
#error "Game binding is not part of the isolated task role"
#endif
#define METON_GAME_BINDING_DIGEST METON_GW3_BINDING_DIGEST
