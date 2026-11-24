#pragma once

// What the router, the inspector, the baker and the resources share: input slots, the compiled
// row and its trigger state, shaping, labels, the baked format. Router state is private to
// input_router.cpp.

#include "gamepad_resources.h"

namespace InputRouterInternal {

// Caps the router enforces and the checker warns about.
constexpr uint32_t MAX_PATH_DEPTH = 16;
constexpr uint32_t MAX_CONTEXTS = 64; // Bit per context in a u64.
constexpr uint32_t MAX_SCHEMES = 8; // Bit per scheme in a u8.
constexpr uint32_t MAX_ROWS = 1024; // u16 row ids.
constexpr uint32_t MAX_BAKED = 1024;
constexpr uint32_t MAX_LUTS = 256;
constexpr uint32_t LUT_SIZE = 64; // Samples - 1.
constexpr uint32_t LUT_STRIDE = LUT_SIZE + 1;
constexpr uint16_t NO_ROW = 0xFFFF;
constexpr uint16_t NO_LUT = 0xFFFF;

// Slots: 21 buttons, 3 per axis (negative half, full, positive half), 2 sticks.
constexpr uint8_t BUTTON_COUNT = (uint8_t)JoyButton::SDL_MAX;
constexpr uint8_t AXIS_COUNT = (uint8_t)JoyAxis::SDL_MAX;
constexpr uint8_t AXIS_BASE = BUTTON_COUNT;
constexpr uint8_t STICK_BASE = AXIS_BASE + 3 * AXIS_COUNT;
constexpr uint8_t INPUT_COUNT = STICK_BASE + 2;
constexpr uint8_t NONE = 0xFF;
static_assert(INPUT_COUNT <= 64, "Slot sets are u64 masks.");

_FORCE_INLINE_ uint8_t button_slot(JoyButton p_button) { return (uint32_t)p_button < BUTTON_COUNT ? (uint8_t)p_button : NONE; }
_FORCE_INLINE_ uint8_t axis_slot(JoyAxis p_axis, int p_direction) { return (uint32_t)p_axis < AXIS_COUNT ? AXIS_BASE + 3 * (uint8_t)p_axis + (p_direction < 0 ? 0 : (p_direction == 0 ? 1 : 2)) : NONE; }
_FORCE_INLINE_ uint8_t stick_slot(int p_stick) { return p_stick == 0 || p_stick == 1 ? STICK_BASE + p_stick : NONE; }
_FORCE_INLINE_ uint64_t slot_bit(uint8_t p_slot) { return uint64_t(1) << p_slot; }
_FORCE_INLINE_ bool is_button_slot(uint8_t p_slot) { return p_slot < BUTTON_COUNT; }
_FORCE_INLINE_ bool is_axis_slot(uint8_t p_slot) { return p_slot >= AXIS_BASE && p_slot < STICK_BASE; }
_FORCE_INLINE_ bool is_stick_slot(uint8_t p_slot) { return p_slot >= STICK_BASE && p_slot < INPUT_COUNT; }
_FORCE_INLINE_ JoyAxis slot_axis(uint8_t p_slot) { return JoyAxis((p_slot - AXIS_BASE) / 3); }
_FORCE_INLINE_ int slot_direction(uint8_t p_slot) { return (int)((p_slot - AXIS_BASE) % 3) - 1; }
_FORCE_INLINE_ int stick_of_axis(JoyAxis p_axis) { return p_axis <= JoyAxis::LEFT_Y ? 0 : (p_axis <= JoyAxis::RIGHT_Y ? 1 : -1); }

enum RowFlags : uint8_t {
	ROW_POINTER = 1 << 0,
	ROW_DEADZONE_AXIAL = 1 << 1,
	ROW_DEADZONE_HYBRID = 1 << 2,
	ROW_LOCK_AXIS = 1 << 3,
};

// One attached binding, one cache line. Bytes 0-15 hold everything a button press reads.
// Baked maps store raw rows: a layout change bumps Baked::FORMAT_VERSION.
struct RouteBinding {
	uint8_t input = NONE; // NONE: tombstone.
	uint8_t gate = NONE; // Modifier button.
	uint8_t context = NONE;
	int8_t priority = 0;
	uint8_t trigger = RouteBindingResource::TRIGGER_PRESS;
	uint8_t flags = 0; // RowFlags.
	uint8_t scheme_mask = 0xFF;
	uint8_t reserved0 = 0;
	float press = 0.5f;
	float release = 0.4f;
	uint32_t hold_usec = 400000;
	uint32_t tap_usec = 250000;
	uint32_t repeat_delay_usec = 400000;
	uint32_t repeat_interval_usec = 80000;
	float deadzone = 0.2f;
	float saturation = 0.95f;
	float anti_deadzone = 0.0f;
	float pointer_speed = 900.0f;
	uint16_t lut = NO_LUT; // Router LUT pool index, in LUT_STRIDE units.
	uint8_t sequence[4] = { NONE, NONE, NONE, NONE };
	uint8_t authored_input = NONE; // Restored when an override clears.
	uint8_t authored_gate = NONE;
	uint8_t reserved[8] = {};

	bool same_tuning(const RouteBinding &p_other) const; // Ignores context, scheme_mask and lut.
};
static_assert(sizeof(RouteBinding) == 64, "One cache line per row.");
static_assert(offsetof(RouteBinding, hold_usec) == 16, "A press reads bytes 0-15.");

enum ClaimBits : uint8_t {
	CLAIM_DOWN = 1 << 0,
	CLAIM_HOLD_FIRED = 1 << 1,
	CLAIM_ACTION_DOWN = 1 << 2,
	CLAIM_HELD_BACK = 1 << 3, // Waiting on open chords.
	CLAIM_GATE_DEFERRED = 1 << 4, // A gate's own row: resolves at release.
	CLAIM_GATE_USED = 1 << 5, // A chord used the gate: its own row never fires.
};

// The row holding one slot, press to release, and its trigger state. Two per line.
struct Claim {
	uint16_t row = NO_ROW;
	uint8_t bits = 0; // ClaimBits.
	uint8_t reserved = 0;
	uint32_t deferred = 0; // Fire bits held while HELD_BACK or GATE_DEFERRED.
	float value = 0.0f; // Last shaped value.
	uint64_t down_usec = 0;
	uint64_t next_usec = 0; // Next repeat.
};
static_assert(sizeof(Claim) == 32, "Two claims per line.");

enum Fire : uint32_t {
	FIRE_NONE = 0,
	FIRE_PRESSED = 1 << 0,
	FIRE_HELD = 1 << 1,
	FIRE_TAPPED = 1 << 2,
	FIRE_REPEATED = 1 << 3,
	FIRE_CHANGED = 1 << 4,
	FIRE_RELEASED = 1 << 5,
	FIRE_SEQUENCE_ABANDONED = 1 << 6, // Traced only.
};

float shape_magnitude(const RouteBinding &p_row, const float *p_lut, float p_magnitude);
float shape_axis(const RouteBinding &p_row, const float *p_lut, float p_raw, int p_direction);
Vector2 shape_stick(const RouteBinding &p_row, const float *p_lut, const Vector2 &p_raw);
uint32_t trigger_step(const RouteBinding &p_row, Claim &r_claim, float p_shaped, uint64_t p_now_usec);
uint32_t sequence_step(const RouteBinding &p_row, uint8_t &r_step, uint64_t &r_until, uint8_t p_pressed_slot, uint64_t p_now_usec, uint64_t p_timeout_usec);
bool compile_binding(const RouteBindingResource &p_binding, RouteBinding &r_row, float *r_lut, bool &r_has_lut, String &r_error);
String button_label(JoyButton p_button, int p_family);
String axis_label(JoyAxis p_axis, int p_direction, int p_family);
String input_label(uint8_t p_slot, int p_family);
const char *trigger_name(int p_trigger);

// The route map: GamepadBakedMaps.data, packed arrays under these keys. Templates are unique per
// (id, scheme): raw RouteBinding rows with context NONE, lut indexing KEY_LUTS. The baker and the
// router's merge both build it with add_binding().
namespace Baked {
constexpr int FORMAT_VERSION = 2;
inline constexpr const char *KEY_FORMAT = "format";
inline constexpr const char *KEY_CAPS = "caps";
inline constexpr const char *KEY_SCHEMES = "schemes";
inline constexpr const char *KEY_CONTEXT_IDS = "context_ids";
inline constexpr const char *KEY_CONTEXT_INFO = "context_info"; // fallback, modal per context.
inline constexpr const char *KEY_IDS = "ids";
inline constexpr const char *KEY_ACTIONS = "actions";
inline constexpr const char *KEY_LABELS = "labels";
inline constexpr const char *KEY_ROWS = "rows"; // 64 B per template.
inline constexpr const char *KEY_LUTS = "luts";

enum Added {
	ADD_FAILED,
	ADD_NEW,
	ADD_SAME,
	ADD_REPLACED, // Same (id, scheme), different content.
};

PackedInt32Array caps();
Dictionary empty();
String validate(const Dictionary &p_data); // Empty when the router can load it.
bool add_context(Dictionary &r_data, const StringName &p_id, int p_fallback, bool p_modal);
Added add_binding(Dictionary &r_data, const RouteBindingResource &p_binding, String &r_error);
} // namespace Baked

#ifdef TOOLS_ENABLED
// input_router_tools.cpp
Ref<GamepadBakedMaps> bake(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const TypedArray<RouteBindingResource> &p_bindings, PackedStringArray &r_errors);
TypedArray<Dictionary> check_maps(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const Array &p_placements, const TypedArray<PackedStringArray> &p_paths);
#endif

} // namespace InputRouterInternal
