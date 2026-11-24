#include "gamepad_resources.h"

#include "input_router.h"
#include "input_router_internal.hpp"

using namespace InputRouterInternal;

static const char *BUTTON_HINT = "A (South),B (East),X (West),Y (North),Back,Guide,Start,Left Stick,Right Stick,Left Shoulder,Right Shoulder,D-pad Up,D-pad Down,D-pad Left,D-pad Right,Misc,Paddle 1,Paddle 2,Paddle 3,Paddle 4,Touchpad";
static const char *AXIS_HINT = "Left X,Left Y,Right X,Right Y,Left Trigger,Right Trigger";

// --- GamepadContext ---

void GamepadContext::set_id(const StringName &p_id) {
	id = p_id;
	emit_changed();
}

void GamepadContext::set_fallback(Fallback p_fallback) {
	fallback = p_fallback;
	emit_changed();
}

void GamepadContext::set_modal(bool p_modal) {
	modal = p_modal;
	emit_changed();
}

void GamepadContext::set_label(const String &p_label) {
	label = p_label;
	emit_changed();
}

// --- RouteBindingResource ---

// "LB + RT (analog)", "X > Y > A (sequence)". For logs and tools.
String RouteBindingResource::describe() const {
	String input;
	switch (source) {
		case SOURCE_BUTTON:
			input = button_label(button, InputRouter::PAD_FAMILY_GENERIC);
			break;
		case SOURCE_AXIS:
			input = axis_label(axis, axis_direction, InputRouter::PAD_FAMILY_GENERIC);
			break;
		case SOURCE_STICK:
			input = input_label(stick_slot(stick), InputRouter::PAD_FAMILY_GENERIC);
			break;
	}
	if (trigger == TRIGGER_SEQUENCE) {
		for (int i = 0; i < sequence.size(); i++) {
			input += " > " + button_label(JoyButton(sequence[i]), InputRouter::PAD_FAMILY_GENERIC);
		}
	}
	if (gate != JoyButton::INVALID) {
		input = button_label(gate, InputRouter::PAD_FAMILY_GENERIC) + " + " + input;
	}
	return vformat("%s (%s)", input, trigger_name(trigger));
}

// A preset as an editable Curve: five points with matching tangents.
Ref<Curve> RouteBindingResource::make_response_curve(ResponsePreset p_preset) {
	Ref<Curve> curve;
	curve.instantiate();
	for (int i = 0; i <= 4; i++) {
		const float t = i / 4.0f;
		float y = t;
		float slope = 1.0f;
		switch (p_preset) {
			case RESPONSE_LINEAR:
				break;
			case RESPONSE_SMOOTH:
				y = Math::pow(t, 1.6f);
				slope = 1.6f * Math::pow(t, 0.6f);
				break;
			case RESPONSE_PRECISE:
				y = Math::pow(t, 2.5f);
				slope = 2.5f * Math::pow(t, 1.5f);
				break;
			case RESPONSE_AGGRESSIVE:
				y = 1.0f - (1.0f - t) * (1.0f - t);
				slope = 2.0f * (1.0f - t);
				break;
		}
		curve->add_point(Vector2(t, y), slope, slope);
	}
	return curve;
}

// Hides what the source and trigger do not use. Hidden values are still stored.
void RouteBindingResource::_validate_property(PropertyInfo &p_property) const {
	const String &prop = p_property.name;
	bool shown = true;
	if (prop == "button") {
		shown = source == SOURCE_BUTTON;
	} else if (prop == "axis" || prop == "axis_direction" || prop == "lock_axis") {
		shown = source == SOURCE_AXIS;
	} else if (prop == "stick" || prop == "deadzone_shape" || prop == "pointer") {
		shown = source == SOURCE_STICK;
	} else if (prop == "pointer_speed") {
		shown = source == SOURCE_STICK && pointer;
	} else if (prop == "deadzone" || prop == "saturation" || prop == "anti_deadzone" || prop == "response" || prop == "press_threshold" || prop == "release_threshold") {
		shown = source != SOURCE_BUTTON;
	} else if (prop == "hold_time") {
		shown = trigger == TRIGGER_HOLD;
	} else if (prop == "tap_time") {
		shown = trigger == TRIGGER_TAP;
	} else if (prop == "repeat_delay" || prop == "repeat_interval") {
		shown = trigger == TRIGGER_REPEAT;
	} else if (prop == "sequence") {
		shown = trigger == TRIGGER_SEQUENCE;
	}
	if (!shown) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void RouteBindingResource::set_id(const StringName &p_id) {
	id = p_id;
	emit_changed();
}

void RouteBindingResource::set_context(const Ref<GamepadContext> &p_context) {
	context = p_context;
	emit_changed();
}

void RouteBindingResource::set_scheme(const StringName &p_scheme) {
	scheme = p_scheme;
	emit_changed();
}

void RouteBindingResource::set_source(Source p_source) {
	source = p_source;
	notify_property_list_changed();
	emit_changed();
}

void RouteBindingResource::set_button(JoyButton p_button) {
	button = p_button;
	emit_changed();
}

void RouteBindingResource::set_axis(JoyAxis p_axis) {
	axis = p_axis;
	emit_changed();
}

void RouteBindingResource::set_axis_direction(int p_direction) {
	axis_direction = CLAMP(p_direction, -1, 1);
	emit_changed();
}

void RouteBindingResource::set_stick(Stick p_stick) {
	stick = p_stick;
	emit_changed();
}

void RouteBindingResource::set_gate(JoyButton p_gate) {
	gate = p_gate;
	emit_changed();
}

void RouteBindingResource::set_trigger(Trigger p_trigger) {
	trigger = p_trigger;
	notify_property_list_changed();
	emit_changed();
}

void RouteBindingResource::set_press_threshold(float p_threshold) {
	press_threshold = p_threshold;
	emit_changed();
}

void RouteBindingResource::set_release_threshold(float p_threshold) {
	release_threshold = p_threshold;
	emit_changed();
}

void RouteBindingResource::set_hold_time(float p_seconds) {
	hold_time = p_seconds;
	emit_changed();
}

void RouteBindingResource::set_tap_time(float p_seconds) {
	tap_time = p_seconds;
	emit_changed();
}

void RouteBindingResource::set_repeat_delay(float p_seconds) {
	repeat_delay = p_seconds;
	emit_changed();
}

void RouteBindingResource::set_repeat_interval(float p_seconds) {
	repeat_interval = p_seconds;
	emit_changed();
}

void RouteBindingResource::set_sequence(const PackedInt32Array &p_buttons) {
	sequence = p_buttons;
	emit_changed();
}

void RouteBindingResource::set_deadzone(float p_deadzone) {
	deadzone = p_deadzone;
	emit_changed();
}

void RouteBindingResource::set_saturation(float p_saturation) {
	saturation = p_saturation;
	emit_changed();
}

void RouteBindingResource::set_anti_deadzone(float p_anti_deadzone) {
	anti_deadzone = p_anti_deadzone;
	emit_changed();
}

void RouteBindingResource::set_deadzone_shape(DeadzoneShape p_shape) {
	deadzone_shape = p_shape;
	emit_changed();
}

void RouteBindingResource::set_lock_axis(bool p_lock) {
	lock_axis = p_lock;
	emit_changed();
}

// Forwards the curve's changed signal, so editing points recompiles live.
void RouteBindingResource::set_response(const Ref<Curve> &p_response) {
	if (response.is_valid()) {
		response->disconnect_changed(callable_mp((Resource *)this, &Resource::emit_changed));
	}
	response = p_response;
	if (response.is_valid()) {
		response->connect_changed(callable_mp((Resource *)this, &Resource::emit_changed));
	}
	emit_changed();
}

void RouteBindingResource::set_action(const StringName &p_action) {
	action = p_action;
	emit_changed();
}

void RouteBindingResource::set_pointer(bool p_pointer) {
	pointer = p_pointer;
	notify_property_list_changed();
	emit_changed();
}

void RouteBindingResource::set_pointer_speed(float p_speed) {
	pointer_speed = p_speed;
	emit_changed();
}

void RouteBindingResource::set_priority(int p_priority) {
	priority = CLAMP(p_priority, -128, 127);
	emit_changed();
}

void RouteBindingResource::set_label(const String &p_label) {
	label = p_label;
	emit_changed();
}

// --- GamepadMap ---

void GamepadMap::set_context(const Ref<GamepadContext> &p_context) {
	context = p_context;
	emit_changed();
}

void GamepadMap::set_bindings(const TypedArray<RouteBindingResource> &p_bindings) {
	bindings = p_bindings;
	emit_changed();
}

// --- GamepadGlyphProvider ---

// p_family: InputRouter::PadFamily. p_input: input slot.
Ref<Texture2D> GamepadGlyphProvider::get_glyph(int p_family, int p_input) const {
	Ref<Texture2D> glyph;
	GDVIRTUAL_CALL(_get_glyph, p_family, p_input, glyph);
	return glyph;
}

// --- Bindings ---

void GamepadContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_id", "id"), &GamepadContext::set_id);
	ClassDB::bind_method(D_METHOD("get_id"), &GamepadContext::get_id);
	ClassDB::bind_method(D_METHOD("set_fallback", "fallback"), &GamepadContext::set_fallback);
	ClassDB::bind_method(D_METHOD("get_fallback"), &GamepadContext::get_fallback);
	ClassDB::bind_method(D_METHOD("set_modal", "modal"), &GamepadContext::set_modal);
	ClassDB::bind_method(D_METHOD("is_modal"), &GamepadContext::is_modal);
	ClassDB::bind_method(D_METHOD("set_label", "label"), &GamepadContext::set_label);
	ClassDB::bind_method(D_METHOD("get_label"), &GamepadContext::get_label);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fallback", PROPERTY_HINT_ENUM, "Inherit,Swallow,Pass"), "set_fallback", "get_fallback");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "modal"), "set_modal", "is_modal");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "label"), "set_label", "get_label");

	BIND_ENUM_CONSTANT(FALLBACK_INHERIT);
	BIND_ENUM_CONSTANT(FALLBACK_SWALLOW);
	BIND_ENUM_CONSTANT(FALLBACK_PASS);
}

void RouteBindingResource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("describe"), &RouteBindingResource::describe);
	ClassDB::bind_static_method("RouteBindingResource", D_METHOD("make_response_curve", "preset"), &RouteBindingResource::make_response_curve);

	ClassDB::bind_method(D_METHOD("set_id", "id"), &RouteBindingResource::set_id);
	ClassDB::bind_method(D_METHOD("get_id"), &RouteBindingResource::get_id);
	ClassDB::bind_method(D_METHOD("set_context", "context"), &RouteBindingResource::set_context);
	ClassDB::bind_method(D_METHOD("get_context"), &RouteBindingResource::get_context);
	ClassDB::bind_method(D_METHOD("set_scheme", "scheme"), &RouteBindingResource::set_scheme);
	ClassDB::bind_method(D_METHOD("get_scheme"), &RouteBindingResource::get_scheme);
	ClassDB::bind_method(D_METHOD("set_source", "source"), &RouteBindingResource::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &RouteBindingResource::get_source);
	ClassDB::bind_method(D_METHOD("set_button", "button"), &RouteBindingResource::set_button);
	ClassDB::bind_method(D_METHOD("get_button"), &RouteBindingResource::get_button);
	ClassDB::bind_method(D_METHOD("set_axis", "axis"), &RouteBindingResource::set_axis);
	ClassDB::bind_method(D_METHOD("get_axis"), &RouteBindingResource::get_axis);
	ClassDB::bind_method(D_METHOD("set_axis_direction", "direction"), &RouteBindingResource::set_axis_direction);
	ClassDB::bind_method(D_METHOD("get_axis_direction"), &RouteBindingResource::get_axis_direction);
	ClassDB::bind_method(D_METHOD("set_stick", "stick"), &RouteBindingResource::set_stick);
	ClassDB::bind_method(D_METHOD("get_stick"), &RouteBindingResource::get_stick);
	ClassDB::bind_method(D_METHOD("set_gate", "gate"), &RouteBindingResource::set_gate);
	ClassDB::bind_method(D_METHOD("get_gate"), &RouteBindingResource::get_gate);
	ClassDB::bind_method(D_METHOD("set_trigger", "trigger"), &RouteBindingResource::set_trigger);
	ClassDB::bind_method(D_METHOD("get_trigger"), &RouteBindingResource::get_trigger);
	ClassDB::bind_method(D_METHOD("set_press_threshold", "threshold"), &RouteBindingResource::set_press_threshold);
	ClassDB::bind_method(D_METHOD("get_press_threshold"), &RouteBindingResource::get_press_threshold);
	ClassDB::bind_method(D_METHOD("set_release_threshold", "threshold"), &RouteBindingResource::set_release_threshold);
	ClassDB::bind_method(D_METHOD("get_release_threshold"), &RouteBindingResource::get_release_threshold);
	ClassDB::bind_method(D_METHOD("set_hold_time", "seconds"), &RouteBindingResource::set_hold_time);
	ClassDB::bind_method(D_METHOD("get_hold_time"), &RouteBindingResource::get_hold_time);
	ClassDB::bind_method(D_METHOD("set_tap_time", "seconds"), &RouteBindingResource::set_tap_time);
	ClassDB::bind_method(D_METHOD("get_tap_time"), &RouteBindingResource::get_tap_time);
	ClassDB::bind_method(D_METHOD("set_repeat_delay", "seconds"), &RouteBindingResource::set_repeat_delay);
	ClassDB::bind_method(D_METHOD("get_repeat_delay"), &RouteBindingResource::get_repeat_delay);
	ClassDB::bind_method(D_METHOD("set_repeat_interval", "seconds"), &RouteBindingResource::set_repeat_interval);
	ClassDB::bind_method(D_METHOD("get_repeat_interval"), &RouteBindingResource::get_repeat_interval);
	ClassDB::bind_method(D_METHOD("set_sequence", "buttons"), &RouteBindingResource::set_sequence);
	ClassDB::bind_method(D_METHOD("get_sequence"), &RouteBindingResource::get_sequence);
	ClassDB::bind_method(D_METHOD("set_deadzone", "deadzone"), &RouteBindingResource::set_deadzone);
	ClassDB::bind_method(D_METHOD("get_deadzone"), &RouteBindingResource::get_deadzone);
	ClassDB::bind_method(D_METHOD("set_saturation", "saturation"), &RouteBindingResource::set_saturation);
	ClassDB::bind_method(D_METHOD("get_saturation"), &RouteBindingResource::get_saturation);
	ClassDB::bind_method(D_METHOD("set_anti_deadzone", "anti_deadzone"), &RouteBindingResource::set_anti_deadzone);
	ClassDB::bind_method(D_METHOD("get_anti_deadzone"), &RouteBindingResource::get_anti_deadzone);
	ClassDB::bind_method(D_METHOD("set_deadzone_shape", "shape"), &RouteBindingResource::set_deadzone_shape);
	ClassDB::bind_method(D_METHOD("get_deadzone_shape"), &RouteBindingResource::get_deadzone_shape);
	ClassDB::bind_method(D_METHOD("set_lock_axis", "lock"), &RouteBindingResource::set_lock_axis);
	ClassDB::bind_method(D_METHOD("is_lock_axis"), &RouteBindingResource::is_lock_axis);
	ClassDB::bind_method(D_METHOD("set_response", "response"), &RouteBindingResource::set_response);
	ClassDB::bind_method(D_METHOD("get_response"), &RouteBindingResource::get_response);
	ClassDB::bind_method(D_METHOD("set_action", "action"), &RouteBindingResource::set_action);
	ClassDB::bind_method(D_METHOD("get_action"), &RouteBindingResource::get_action);
	ClassDB::bind_method(D_METHOD("set_pointer", "pointer"), &RouteBindingResource::set_pointer);
	ClassDB::bind_method(D_METHOD("is_pointer"), &RouteBindingResource::is_pointer);
	ClassDB::bind_method(D_METHOD("set_pointer_speed", "speed"), &RouteBindingResource::set_pointer_speed);
	ClassDB::bind_method(D_METHOD("get_pointer_speed"), &RouteBindingResource::get_pointer_speed);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &RouteBindingResource::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &RouteBindingResource::get_priority);
	ClassDB::bind_method(D_METHOD("set_label", "label"), &RouteBindingResource::set_label);
	ClassDB::bind_method(D_METHOD("get_label"), &RouteBindingResource::get_label);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "GamepadContext"), "set_context", "get_context");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "scheme"), "set_scheme", "get_scheme");

	ADD_GROUP("Input", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source", PROPERTY_HINT_ENUM, "Button,Axis,Stick"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "button", PROPERTY_HINT_ENUM, BUTTON_HINT), "set_button", "get_button");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "axis", PROPERTY_HINT_ENUM, AXIS_HINT), "set_axis", "get_axis");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "axis_direction", PROPERTY_HINT_ENUM, "Negative:-1,Full:0,Positive:1"), "set_axis_direction", "get_axis_direction");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stick", PROPERTY_HINT_ENUM, "Left,Right"), "set_stick", "get_stick");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gate", PROPERTY_HINT_ENUM, String("None:-1,") + BUTTON_HINT), "set_gate", "get_gate");

	ADD_GROUP("Trigger", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "trigger", PROPERTY_HINT_ENUM, "Press,Release,Hold,Tap,Repeat,Analog,Sequence"), "set_trigger", "get_trigger");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "press_threshold", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_press_threshold", "get_press_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "release_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_release_threshold", "get_release_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hold_time", PROPERTY_HINT_RANGE, "0.01,3,0.01,suffix:s"), "set_hold_time", "get_hold_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tap_time", PROPERTY_HINT_RANGE, "0.01,1,0.01,suffix:s"), "set_tap_time", "get_tap_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "repeat_delay", PROPERTY_HINT_RANGE, "0.01,2,0.01,suffix:s"), "set_repeat_delay", "get_repeat_delay");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "repeat_interval", PROPERTY_HINT_RANGE, "0.01,1,0.01,suffix:s"), "set_repeat_interval", "get_repeat_interval");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "sequence", PROPERTY_HINT_TYPE_STRING, vformat("%d/%d:%s", Variant::INT, PROPERTY_HINT_ENUM, BUTTON_HINT)), "set_sequence", "get_sequence");

	ADD_GROUP("Response", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deadzone", PROPERTY_HINT_RANGE, "0,0.95,0.01"), "set_deadzone", "get_deadzone");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "saturation", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_saturation", "get_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "anti_deadzone", PROPERTY_HINT_RANGE, "0,0.95,0.01"), "set_anti_deadzone", "get_anti_deadzone");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deadzone_shape", PROPERTY_HINT_ENUM, "Radial,Axial,Hybrid"), "set_deadzone_shape", "get_deadzone_shape");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lock_axis"), "set_lock_axis", "is_lock_axis");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "response", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_response", "get_response");

	ADD_GROUP("Output", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "action", PROPERTY_HINT_INPUT_NAME, "show_builtin,loose_mode"), "set_action", "get_action");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "pointer"), "set_pointer", "is_pointer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pointer_speed", PROPERTY_HINT_RANGE, "0,4000,10,suffix:px/s"), "set_pointer_speed", "get_pointer_speed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-128,127,1"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "label"), "set_label", "get_label");

	BIND_ENUM_CONSTANT(SOURCE_BUTTON);
	BIND_ENUM_CONSTANT(SOURCE_AXIS);
	BIND_ENUM_CONSTANT(SOURCE_STICK);
	BIND_ENUM_CONSTANT(STICK_LEFT);
	BIND_ENUM_CONSTANT(STICK_RIGHT);
	BIND_ENUM_CONSTANT(TRIGGER_PRESS);
	BIND_ENUM_CONSTANT(TRIGGER_RELEASE);
	BIND_ENUM_CONSTANT(TRIGGER_HOLD);
	BIND_ENUM_CONSTANT(TRIGGER_TAP);
	BIND_ENUM_CONSTANT(TRIGGER_REPEAT);
	BIND_ENUM_CONSTANT(TRIGGER_ANALOG);
	BIND_ENUM_CONSTANT(TRIGGER_SEQUENCE);
	BIND_ENUM_CONSTANT(DEADZONE_RADIAL);
	BIND_ENUM_CONSTANT(DEADZONE_AXIAL);
	BIND_ENUM_CONSTANT(DEADZONE_HYBRID);
	BIND_ENUM_CONSTANT(RESPONSE_LINEAR);
	BIND_ENUM_CONSTANT(RESPONSE_SMOOTH);
	BIND_ENUM_CONSTANT(RESPONSE_PRECISE);
	BIND_ENUM_CONSTANT(RESPONSE_AGGRESSIVE);
}

void GamepadMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_context", "context"), &GamepadMap::set_context);
	ClassDB::bind_method(D_METHOD("get_context"), &GamepadMap::get_context);
	ClassDB::bind_method(D_METHOD("set_bindings", "bindings"), &GamepadMap::set_bindings);
	ClassDB::bind_method(D_METHOD("get_bindings"), &GamepadMap::get_bindings);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "GamepadContext"), "set_context", "get_context");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bindings", PROPERTY_HINT_ARRAY_TYPE, "RouteBindingResource"), "set_bindings", "get_bindings");
}

void GamepadBakedMaps::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &GamepadBakedMaps::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &GamepadBakedMaps::get_data);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
}

void GamepadGlyphProvider::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_glyph", "family", "input"), &GamepadGlyphProvider::get_glyph);

	GDVIRTUAL_BIND(_get_glyph, "family", "input");
}
