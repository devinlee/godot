#pragma once
#include "modules/register_module_types.h"
/** 注册实验执行器；不向业务 JS 暴露 Godot 对象。 */
void initialize_meton_js_module(ModuleInitializationLevel p_level);
/** 在所有实例释放后关闭本进程的 V8 平台。 */
void uninitialize_meton_js_module(ModuleInitializationLevel p_level);
