#pragma once

// The functions the router, the inspector and the baker share, then the router itself driven
// through Input, one input_poll_process_flip() per frame, as Main::iteration drives it.

#include "modules/input_router/input_router.h"
#include "modules/input_router/input_router_internal.hpp"
#include "modules/input_router/line_vector.h"

#include "core/input/input_map.h"
#include "tests/test_macros.h"

namespace TestInputRouter {

using namespace InputRouterInternal;

TEST_CASE("[InputRouter] Deadzone, saturation and anti-deadzone are exact") {
	RouteBinding row;
	row.deadzone = 0.2f;
	row.saturation = 0.8f;
	CHECK(shape_magnitude(row, nullptr, 0.2f) == 0.0f);
	CHECK(shape_magnitude(row, nullptr, 0.5f) == doctest::Approx(0.5f));
	CHECK(shape_magnitude(row, nullptr, 0.8f) == doctest::Approx(1.0f));
	CHECK(shape_magnitude(row, nullptr, 1.0f) == doctest::Approx(1.0f));
	row.anti_deadzone = 0.1f;
	CHECK(shape_magnitude(row, nullptr, 0.2001f) == doctest::Approx(0.1f).epsilon(0.01));
	CHECK(shape_axis(row, nullptr, -0.5f, 1) == 0.0f);
	CHECK(shape_axis(row, nullptr, -0.8f, 0) == doctest::Approx(-1.0f));
}

TEST_CASE("[InputRouter] Radial stick keeps direction") {
	RouteBinding row;
	row.deadzone = 0.1f;
	row.saturation = 1.0f;
	const Vector2 shaped = shape_stick(row, nullptr, Vector2(0.3f, 0.4f));
	CHECK(shaped.normalized().is_equal_approx(Vector2(0.6f, 0.8f)));
	CHECK(shape_stick(row, nullptr, Vector2(0.05f, 0.05f)) == Vector2());
}

TEST_CASE("[InputRouter] Press and release use hysteresis") {
	RouteBinding row;
	row.trigger = RouteBindingResource::TRIGGER_PRESS;
	row.press = 0.5f;
	row.release = 0.4f;
	Claim c;
	CHECK(trigger_step(row, c, 0.45f, 0) == FIRE_NONE);
	CHECK(trigger_step(row, c, 0.55f, 10) == FIRE_PRESSED);
	CHECK(trigger_step(row, c, 0.45f, 20) == FIRE_NONE);
	CHECK(trigger_step(row, c, 0.35f, 30) == FIRE_RELEASED);
}

TEST_CASE("[InputRouter] Hold, tap and repeat fire on time") {
	RouteBinding row;
	row.hold_usec = 400000;
	row.tap_usec = 250000;
	row.repeat_delay_usec = 400000;
	row.repeat_interval_usec = 100000;

	row.trigger = RouteBindingResource::TRIGGER_HOLD;
	Claim hold;
	CHECK(trigger_step(row, hold, 1.0f, 0) == FIRE_NONE);
	CHECK(trigger_step(row, hold, 1.0f, 399999) == FIRE_NONE);
	CHECK(trigger_step(row, hold, 1.0f, 400000) == FIRE_HELD);
	CHECK(trigger_step(row, hold, 1.0f, 500000) == FIRE_NONE);
	CHECK(trigger_step(row, hold, 0.0f, 600000) == FIRE_RELEASED);

	row.trigger = RouteBindingResource::TRIGGER_TAP;
	Claim tap;
	trigger_step(row, tap, 1.0f, 0);
	CHECK(trigger_step(row, tap, 0.0f, 200000) == FIRE_TAPPED);
	trigger_step(row, tap, 1.0f, 1000000);
	CHECK(trigger_step(row, tap, 0.0f, 1300000) == FIRE_NONE);

	row.trigger = RouteBindingResource::TRIGGER_REPEAT;
	Claim rep;
	CHECK(trigger_step(row, rep, 1.0f, 0) == FIRE_PRESSED);
	CHECK(trigger_step(row, rep, 1.0f, 400000) == FIRE_REPEATED);
	CHECK(trigger_step(row, rep, 1.0f, 450000) == FIRE_NONE);
	CHECK(trigger_step(row, rep, 1.0f, 2000000) == FIRE_REPEATED); // Late frame: one, not a burst.
	CHECK(trigger_step(row, rep, 1.0f, 2050000) == FIRE_NONE);
}

TEST_CASE("[InputRouter] Analog reports every change, back to zero included") {
	RouteBinding row;
	row.trigger = RouteBindingResource::TRIGGER_ANALOG;
	Claim c;
	CHECK((trigger_step(row, c, 0.3f, 0) & FIRE_CHANGED) != 0);
	CHECK((trigger_step(row, c, 0.3f, 10) & FIRE_CHANGED) == 0);
	CHECK((trigger_step(row, c, 0.0f, 20) & FIRE_CHANGED) != 0);
}

TEST_CASE("[InputRouter] Sequences complete, time out and restart") {
	RouteBinding row;
	row.trigger = RouteBindingResource::TRIGGER_SEQUENCE;
	row.input = button_slot(JoyButton::X);
	row.sequence[0] = button_slot(JoyButton::Y);
	row.sequence[1] = button_slot(JoyButton::A);
	uint8_t step = 0;
	uint64_t until = 0;
	const uint64_t timeout = 800000;

	CHECK(sequence_step(row, step, until, row.input, 0, timeout) == FIRE_NONE);
	CHECK(sequence_step(row, step, until, row.sequence[0], 100000, timeout) == FIRE_NONE);
	CHECK(sequence_step(row, step, until, row.sequence[1], 200000, timeout) == FIRE_PRESSED);
	CHECK(step == 0);

	sequence_step(row, step, until, row.input, 1000000, timeout);
	CHECK(sequence_step(row, step, until, NONE, 1900000, timeout) == FIRE_SEQUENCE_ABANDONED);

	sequence_step(row, step, until, row.input, 3000000, timeout);
	CHECK(sequence_step(row, step, until, row.input, 3100000, timeout) == FIRE_SEQUENCE_ABANDONED);
	CHECK(step == 1); // The wrong button was the input: restarted.
}

TEST_CASE("[InputRouter] Compile refuses data that cannot route") {
	Ref<RouteBindingResource> b;
	b.instantiate();
	b->set_button(JoyButton::A);
	b->set_gate(JoyButton::A);
	RouteBinding row;
	float lut[LUT_STRIDE];
	bool has_lut = false;
	String error;
	CHECK_FALSE(compile_binding(**b, row, lut, has_lut, error));

	b->set_gate(JoyButton::LEFT_SHOULDER);
	CHECK(compile_binding(**b, row, lut, has_lut, error));
	CHECK(row.input == button_slot(JoyButton::A));
	CHECK(row.gate == button_slot(JoyButton::LEFT_SHOULDER));
	CHECK(row.authored_input == row.input);
	CHECK(row.authored_gate == row.gate);
	CHECK(row.context == NONE);

	b->set_source(RouteBindingResource::SOURCE_AXIS);
	b->set_pointer(true);
	CHECK_FALSE(compile_binding(**b, row, lut, has_lut, error));
}

TEST_CASE("[InputRouter] Labels are positional per family") {
	CHECK(button_label(JoyButton::A, InputRouter::PAD_FAMILY_NINTENDO) == "B");
	CHECK(button_label(JoyButton::A, InputRouter::PAD_FAMILY_PLAYSTATION) == "Cross");
	CHECK(axis_label(JoyAxis::TRIGGER_RIGHT, 1, InputRouter::PAD_FAMILY_NINTENDO) == "ZR");
	CHECK(axis_label(JoyAxis::LEFT_Y, -1, InputRouter::PAD_FAMILY_XBOX) == "LS Up");
	CHECK(input_label(button_slot(JoyButton::PADDLE1), InputRouter::PAD_FAMILY_STEAM) == "R4");
}

TEST_CASE("[InputRouter] Rows survive a byte copy; tuning ignores placement") {
	RouteBinding row;
	row.input = axis_slot(JoyAxis::TRIGGER_RIGHT, 1);
	row.gate = button_slot(JoyButton::LEFT_SHOULDER);
	row.trigger = RouteBindingResource::TRIGGER_ANALOG;
	row.flags = ROW_LOCK_AXIS;
	row.scheme_mask = 0x02;
	row.priority = -3;
	row.deadzone = 0.15f;
	row.hold_usec = 350000;
	row.lut = 4;
	uint8_t bytes[sizeof(RouteBinding)];
	memcpy(bytes, &row, sizeof(RouteBinding));
	RouteBinding back;
	memcpy(&back, bytes, sizeof(RouteBinding));
	CHECK(back.same_tuning(row));
	CHECK(back.scheme_mask == row.scheme_mask);
	CHECK(back.lut == row.lut);

	back.context = 7;
	back.lut = NO_LUT;
	CHECK(back.same_tuning(row)); // Placement, not behaviour.
	back.priority = 0;
	CHECK_FALSE(back.same_tuning(row));
}

TEST_CASE("[InputRouter] LineVector keeps elements on cache lines through growth") {
	LineVector<RouteBinding> rows;
	for (int i = 0; i < 40; i++) {
		RouteBinding row;
		row.priority = (int8_t)i;
		rows.push_back(row);
	}
	CHECK(rows.size() == 40);
	CHECK(((uintptr_t)&rows[0] % CACHE_LINE) == 0);
	CHECK(rows[39].priority == 39);

	LineVector<StringName> names;
	for (int i = 0; i < 20; i++) {
		names.push_back(StringName(itos(i)));
	}
	CHECK(((uintptr_t)&names[0] % CACHE_LINE) == 0);
	CHECK(names[19] == StringName("19"));
	names.clear();
	CHECK(names.size() == 0);
}

TEST_CASE("[InputRouter] The route map merges by (id, scheme) and validates") {
	Dictionary map = Baked::empty();
	Ref<RouteBindingResource> b;
	b.instantiate();
	b->set_id("fire");
	b->set_action("fire");
	b->set_button(JoyButton::A);
	String error;
	CHECK(Baked::add_binding(map, **b, error) == Baked::ADD_NEW);
	CHECK(Baked::add_binding(map, **b, error) == Baked::ADD_SAME);
	b->set_scheme("modern");
	CHECK(Baked::add_binding(map, **b, error) == Baked::ADD_NEW); // Same id, another scheme.
	b->set_button(JoyButton::B);
	CHECK(Baked::add_binding(map, **b, error) == Baked::ADD_REPLACED);
	CHECK(PackedStringArray(map[Baked::KEY_IDS]).size() == 2);
	CHECK(PackedStringArray(map[Baked::KEY_SCHEMES]).size() == 1);
	CHECK(Baked::validate(map).is_empty());
	b->set_id(StringName());
	CHECK(Baked::add_binding(map, **b, error) == Baked::ADD_FAILED);
	CHECK(Baked::add_context(map, "test_bad_fallback", GamepadContext::FALLBACK_PASS + 1, false));
	CHECK_FALSE(Baked::validate(map).is_empty()); // Would set other slot flags.
}

static Ref<RouteBindingResource> test_binding(const StringName &p_id, JoyButton p_button) {
	Ref<RouteBindingResource> b;
	b.instantiate();
	b->set_id(p_id);
	b->set_action(p_id);
	b->set_button(p_button);
	return b;
}

// One engine frame: pad 0's button queued in Input the way a driver queues it, then the router's
// frame.
static void frame_button(InputRouter *p_router, JoyButton p_button, bool p_pressed) {
	Ref<InputEventJoypadButton> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_button_index(p_button);
	ev->set_pressed(p_pressed);
	Input::get_singleton()->parse_input_event(ev);
	p_router->input_poll_process_flip();
}

// One engine frame with nothing buffered.
static void frame_idle(InputRouter *p_router) {
	p_router->input_poll_process_flip();
}

// set_enabled() lands with the next frame's flip, a disable included.
static void enable_router(InputRouter *p_router, bool p_enabled) {
	p_router->set_enabled(p_enabled);
	CHECK(p_router->is_enabled() != p_enabled);
	frame_idle(p_router);
	REQUIRE(p_router->is_enabled() == p_enabled);
}

// [SceneTree] gives the case an Input. Leaves the singleton disabled with no rows.
TEST_CASE("[SceneTree][InputRouter] A routed press reaches Input as its action; getters follow the flip") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	InputMap::get_singleton()->add_action("test_press");
	const Ref<RouteBindingResource> binding = test_binding("test_press", JoyButton::A);
	Dictionary data = Baked::empty();
	String error;
	REQUIRE(Baked::add_binding(data, **binding, error) == Baked::ADD_NEW);
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(data);
	router->set_route_map(map); // Nothing placed yet: applies at once.

	const int handle = router->attach(binding, "test_context");
	REQUIRE(handle != 0);
	router->set_gamepad_base_contexts(PackedStringArray({ "test_context" }));
	enable_router(router, true);
	router->joy_connection_changed(0, true);

	frame_button(router, JoyButton::A, true);
	CHECK(input->is_action_pressed("test_press"));
	CHECK(router->get_device() == InputRouter::DEVICE_PAD);
	CHECK(router->get_active_pad() == 0);
	CHECK(router->is_pad_button_down(JoyButton::A));
	CHECK(router->get_gamepad_context() == StringName("test_context"));

	frame_button(router, JoyButton::A, false);
	CHECK_FALSE(input->is_action_pressed("test_press"));
	CHECK_FALSE(router->is_pad_button_down(JoyButton::A));

	const uint64_t frame = router->get_frame();
	for (int i = 0; i < 40; i++) {
		frame_button(router, JoyButton::A, i % 2 == 0);
		CHECK(input->is_action_pressed("test_press") == (i % 2 == 0)); // Pooled events, reused.
	}
	CHECK(router->get_frame() == frame + 40);

	enable_router(router, false);
	router->detach(handle);
	router->set_gamepad_base_contexts(PackedStringArray());
	InputMap::get_singleton()->erase_action("test_press");
}

TEST_CASE("[SceneTree][InputRouter] A binding the route map lacks is merged and routes from the next frame") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	InputMap::get_singleton()->add_action("test_late");
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(Baked::empty());
	router->set_route_map(map);
	router->set_gamepad_base_contexts(PackedStringArray({ "test_context" }));
	enable_router(router, true);
	router->joy_connection_changed(0, true);

	ERR_PRINT_OFF; // The rebake warning.
	const int handle = router->attach(test_binding("test_late", JoyButton::B), "test_context");
	ERR_PRINT_ON;
	REQUIRE(handle != 0);

	frame_button(router, JoyButton::B, true); // _poll() merges and places it before routing.
	CHECK(input->is_action_pressed("test_late"));
	frame_button(router, JoyButton::B, false);
	CHECK_FALSE(input->is_action_pressed("test_late"));

	enable_router(router, false);
	router->detach(handle);
	router->set_gamepad_base_contexts(PackedStringArray());
	InputMap::get_singleton()->erase_action("test_late");
}

TEST_CASE("[SceneTree][InputRouter] State carries through the frame ring; edits wait for the flip") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(Baked::empty());
	router->set_route_map(map);
	router->set_gamepad_base_contexts(PackedStringArray({ "test_context" }));
	enable_router(router, true);
	router->joy_connection_changed(0, true);

	// HEAD and BODY fork from the read frame at every flip: a held button outlives several laps.
	frame_button(router, JoyButton::A, true);
	CHECK(router->was_pad_button_pressed(JoyButton::A, 0));
	const uint64_t frame = router->get_frame();
	for (uint64_t i = 1; i <= 9; i++) {
		frame_idle(router);
		CHECK(router->is_pad_button_down(JoyButton::A, 0));
		CHECK_FALSE(router->was_pad_button_pressed(JoyButton::A, 0)); // Edges last one frame.
		CHECK(router->get_frame() == frame + i);
	}

	// A connect the frame already shows, as when enable() races the ring, does not reset the pad.
	router->joy_connection_changed(0, true);
	frame_idle(router);
	CHECK(router->is_pad_button_down(JoyButton::A, 0));
	frame_button(router, JoyButton::A, false);
	CHECK_FALSE(router->is_pad_button_down(JoyButton::A, 0));

	// FOOT: the push copies the names into the spare slot; getters keep the read frame's until
	// the flip, and a frame with no edits shares the new copy.
	CHECK(router->get_gamepad_context() == StringName("test_context"));
	const int overlay = router->push_gamepad_context("test_overlay");
	CHECK(router->get_gamepad_context() == StringName("test_context"));
	frame_idle(router);
	CHECK(router->get_gamepad_context() == StringName("test_overlay"));
	frame_idle(router);
	const PackedStringArray path = router->get_gamepad_context_path();
	REQUIRE(path.size() == 2);
	CHECK(path[0] == "test_context");
	CHECK(path[1] == "test_overlay");
	router->pop_gamepad_context(overlay);
	CHECK(router->get_gamepad_context() == StringName("test_overlay"));
	frame_idle(router);
	CHECK(router->get_gamepad_context() == StringName("test_context"));

	enable_router(router, false);
	CHECK(Input::get_singleton()->get_frame_filter() == nullptr);
	router->set_gamepad_base_contexts(PackedStringArray());
}

} // namespace TestInputRouter
