// 由 game_resources.ts 生成；只读编译输入。
#pragma once
namespace MetonResourceSupport {
inline constexpr const char *INPUT_DIGEST = "99145566ed8e8f8574c3cf70f4e07ef29b46bee59f8993d92f1d05114d21b2a3";
inline constexpr int CONTENT_FILES = 256;
inline constexpr int CONTENT_BYTES = 33554432;
inline constexpr int CONTENT_DEPTH = 16;
inline constexpr int NATIVE_RESOURCES = 128;
inline constexpr int GPU_PIXELS = 4194304;
inline constexpr const char *NODE_TYPES[] = {"Node", "Node2D", "CharacterBody2D", "Sprite2D"};

struct Class { const char *name; const char *base; };
inline constexpr Class CLASSES[] = {
    {"RefCounted", "Object"},
    {"Resource", "RefCounted"},
    {"Texture", "Resource"},
    {"Texture2D", "Texture"},
};
struct Member { const char *id; unsigned phases; };
inline constexpr Member MEMBERS[] = {
    {"Sprite2D.texture.get", 382},
    {"Sprite2D.texture.set", 382},
    {"ResourceLoader.load", 380},
};
}
