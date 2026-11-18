// 由 game_impact.ts 生成；分项候选不表示 B4 整项通过。
#pragma once
namespace MetonImpactSupport {
inline constexpr const char *INPUT_DIGEST = "ba7c85188f014ff4101326004097880cf1a0629fd03fe38297c799bc80bdf5a8";
inline constexpr const char *FIELDS[] = {"Node2D.position", "CharacterBody2D.velocity", "CanvasItem.visible", "Sprite2D.texture"};
inline constexpr const char *PHASES[] = {"module", "construct", "enter_tree", "ready", "process", "physics", "signal", "cleanup", "safe_point"};
struct Member { const char *id; unsigned phases; };
inline constexpr Member MEMBERS[] = {
    {"Node.queue_free", 380},
    {"Object.free", 382},
};
}
