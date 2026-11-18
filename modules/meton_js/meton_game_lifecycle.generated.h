// 由 game_lifecycle.ts 生成；只有编译描述，没有运行支持声明。
#pragma once
namespace MetonLifecycleSupport {
inline constexpr const char *INPUT_DIGEST = "7cba20a22b6473d5e6338dbc9867ca856ae20be71253ef1cd717f2f76792b20d";
inline constexpr int CALLBACK_MS = 100;
inline constexpr int MICROTASKS = 128;
inline constexpr int EVENTS_QUEUED = 256;
inline constexpr int CONNECTIONS = 256;
inline constexpr int SIGNAL_FLAGS = 4;
struct Callback { const char *name; const char *phase; int arguments; };
inline constexpr Callback CALLBACKS[] = {
    {"_enter_tree", "enter_tree", 0},
    {"_ready", "ready", 0},
    {"_process", "process", 1},
    {"_physics_process", "physics", 1},
    {"_exit_tree", "cleanup", 0},
};
struct Member { const char *id; unsigned phases; };
inline constexpr Member MEMBERS[] = {
    {"Node._enter_tree", 4},
    {"Node._ready", 8},
    {"Node._process", 16},
    {"Node._physics_process", 32},
    {"Node._exit_tree", 128},
    {"Node.tree_exiting", 508},
    {"Callable.create", 380},
    {"Signal.connect", 380},
    {"Signal.disconnect", 508},
};
} // namespace MetonLifecycleSupport
