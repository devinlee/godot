// 由 tools/engine/game_binding.ts 生成；只读编译描述，不是运行验收声明。
#pragma once
#include "core/variant/variant.h"
namespace MetonGameSupport {
inline constexpr const char *GODOT_REVISION = "ed1daf0bf001b61586d9930840f2f1394092c079";
inline constexpr const char *V8_VERSION = "15.7.112.43";
inline constexpr const char *SUPPORT_DIGEST = "6387f6f885f27997ed81da764329ef225acaccbdb8ec50cb5c3ed7bd0f60f091";
inline constexpr const char *METADATA_DIGEST = "8f7a4817026497402ed5fe0a9827c3b0be37e51f2004bc58ae63e402135ec44c";
inline constexpr int MAX_OBJECTS = 512;
inline constexpr int MAX_CALLS_PER_ENTRY = 2048;
enum Phase : uint32_t {
    P_MODULE = 1u << 0,
    P_CONSTRUCT = 1u << 1,
    P_ENTER_TREE = 1u << 2,
    P_READY = 1u << 3,
    P_PROCESS = 1u << 4,
    P_PHYSICS = 1u << 5,
    P_SIGNAL = 1u << 6,
    P_CLEANUP = 1u << 7,
    P_SAFE_POINT = 1u << 8,
};
enum MemberId {
    M_VECTOR2_NEW_ZERO,
    M_VECTOR2_NEW_XY,
    M_VECTOR2_X,
    M_VECTOR2_Y,
    M_NODE2D_NEW,
    M_CHARACTERBODY2D_NEW,
    M_SPRITE2D_NEW,
    M_NODE2D_POSITION_GET,
    M_NODE2D_POSITION_SET,
    M_CHARACTERBODY2D_VELOCITY_GET,
    M_CHARACTERBODY2D_VELOCITY_SET,
    M_CANVASITEM_VISIBLE_GET,
    M_CANVASITEM_VISIBLE_SET,
    M_INPUT_GET_AXIS,
    M_CHARACTERBODY2D_MOVE_AND_SLIDE,
    M_CHARACTERBODY2D_IS_ON_FLOOR,
    M_VARIANT_TYPE_TYPE_FLOAT,
    MEMBER_COUNT
};
struct Member { const char *id; const char *owner; const char *kind; const char *method; uint32_t phases; int arguments; Variant::Type result; Variant::Type arguments_types[2]; };
inline constexpr Member MEMBERS[] = {
    {"Vector2.new.zero", "Vector2", "constructor", "", P_MODULE | P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::VECTOR2, {Variant::NIL, Variant::NIL}},
    {"Vector2.new.xy", "Vector2", "constructor", "", P_MODULE | P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 2, Variant::VECTOR2, {Variant::FLOAT, Variant::FLOAT}},
    {"Vector2.x", "Vector2", "property_get", "", P_MODULE | P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT | P_CLEANUP, 0, Variant::FLOAT, {Variant::NIL, Variant::NIL}},
    {"Vector2.y", "Vector2", "property_get", "", P_MODULE | P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT | P_CLEANUP, 0, Variant::FLOAT, {Variant::NIL, Variant::NIL}},
    {"Node2D.new", "Node2D", "constructor", "", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::OBJECT, {Variant::NIL, Variant::NIL}},
    {"CharacterBody2D.new", "CharacterBody2D", "constructor", "", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::OBJECT, {Variant::NIL, Variant::NIL}},
    {"Sprite2D.new", "Sprite2D", "constructor", "", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::OBJECT, {Variant::NIL, Variant::NIL}},
    {"Node2D.position.get", "Node2D", "property_get", "get_position", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::VECTOR2, {Variant::NIL, Variant::NIL}},
    {"Node2D.position.set", "Node2D", "property_set", "set_position", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 1, Variant::NIL, {Variant::VECTOR2, Variant::NIL}},
    {"CharacterBody2D.velocity.get", "CharacterBody2D", "property_get", "get_velocity", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::VECTOR2, {Variant::NIL, Variant::NIL}},
    {"CharacterBody2D.velocity.set", "CharacterBody2D", "property_set", "set_velocity", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 1, Variant::NIL, {Variant::VECTOR2, Variant::NIL}},
    {"CanvasItem.visible.get", "CanvasItem", "property_get", "is_visible", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::BOOL, {Variant::NIL, Variant::NIL}},
    {"CanvasItem.visible.set", "CanvasItem", "property_set", "set_visible", P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 1, Variant::NIL, {Variant::BOOL, Variant::NIL}},
    {"Input.get_axis", "Input", "method", "get_axis", P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 2, Variant::FLOAT, {Variant::STRING_NAME, Variant::STRING_NAME}},
    {"CharacterBody2D.move_and_slide", "CharacterBody2D", "method", "move_and_slide", P_PHYSICS, 0, Variant::BOOL, {Variant::NIL, Variant::NIL}},
    {"CharacterBody2D.is_on_floor", "CharacterBody2D", "method", "is_on_floor", P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::BOOL, {Variant::NIL, Variant::NIL}},
    {"Variant.Type.TYPE_FLOAT", "Variant", "constant", "", P_MODULE | P_CONSTRUCT | P_ENTER_TREE | P_READY | P_PROCESS | P_PHYSICS | P_SIGNAL | P_SAFE_POINT, 0, Variant::INT, {Variant::NIL, Variant::NIL}},
};
struct Class { const char *name; const char *base; int constructor; };
inline constexpr Class CLASSES[] = {
    {"Object", "", -1},
    {"Node", "Object", -1},
    {"CanvasItem", "Node", -1},
    {"Node2D", "CanvasItem", M_NODE2D_NEW},
    {"Sprite2D", "Node2D", M_SPRITE2D_NEW},
    {"CollisionObject2D", "Node2D", -1},
    {"PhysicsBody2D", "CollisionObject2D", -1},
    {"CharacterBody2D", "PhysicsBody2D", M_CHARACTERBODY2D_NEW},
};
static_assert(Variant::FLOAT == 3, "固定 Variant 枚举与引擎不符");
} // namespace MetonGameSupport
