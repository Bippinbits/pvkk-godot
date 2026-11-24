#pragma once

// Gamepad authoring data, read when a binding is attached, compiled or baked; never per event.
// A RouteBindingResource compiles to one RouteBinding row. The first binding to claim an input
// owns it until release.

#include "core/input/input_enums.h"
#include "core/io/resource.h"
#include "core/object/gdvirtual.gen.inc"
#include "core/variant/typed_array.h"
#include "scene/resources/curve.h"
#include "scene/resources/texture.h"

class GamepadContext : public Resource {
	GDCLASS(GamepadContext, Resource);

public:
	enum Fallback {
		FALLBACK_INHERIT, // Ask the context below.
		FALLBACK_SWALLOW,
		FALLBACK_PASS, // Raw pad event to Godot: GUI focus, ui_*, InputMap.
	};

private:
	StringName id;
	Fallback fallback = FALLBACK_INHERIT;
	bool modal = false;
	String label;

protected:
	static void _bind_methods();

public:
	void set_id(const StringName &p_id);
	StringName get_id() const { return id; }
	void set_fallback(Fallback p_fallback);
	Fallback get_fallback() const { return fallback; }
	void set_modal(bool p_modal);
	bool is_modal() const { return modal; }
	void set_label(const String &p_label);
	String get_label() const { return label; }
};

class RouteBindingResource : public Resource {
	GDCLASS(RouteBindingResource, Resource);

public:
	enum Source {
		SOURCE_BUTTON,
		SOURCE_AXIS,
		SOURCE_STICK,
	};

	enum Stick {
		STICK_LEFT,
		STICK_RIGHT,
	};

	enum Trigger {
		TRIGGER_PRESS,
		TRIGGER_RELEASE,
		TRIGGER_HOLD,
		TRIGGER_TAP,
		TRIGGER_REPEAT,
		TRIGGER_ANALOG,
		TRIGGER_SEQUENCE,
	};

	enum DeadzoneShape {
		DEADZONE_RADIAL,
		DEADZONE_AXIAL,
		DEADZONE_HYBRID,
	};

	enum ResponsePreset {
		RESPONSE_LINEAR,
		RESPONSE_SMOOTH,
		RESPONSE_PRECISE,
		RESPONSE_AGGRESSIVE,
	};

private:
	StringName id;
	Ref<GamepadContext> context;
	StringName scheme;

	Source source = SOURCE_BUTTON;
	JoyButton button = JoyButton::A;
	JoyAxis axis = JoyAxis::TRIGGER_RIGHT;
	int axis_direction = 1;
	Stick stick = STICK_LEFT;
	JoyButton gate = JoyButton::INVALID;

	Trigger trigger = TRIGGER_PRESS;
	float press_threshold = 0.5f;
	float release_threshold = 0.4f;
	float hold_time = 0.4f;
	float tap_time = 0.25f;
	float repeat_delay = 0.4f;
	float repeat_interval = 0.08f;
	PackedInt32Array sequence;

	float deadzone = 0.2f;
	float saturation = 0.95f;
	float anti_deadzone = 0.0f;
	DeadzoneShape deadzone_shape = DEADZONE_RADIAL;
	bool lock_axis = false;
	Ref<Curve> response;

	StringName action;
	bool pointer = false;
	float pointer_speed = 900.0f;
	int priority = 0;
	String label;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	String describe() const;
	static Ref<Curve> make_response_curve(ResponsePreset p_preset);

	void set_id(const StringName &p_id);
	StringName get_id() const { return id; }
	void set_context(const Ref<GamepadContext> &p_context);
	Ref<GamepadContext> get_context() const { return context; }
	void set_scheme(const StringName &p_scheme);
	StringName get_scheme() const { return scheme; }

	void set_source(Source p_source);
	Source get_source() const { return source; }
	void set_button(JoyButton p_button);
	JoyButton get_button() const { return button; }
	void set_axis(JoyAxis p_axis);
	JoyAxis get_axis() const { return axis; }
	void set_axis_direction(int p_direction);
	int get_axis_direction() const { return axis_direction; }
	void set_stick(Stick p_stick);
	Stick get_stick() const { return stick; }
	void set_gate(JoyButton p_gate);
	JoyButton get_gate() const { return gate; }

	void set_trigger(Trigger p_trigger);
	Trigger get_trigger() const { return trigger; }
	void set_press_threshold(float p_threshold);
	float get_press_threshold() const { return press_threshold; }
	void set_release_threshold(float p_threshold);
	float get_release_threshold() const { return release_threshold; }
	void set_hold_time(float p_seconds);
	float get_hold_time() const { return hold_time; }
	void set_tap_time(float p_seconds);
	float get_tap_time() const { return tap_time; }
	void set_repeat_delay(float p_seconds);
	float get_repeat_delay() const { return repeat_delay; }
	void set_repeat_interval(float p_seconds);
	float get_repeat_interval() const { return repeat_interval; }
	void set_sequence(const PackedInt32Array &p_buttons);
	PackedInt32Array get_sequence() const { return sequence; }

	void set_deadzone(float p_deadzone);
	float get_deadzone() const { return deadzone; }
	void set_saturation(float p_saturation);
	float get_saturation() const { return saturation; }
	void set_anti_deadzone(float p_anti_deadzone);
	float get_anti_deadzone() const { return anti_deadzone; }
	void set_deadzone_shape(DeadzoneShape p_shape);
	DeadzoneShape get_deadzone_shape() const { return deadzone_shape; }
	void set_lock_axis(bool p_lock);
	bool is_lock_axis() const { return lock_axis; }
	void set_response(const Ref<Curve> &p_response);
	Ref<Curve> get_response() const { return response; }

	void set_action(const StringName &p_action);
	StringName get_action() const { return action; }
	void set_pointer(bool p_pointer);
	bool is_pointer() const { return pointer; }
	void set_pointer_speed(float p_speed);
	float get_pointer_speed() const { return pointer_speed; }
	void set_priority(int p_priority);
	int get_priority() const { return priority; }
	void set_label(const String &p_label);
	String get_label() const { return label; }
};

class GamepadMap : public Resource {
	GDCLASS(GamepadMap, Resource);

	Ref<GamepadContext> context;
	TypedArray<RouteBindingResource> bindings;

protected:
	static void _bind_methods();

public:
	void set_context(const Ref<GamepadContext> &p_context);
	Ref<GamepadContext> get_context() const { return context; }
	void set_bindings(const TypedArray<RouteBindingResource> &p_bindings);
	TypedArray<RouteBindingResource> get_bindings() const { return bindings; }
};

class GamepadBakedMaps : public Resource {
	GDCLASS(GamepadBakedMaps, Resource);

	Dictionary data; // Packed arrays; layout in input_router_internal.hpp.

protected:
	static void _bind_methods();

public:
	void set_data(const Dictionary &p_data) { data = p_data; }
	Dictionary get_data() const { return data; }
};

class GamepadGlyphProvider : public Resource {
	GDCLASS(GamepadGlyphProvider, Resource);

protected:
	static void _bind_methods();

	GDVIRTUAL2RC(Ref<Texture2D>, _get_glyph, int, int)

public:
	Ref<Texture2D> get_glyph(int p_family, int p_input) const;
};

VARIANT_ENUM_CAST(GamepadContext::Fallback);
VARIANT_ENUM_CAST(RouteBindingResource::Source);
VARIANT_ENUM_CAST(RouteBindingResource::Stick);
VARIANT_ENUM_CAST(RouteBindingResource::Trigger);
VARIANT_ENUM_CAST(RouteBindingResource::DeadzoneShape);
VARIANT_ENUM_CAST(RouteBindingResource::ResponsePreset);
