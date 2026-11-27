#pragma once

// Generic gamepad router, installed as Input's frame filter. Once per frame, before physics, it
// takes the frame's input, picks the device that owns the frame, routes pad input through the
// gamepad context path, flips the read side, then runs callbacks and signals. Getters read the
// newest sealed frame. [any]: safe from any thread, one atomic load, no lock or copy. Everything
// else is main thread, signal connections included; bound mutators called off it are deferred,
// bound getters return empty.

#include "gamepad_resources.h"

#include "core/input/input.h"

namespace InputRouterInternal {
struct State;
}

class InputRouter : public Object, public Input::FrameFilter {
	GDCLASS(InputRouter, Object);

	static inline InputRouter *singleton = nullptr;

public:
	enum Device {
		DEVICE_NONE,
		DEVICE_KBM,
		DEVICE_PAD,
	};

	enum DevicePolicy {
		DEVICE_POLICY_AUTO,
		DEVICE_POLICY_PAD,
		DEVICE_POLICY_KBM,
	};

	enum MouseInPad {
		MOUSE_IN_PAD_DROP,
		MOUSE_IN_PAD_POINTER,
	};

	enum TouchMode {
		TOUCH_AS_POINTER,
		TOUCH_AS_PAD,
		TOUCH_IGNORE,
	};

	enum PadFamily {
		PAD_FAMILY_GENERIC,
		PAD_FAMILY_XBOX,
		PAD_FAMILY_PLAYSTATION,
		PAD_FAMILY_NINTENDO,
		PAD_FAMILY_STEAM,
		PAD_FAMILY_MAX,
	};

	enum Phase {
		PHASE_PRESSED,
		PHASE_RELEASED,
		PHASE_CANCELED,
		PHASE_HELD,
		PHASE_TAPPED,
		PHASE_REPEATED,
		PHASE_CHANGED,
	};

	static InputRouter *get_singleton() { return singleton; }

	void set_enabled(bool p_enabled); // Lands at the next frame, like every mutator.
	bool is_enabled() const; // [any]

	// Device.
	Device get_device() const; // [any]
	int get_active_pad() const; // [any]
	bool is_switch_frame() const; // [any]
	void set_device(Device p_device, int p_pad = -1);
	void set_device_policy(DevicePolicy p_policy);
	DevicePolicy get_device_policy() const; // [any]
	void set_mouse_in_pad(MouseInPad p_mode);
	MouseInPad get_mouse_in_pad() const;
	void set_trigger_wake_threshold(float p_threshold);
	float get_trigger_wake_threshold() const;

	// Pad identity and prompts.
	PadFamily get_pad_family(int p_pad = -1) const; // [any]
	String get_pad_name(int p_pad = -1) const;
	static PadFamily detect_pad_family(int p_pad); // From Input: router off and editor too.
	static String get_input_label(int p_input, PadFamily p_family);
	Ref<GlyphFontMap> get_glyph_map() const;
	String get_input_glyph(int p_input, PadFamily p_family) const;

	// Captured pad state. p_pad -1: active pad.
	bool is_pad_button_down(JoyButton p_button, int p_pad = -1) const; // [any]
	bool was_pad_button_pressed(JoyButton p_button, int p_pad = -1) const; // [any]
	float get_pad_axis(JoyAxis p_axis, int p_pad = -1) const; // [any]
	Vector2 get_pad_stick(RouteBindingResource::Stick p_stick, float p_deadzone = 0.2f, int p_pad = -1) const; // [any]
	uint64_t get_frame() const; // [any]

	// Gamepad context.
	void register_gamepad_context(const Ref<GamepadContext> &p_context);
	void set_gamepad_base_contexts(const PackedStringArray &p_contexts);
	int push_gamepad_context(const StringName &p_context);
	void pop_gamepad_context(int p_handle);
	StringName get_gamepad_context() const;
	PackedStringArray get_gamepad_context_path() const;
	bool is_gamepad_context_active(const StringName &p_context) const;

	// Bindings. Handles are never 0 and never reused.
	int attach(const Ref<RouteBindingResource> &p_binding, const StringName &p_context = StringName(), const Callable &p_target = Callable());
	void detach(int p_handle);
	int load_map(const Ref<GamepadMap> &p_map);
	void unload_map(int p_handle);
	int set_callback(const StringName &p_context, const StringName &p_binding_id, const Callable &p_target);
	void clear_callback(int p_handle);
	void clear();

	// Schemes, player rebinds, player timing.
	void set_scheme(const StringName &p_scheme);
	StringName get_scheme() const;
	bool set_override(const StringName &p_binding_id, int p_input, int p_gate = -1);
	void clear_override(const StringName &p_binding_id);
	void clear_overrides();
	void set_sequence_timeout(float p_seconds);
	float get_sequence_timeout() const;
	void set_chord_window(float p_seconds);
	float get_chord_window() const;

	// Route map: the compiled templates every row comes from. The next frame rebuilds clean.
	void set_route_map(const Ref<GamepadBakedMaps> &p_map);

	// Touch and virtual pointer.
	void set_touch_mode(TouchMode p_mode);
	TouchMode get_touch_mode() const;
	Vector2 take_pointer_delta();
	Vector2 get_pointer_velocity() const; // [any]

	// Analysis.
	void set_trace_enabled(bool p_enabled, int p_capacity = 4096);
	bool is_trace_enabled() const;

	// Input::FrameFilter.
	void input_poll_process_flip() override;
	void pressed_events_released() override;
	void joy_connection_changed(int p_pad, bool p_connected) override;

	InputRouter();
	~InputRouter();

protected:
	static void _bind_methods();

private:
	InputRouterInternal::State *s = nullptr;

	// Like Node: signals emit in _emit_signals() and connect on the main thread, so no signal mutex.
	bool _uses_signal_mutex() const override { return false; }

	// input_poll_process_flip(), in order.
	void _poll();
	void _process();
	void _flip();
	void _callback();
	void _emit_signals();

	void _defer(const Callable &p_call);
	void _auto_enable();
	bool _attach_handle(const Ref<RouteBindingResource> &p_binding, const StringName &p_context, const Callable &p_target, uint32_t p_handle);
	void _push_handle(const StringName &p_context, uint32_t p_handle);
	void _load_map_handle(const Ref<GamepadMap> &p_map, uint32_t p_handle);
	void _set_callback_handle(const StringName &p_context, const StringName &p_binding_id, const Callable &p_target, uint32_t p_handle);
#ifdef TOOLS_ENABLED
	void _binding_changed(ObjectID p_binding);
#endif

	// Script only: Variant-shaped getters.
	Dictionary _get_pad_info(int p_pad) const;
	PackedInt32Array _get_connected_pads() const; // [any]
	TypedArray<Dictionary> _get_routes() const;
	PackedStringArray _get_schemes() const;
	Dictionary _get_overrides() const;
	TypedArray<Dictionary> _get_trace();

	// Script only (input_router_bind.cpp).
	static int _button_input(JoyButton p_button);
	static int _axis_input(JoyAxis p_axis, int p_direction);
	static int _stick_input(RouteBindingResource::Stick p_stick);
#ifdef TOOLS_ENABLED
	static TypedArray<Dictionary> _check_maps(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const Array &p_placements, const TypedArray<PackedStringArray> &p_paths);
#endif
};

VARIANT_ENUM_CAST(InputRouter::Device);
VARIANT_ENUM_CAST(InputRouter::DevicePolicy);
VARIANT_ENUM_CAST(InputRouter::MouseInPad);
VARIANT_ENUM_CAST(InputRouter::TouchMode);
VARIANT_ENUM_CAST(InputRouter::PadFamily);
VARIANT_ENUM_CAST(InputRouter::Phase);
