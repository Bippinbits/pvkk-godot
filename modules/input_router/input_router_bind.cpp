// InputRouter's bindings: slot helpers, the map checker and _bind_methods. The Variant-shaped
// getters read router state, so they live in input_router.cpp.

#include "input_router.h"

#include "input_router_internal.hpp"

using namespace InputRouterInternal;

int InputRouter::_button_input(JoyButton p_button) {
	const uint8_t slot = button_slot(p_button);
	return slot == NONE ? -1 : slot;
}

int InputRouter::_axis_input(JoyAxis p_axis, int p_direction) {
	const uint8_t slot = axis_slot(p_axis, p_direction);
	return slot == NONE ? -1 : slot;
}

int InputRouter::_stick_input(RouteBindingResource::Stick p_stick) {
	const uint8_t slot = stick_slot(p_stick);
	return slot == NONE ? -1 : slot;
}

#ifdef TOOLS_ENABLED
TypedArray<Dictionary> InputRouter::_check_maps(const TypedArray<GamepadContext> &p_contexts, const TypedArray<GamepadMap> &p_maps, const Array &p_placements, const TypedArray<PackedStringArray> &p_paths) {
	return check_maps(p_contexts, p_maps, p_placements, p_paths);
}
#endif

void InputRouter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &InputRouter::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &InputRouter::is_enabled);

	ClassDB::bind_method(D_METHOD("get_device"), &InputRouter::get_device);
	ClassDB::bind_method(D_METHOD("get_active_pad"), &InputRouter::get_active_pad);
	ClassDB::bind_method(D_METHOD("is_switch_frame"), &InputRouter::is_switch_frame);
	ClassDB::bind_method(D_METHOD("set_device", "device", "pad"), &InputRouter::set_device, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_device_policy", "policy"), &InputRouter::set_device_policy);
	ClassDB::bind_method(D_METHOD("get_device_policy"), &InputRouter::get_device_policy);
	ClassDB::bind_method(D_METHOD("set_mouse_in_pad", "mode"), &InputRouter::set_mouse_in_pad);
	ClassDB::bind_method(D_METHOD("get_mouse_in_pad"), &InputRouter::get_mouse_in_pad);
	ClassDB::bind_method(D_METHOD("set_trigger_wake_threshold", "threshold"), &InputRouter::set_trigger_wake_threshold);
	ClassDB::bind_method(D_METHOD("get_trigger_wake_threshold"), &InputRouter::get_trigger_wake_threshold);

	ClassDB::bind_method(D_METHOD("get_pad_family", "pad"), &InputRouter::get_pad_family, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_pad_name", "pad"), &InputRouter::get_pad_name, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_pad_info", "pad"), &InputRouter::_get_pad_info, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_connected_pads"), &InputRouter::_get_connected_pads);
	ClassDB::bind_static_method("InputRouter", D_METHOD("detect_pad_family", "pad"), &InputRouter::detect_pad_family);
	ClassDB::bind_static_method("InputRouter", D_METHOD("get_input_label", "input", "family"), &InputRouter::get_input_label);
	ClassDB::bind_method(D_METHOD("get_glyph_map"), &InputRouter::get_glyph_map);
	ClassDB::bind_method(D_METHOD("get_input_glyph", "input", "family"), &InputRouter::get_input_glyph);
	ClassDB::bind_static_method("InputRouter", D_METHOD("button_input", "button"), &InputRouter::_button_input);
	ClassDB::bind_static_method("InputRouter", D_METHOD("axis_input", "axis", "direction"), &InputRouter::_axis_input);
	ClassDB::bind_static_method("InputRouter", D_METHOD("stick_input", "stick"), &InputRouter::_stick_input);

	ClassDB::bind_method(D_METHOD("is_pad_button_down", "button", "pad"), &InputRouter::is_pad_button_down, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("was_pad_button_pressed", "button", "pad"), &InputRouter::was_pad_button_pressed, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_pad_axis", "axis", "pad"), &InputRouter::get_pad_axis, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_pad_stick", "stick", "deadzone", "pad"), &InputRouter::get_pad_stick, DEFVAL(0.2f), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_frame"), &InputRouter::get_frame);

	ClassDB::bind_method(D_METHOD("register_gamepad_context", "context"), &InputRouter::register_gamepad_context);
	ClassDB::bind_method(D_METHOD("set_gamepad_base_contexts", "contexts"), &InputRouter::set_gamepad_base_contexts);
	ClassDB::bind_method(D_METHOD("push_gamepad_context", "context"), &InputRouter::push_gamepad_context);
	ClassDB::bind_method(D_METHOD("pop_gamepad_context", "handle"), &InputRouter::pop_gamepad_context);
	ClassDB::bind_method(D_METHOD("get_gamepad_context"), &InputRouter::get_gamepad_context);
	ClassDB::bind_method(D_METHOD("get_gamepad_context_path"), &InputRouter::get_gamepad_context_path);
	ClassDB::bind_method(D_METHOD("is_gamepad_context_active", "context"), &InputRouter::is_gamepad_context_active);

	ClassDB::bind_method(D_METHOD("attach", "binding", "context", "target"), &InputRouter::attach, DEFVAL(StringName()), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("detach", "handle"), &InputRouter::detach);
	ClassDB::bind_method(D_METHOD("load_map", "map"), &InputRouter::load_map);
	ClassDB::bind_method(D_METHOD("unload_map", "handle"), &InputRouter::unload_map);
	ClassDB::bind_method(D_METHOD("set_callback", "context", "binding_id", "target"), &InputRouter::set_callback);
	ClassDB::bind_method(D_METHOD("clear_callback", "handle"), &InputRouter::clear_callback);
	ClassDB::bind_method(D_METHOD("clear"), &InputRouter::clear);
	ClassDB::bind_method(D_METHOD("get_routes"), &InputRouter::_get_routes);

	ClassDB::bind_method(D_METHOD("set_scheme", "scheme"), &InputRouter::set_scheme);
	ClassDB::bind_method(D_METHOD("get_scheme"), &InputRouter::get_scheme);
	ClassDB::bind_method(D_METHOD("get_schemes"), &InputRouter::_get_schemes);
	ClassDB::bind_method(D_METHOD("set_override", "binding_id", "input", "gate"), &InputRouter::set_override, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("clear_override", "binding_id"), &InputRouter::clear_override);
	ClassDB::bind_method(D_METHOD("clear_overrides"), &InputRouter::clear_overrides);
	ClassDB::bind_method(D_METHOD("get_overrides"), &InputRouter::_get_overrides);
	ClassDB::bind_method(D_METHOD("set_sequence_timeout", "seconds"), &InputRouter::set_sequence_timeout);
	ClassDB::bind_method(D_METHOD("get_sequence_timeout"), &InputRouter::get_sequence_timeout);
	ClassDB::bind_method(D_METHOD("set_chord_window", "seconds"), &InputRouter::set_chord_window);
	ClassDB::bind_method(D_METHOD("get_chord_window"), &InputRouter::get_chord_window);

	ClassDB::bind_method(D_METHOD("set_route_map", "map"), &InputRouter::set_route_map);

	ClassDB::bind_method(D_METHOD("set_touch_mode", "mode"), &InputRouter::set_touch_mode);
	ClassDB::bind_method(D_METHOD("get_touch_mode"), &InputRouter::get_touch_mode);
	ClassDB::bind_method(D_METHOD("take_pointer_delta"), &InputRouter::take_pointer_delta);
	ClassDB::bind_method(D_METHOD("get_pointer_velocity"), &InputRouter::get_pointer_velocity);

	ClassDB::bind_method(D_METHOD("set_trace_enabled", "enabled", "capacity"), &InputRouter::set_trace_enabled, DEFVAL(4096));
	ClassDB::bind_method(D_METHOD("is_trace_enabled"), &InputRouter::is_trace_enabled);
	ClassDB::bind_method(D_METHOD("get_trace"), &InputRouter::_get_trace);
#ifdef TOOLS_ENABLED
	ClassDB::bind_static_method("InputRouter", D_METHOD("check_maps", "contexts", "maps", "placements", "paths"), &InputRouter::_check_maps);
#endif

	ADD_SIGNAL(MethodInfo("device_changed", PropertyInfo(Variant::INT, "device"), PropertyInfo(Variant::INT, "pad"), PropertyInfo(Variant::INT, "previous_device"), PropertyInfo(Variant::INT, "previous_pad")));
	ADD_SIGNAL(MethodInfo("pad_connected", PropertyInfo(Variant::INT, "pad")));
	ADD_SIGNAL(MethodInfo("pad_disconnected", PropertyInfo(Variant::INT, "pad")));
	ADD_SIGNAL(MethodInfo("active_pad_lost", PropertyInfo(Variant::INT, "pad")));
	ADD_SIGNAL(MethodInfo("gamepad_context_changed", PropertyInfo(Variant::PACKED_STRING_ARRAY, "path")));
	ADD_SIGNAL(MethodInfo("routes_changed"));
	ADD_SIGNAL(MethodInfo("scheme_changed", PropertyInfo(Variant::STRING_NAME, "scheme")));
	ADD_SIGNAL(MethodInfo("touch_routed", PropertyInfo(Variant::INT, "index"), PropertyInfo(Variant::INT, "phase"), PropertyInfo(Variant::VECTOR2, "position"), PropertyInfo(Variant::FLOAT, "pressure")));

	BIND_CONSTANT(INPUT_COUNT);

	BIND_ENUM_CONSTANT(DEVICE_NONE);
	BIND_ENUM_CONSTANT(DEVICE_KBM);
	BIND_ENUM_CONSTANT(DEVICE_PAD);

	BIND_ENUM_CONSTANT(DEVICE_POLICY_AUTO);
	BIND_ENUM_CONSTANT(DEVICE_POLICY_PAD);
	BIND_ENUM_CONSTANT(DEVICE_POLICY_KBM);

	BIND_ENUM_CONSTANT(MOUSE_IN_PAD_DROP);
	BIND_ENUM_CONSTANT(MOUSE_IN_PAD_POINTER);

	BIND_ENUM_CONSTANT(TOUCH_AS_POINTER);
	BIND_ENUM_CONSTANT(TOUCH_AS_PAD);
	BIND_ENUM_CONSTANT(TOUCH_IGNORE);

	BIND_ENUM_CONSTANT(PAD_FAMILY_GENERIC);
	BIND_ENUM_CONSTANT(PAD_FAMILY_XBOX);
	BIND_ENUM_CONSTANT(PAD_FAMILY_PLAYSTATION);
	BIND_ENUM_CONSTANT(PAD_FAMILY_NINTENDO);
	BIND_ENUM_CONSTANT(PAD_FAMILY_STEAM);
	BIND_ENUM_CONSTANT(PAD_FAMILY_MAX);

	BIND_ENUM_CONSTANT(PHASE_PRESSED);
	BIND_ENUM_CONSTANT(PHASE_RELEASED);
	BIND_ENUM_CONSTANT(PHASE_CANCELED);
	BIND_ENUM_CONSTANT(PHASE_HELD);
	BIND_ENUM_CONSTANT(PHASE_TAPPED);
	BIND_ENUM_CONSTANT(PHASE_REPEATED);
	BIND_ENUM_CONSTANT(PHASE_CHANGED);
}
