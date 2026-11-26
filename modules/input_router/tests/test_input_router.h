#pragma once

// The functions the router, the inspector and the baker share, then the router itself driven
// through Input, one input_poll_process_flip() per frame, as Main::iteration drives it.

#include "modules/input_router/input_router.h"
#include "modules/input_router/input_router_internal.hpp"
#include "modules/input_router/line_vector.h"

#include "core/input/input_map.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
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

// One engine frame with a key, then a left mouse button, queued the way a display server queues them.
static void frame_key_and_mouse(InputRouter *p_router, Key p_key, bool p_pressed) {
	Ref<InputEventKey> key;
	key.instantiate();
	key->set_keycode(p_key);
	key->set_physical_keycode(p_key);
	key->set_pressed(p_pressed);
	Input::get_singleton()->parse_input_event(key);
	Ref<InputEventMouseButton> mb;
	mb.instantiate();
	mb->set_button_index(MouseButton::LEFT);
	mb->set_pressed(p_pressed);
	Input::get_singleton()->parse_input_event(mb);
	p_router->input_poll_process_flip();
}

TEST_CASE("[SceneTree][InputRouter] A route map merge mid-play keeps held keys and mouse buttons") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(Baked::empty());
	router->set_route_map(map);
	router->set_gamepad_base_contexts(PackedStringArray({ "test_context" }));
	enable_router(router, true);
	router->set_device(InputRouter::DEVICE_KBM, -1); // Earlier cases may leave the pad in charge.
	frame_idle(router);

	frame_key_and_mouse(router, Key::W, true);
	REQUIRE(router->get_device() == InputRouter::DEVICE_KBM);
	REQUIRE(input->is_key_pressed(Key::W));
	REQUIRE(input->is_mouse_button_pressed(MouseButton::LEFT));

	ERR_PRINT_OFF; // The rebake warning.
	const int handle = router->attach(test_binding("test_merge", JoyButton::Y), "test_context");
	ERR_PRINT_ON;
	REQUIRE(handle != 0);
	frame_idle(router); // _poll() merges the binding and rebuilds the rows.
	CHECK(input->is_key_pressed(Key::W));
	CHECK(input->is_mouse_button_pressed(MouseButton::LEFT));

	frame_key_and_mouse(router, Key::W, false); // Not suppressed: the releases pass.
	CHECK_FALSE(input->is_key_pressed(Key::W));
	CHECK_FALSE(input->is_mouse_button_pressed(MouseButton::LEFT));

	enable_router(router, false);
	router->detach(handle);
	router->set_gamepad_base_contexts(PackedStringArray());
}

static int focus_test_w_events = 0;

static void count_w_event(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->get_physical_keycode() == Key::W) {
		focus_test_w_events++;
	}
}

static void frame_key(InputRouter *p_router, Key p_key, bool p_pressed, bool p_echo) {
	Ref<InputEventKey> key;
	key.instantiate();
	key->set_keycode(p_key);
	key->set_physical_keycode(p_key);
	key->set_pressed(p_pressed);
	key->set_echo(p_echo);
	Input::get_singleton()->parse_input_event(key);
	p_router->input_poll_process_flip();
}

// Divergence D3 (ruled intended): focus loss (Input::release_pressed_events, what a display
// server calls on focus out) drops Input's held state. What the OS still sends for keys held then,
// the echo and the late release, never reaches the game; the next press passes.
TEST_CASE("[SceneTree][InputRouter] Focus loss eats the echo and release of keys held at the time") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	Window *root = SceneTree::get_singleton()->get_root();
	const Callable counter = callable_mp_static(&count_w_event);
	root->connect(StringName("window_input"), counter);
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(Baked::empty());
	router->set_route_map(map);
	enable_router(router, true);
	router->set_device(InputRouter::DEVICE_KBM, -1);
	frame_idle(router);

	focus_test_w_events = 0;
	frame_key(router, Key::W, true, false);
	REQUIRE(input->is_key_pressed(Key::W));
	REQUIRE(focus_test_w_events == 1);

	input->release_pressed_events();
	CHECK_FALSE(input->is_key_pressed(Key::W));
	frame_key(router, Key::W, true, true); // The OS keeps repeating: eaten.
	frame_key(router, Key::W, false, false); // The late release: eaten.
	CHECK(focus_test_w_events == 1);
	CHECK_FALSE(input->is_key_pressed(Key::W));

	frame_key(router, Key::W, true, false); // A new press passes, and so does its release.
	CHECK(input->is_key_pressed(Key::W));
	frame_key(router, Key::W, false, false);
	CHECK_FALSE(input->is_key_pressed(Key::W));
	CHECK(focus_test_w_events == 3);

	enable_router(router, false);
	root->disconnect(StringName("window_input"), counter);
}

// Bug 13: each new context grows the name table one past the other slot's size, so every
// copy-on-write lands in a slot that holds no element yet. Before copy_names() that assigned a
// StringName over raw storage and crashed whenever the storage was not zero.
TEST_CASE("[SceneTree][InputRouter] The context table grows across flips and keeps its names") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(Baked::empty());
	router->set_route_map(map);
	enable_router(router, true);

	PackedStringArray path;
	for (int i = 0; i < 12; i++) {
		path.push_back(vformat("test_grow_%d", i));
		router->set_gamepad_base_contexts(path);
		frame_idle(router);
		CHECK(router->get_gamepad_context_path() == path);
	}
	for (int i = 0; i < 6; i++) {
		path.remove_at(path.size() - 1);
		router->set_gamepad_base_contexts(path);
		frame_idle(router);
		frame_idle(router);
		CHECK(router->get_gamepad_context_path() == path);
	}

	enable_router(router, false);
	router->set_gamepad_base_contexts(PackedStringArray());
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

// --- Modifier layers, callbacks and action slots ---

static LocalVector<int> phase_log; // tag * 16 + phase.

static void log_phase(int p_phase, const Variant &p_value, int p_tag) {
	phase_log.push_back(p_tag * 16 + p_phase);
}

static bool logged(int p_tag, InputRouter::Phase p_phase) {
	for (int entry : phase_log) {
		if (entry == p_tag * 16 + (int)p_phase) {
			return true;
		}
	}
	return false;
}

static bool logged_any(int p_tag) {
	for (int entry : phase_log) {
		if (entry / 16 == p_tag) {
			return true;
		}
	}
	return false;
}

static Callable phase_logger(int p_tag) {
	return callable_mp_static(&log_phase).bind(p_tag);
}

static Ref<RouteBindingResource> test_chord(const StringName &p_id, JoyButton p_button, JoyButton p_gate) {
	Ref<RouteBindingResource> b = test_binding(p_id, p_button);
	b->set_gate(p_gate);
	return b;
}

static Ref<RouteBindingResource> test_stick(const StringName &p_id, JoyButton p_gate) {
	Ref<RouteBindingResource> b;
	b.instantiate();
	b->set_id(p_id);
	b->set_source(RouteBindingResource::SOURCE_STICK);
	b->set_stick(RouteBindingResource::STICK_LEFT);
	b->set_trigger(RouteBindingResource::TRIGGER_ANALOG);
	b->set_gate(p_gate);
	return b;
}

static Ref<RouteBindingResource> test_axis(const StringName &p_id, JoyAxis p_axis, int p_direction, RouteBindingResource::Trigger p_trigger, JoyButton p_gate) {
	Ref<RouteBindingResource> b;
	b.instantiate();
	b->set_id(p_id);
	b->set_source(RouteBindingResource::SOURCE_AXIS);
	b->set_axis(p_axis);
	b->set_axis_direction(p_direction);
	b->set_trigger(p_trigger);
	b->set_gate(p_gate);
	return b;
}

// A route map holding p_bindings, applied at once while nothing is placed.
static void set_test_map(InputRouter *p_router, const Vector<Ref<RouteBindingResource>> &p_bindings) {
	Dictionary data = Baked::empty();
	for (const Ref<RouteBindingResource> &b : p_bindings) {
		String error;
		REQUIRE(Baked::add_binding(data, **b, error) == Baked::ADD_NEW);
	}
	Ref<GamepadBakedMaps> map;
	map.instantiate();
	map->set_data(data);
	p_router->set_route_map(map);
}

// Enabled, pad 0 connected and in charge, so the next press routes instead of waking. No chord
// window: frames run back to back, so a held-back plain press would wait out real time.
static void start_pad_case(InputRouter *p_router, const PackedStringArray &p_path) {
	p_router->set_chord_window(0.0f);
	p_router->set_gamepad_base_contexts(p_path);
	enable_router(p_router, true);
	p_router->joy_connection_changed(0, true);
	p_router->set_device(InputRouter::DEVICE_PAD, 0);
	frame_idle(p_router);
	REQUIRE(p_router->get_device() == InputRouter::DEVICE_PAD);
	phase_log.clear();
}

static void end_pad_case(InputRouter *p_router, const LocalVector<int> &p_handles) {
	enable_router(p_router, false);
	for (int handle : p_handles) {
		p_router->detach(handle);
	}
	p_router->set_gamepad_base_contexts(PackedStringArray());
	p_router->set_chord_window(0.05f);
	phase_log.clear();
}

static void frame_axis(InputRouter *p_router, JoyAxis p_axis, float p_value) {
	Ref<InputEventJoypadMotion> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_axis(p_axis);
	ev->set_axis_value(p_value);
	Input::get_singleton()->parse_input_event(ev);
	p_router->input_poll_process_flip();
}

// Bug 5: letting go of the modifier ends its layer at once, and the input still held never drops
// into the base binding. A bare gate press never reaches Godot.
TEST_CASE("[SceneTree][InputRouter] Releasing a gate ends its chords; the held input stays eaten until released") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	InputMap::get_singleton()->add_action("test_chord");
	InputMap::get_singleton()->add_action("test_plain");
	const Ref<RouteBindingResource> chord = test_chord("test_chord", JoyButton::A, JoyButton::LEFT_SHOULDER);
	const Ref<RouteBindingResource> plain = test_binding("test_plain", JoyButton::A);
	set_test_map(router, { chord, plain });
	LocalVector<int> handles;
	handles.push_back(router->attach(chord, "test_context"));
	handles.push_back(router->attach(plain, "test_context"));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_button(router, JoyButton::LEFT_SHOULDER, true);
	CHECK_FALSE(input->is_joy_button_pressed(0, JoyButton::LEFT_SHOULDER)); // The modifier is the router's.
	frame_button(router, JoyButton::A, true);
	CHECK(input->is_action_pressed("test_chord"));
	CHECK_FALSE(input->is_action_pressed("test_plain"));

	frame_button(router, JoyButton::LEFT_SHOULDER, false);
	CHECK_FALSE(input->is_action_pressed("test_chord"));
	frame_button(router, JoyButton::A, true); // A is still down: a repeat of its press is eaten.
	CHECK_FALSE(input->is_action_pressed("test_plain"));
	frame_button(router, JoyButton::A, false);
	frame_button(router, JoyButton::A, true);
	CHECK(input->is_action_pressed("test_plain"));
	frame_button(router, JoyButton::A, false);
	CHECK_FALSE(input->is_action_pressed("test_plain"));

	end_pad_case(router, handles);
	InputMap::get_singleton()->erase_action("test_chord");
	InputMap::get_singleton()->erase_action("test_plain");
}

// The overlay rule: a modifier's chords in an outer context win over an inner context's plain
// binding while the modifier is held; without it the inner binding still wins.
TEST_CASE("[SceneTree][InputRouter] A held modifier's layer reaches past an inner context's plain binding") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	InputMap::get_singleton()->add_action("test_layer");
	InputMap::get_singleton()->add_action("test_inner");
	const Ref<RouteBindingResource> layer = test_chord("test_layer", JoyButton::A, JoyButton::LEFT_SHOULDER);
	const Ref<RouteBindingResource> inner = test_binding("test_inner", JoyButton::A);
	set_test_map(router, { layer, inner });
	LocalVector<int> handles;
	handles.push_back(router->attach(layer, "test_outer"));
	handles.push_back(router->attach(inner, "test_inner"));
	start_pad_case(router, PackedStringArray({ "test_outer", "test_inner" }));

	frame_button(router, JoyButton::LEFT_SHOULDER, true);
	frame_button(router, JoyButton::A, true);
	CHECK(input->is_action_pressed("test_layer"));
	CHECK_FALSE(input->is_action_pressed("test_inner"));
	frame_button(router, JoyButton::A, false);
	frame_button(router, JoyButton::LEFT_SHOULDER, false);

	frame_button(router, JoyButton::A, true);
	CHECK(input->is_action_pressed("test_inner"));
	CHECK_FALSE(input->is_action_pressed("test_layer"));
	frame_button(router, JoyButton::A, false);

	end_pad_case(router, handles);
	InputMap::get_singleton()->erase_action("test_layer");
	InputMap::get_singleton()->erase_action("test_inner");
}

TEST_CASE("[SceneTree][InputRouter] A gate's own binding fires on release, only when no chord used it") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> own = test_binding("test_gate_own", JoyButton::LEFT_SHOULDER);
	const Ref<RouteBindingResource> chord = test_chord("test_gate_chord", JoyButton::A, JoyButton::LEFT_SHOULDER);
	own->set_action(StringName());
	chord->set_action(StringName());
	set_test_map(router, { own, chord });
	LocalVector<int> handles;
	handles.push_back(router->attach(own, "test_context", phase_logger(1)));
	handles.push_back(router->attach(chord, "test_context", phase_logger(2)));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_button(router, JoyButton::LEFT_SHOULDER, true);
	CHECK_FALSE(logged_any(1)); // Waits for the release.
	frame_button(router, JoyButton::LEFT_SHOULDER, false);
	CHECK(logged(1, InputRouter::PHASE_PRESSED));
	CHECK(logged(1, InputRouter::PHASE_RELEASED));

	phase_log.clear();
	frame_button(router, JoyButton::LEFT_SHOULDER, true);
	frame_button(router, JoyButton::A, true);
	frame_button(router, JoyButton::A, false);
	frame_button(router, JoyButton::LEFT_SHOULDER, false);
	CHECK(logged(2, InputRouter::PHASE_PRESSED));
	CHECK(logged(2, InputRouter::PHASE_RELEASED));
	CHECK_FALSE(logged_any(1));

	end_pad_case(router, handles);
}

// Bug 10: a press whose release never arrived ends the old claim with CANCELED first.
TEST_CASE("[SceneTree][InputRouter] A lost release cancels the old claim before the new press") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> b = test_binding("test_lost", JoyButton::A);
	b->set_action(StringName());
	set_test_map(router, { b });
	LocalVector<int> handles;
	handles.push_back(router->attach(b, "test_context", phase_logger(3)));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_button(router, JoyButton::A, true);
	frame_button(router, JoyButton::A, true); // The release was lost.
	REQUIRE(phase_log.size() == 3);
	CHECK(phase_log[0] == 3 * 16 + InputRouter::PHASE_PRESSED);
	CHECK(phase_log[1] == 3 * 16 + InputRouter::PHASE_CANCELED);
	CHECK(phase_log[2] == 3 * 16 + InputRouter::PHASE_PRESSED);
	frame_button(router, JoyButton::A, false);
	CHECK(logged(3, InputRouter::PHASE_RELEASED));

	end_pad_case(router, handles);
}

static int shared_test_presses = 0;

static void count_shared_press(const Ref<InputEvent> &p_event) {
	const Ref<InputEventAction> a = p_event;
	if (a.is_valid() && a->get_action() == StringName("test_shared") && a->is_pressed()) {
		shared_test_presses++;
	}
}

// Bug 2: the action stays down while either binding holds it, and letting go of one while the
// other holds it dispatches nothing (no extra press for focus navigation to step on).
TEST_CASE("[SceneTree][InputRouter] Two bindings on one action press and release it independently") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Input *input = Input::get_singleton();
	Window *root = SceneTree::get_singleton()->get_root();
	const Callable counter = callable_mp_static(&count_shared_press);
	root->connect(StringName("window_input"), counter);
	InputMap::get_singleton()->add_action("test_shared");
	const Ref<RouteBindingResource> a = test_binding("test_shared_a", JoyButton::A);
	const Ref<RouteBindingResource> b = test_binding("test_shared_b", JoyButton::B);
	a->set_action("test_shared");
	b->set_action("test_shared");
	set_test_map(router, { a, b });
	LocalVector<int> handles;
	handles.push_back(router->attach(a, "test_context"));
	handles.push_back(router->attach(b, "test_context"));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	shared_test_presses = 0;
	frame_button(router, JoyButton::A, true);
	frame_button(router, JoyButton::B, true);
	CHECK(shared_test_presses == 2);
	frame_button(router, JoyButton::A, false);
	CHECK(input->is_action_pressed("test_shared")); // B still holds it.
	CHECK(shared_test_presses == 2);
	frame_button(router, JoyButton::B, false);
	CHECK_FALSE(input->is_action_pressed("test_shared"));

	end_pad_case(router, handles);
	root->disconnect(StringName("window_input"), counter);
	InputMap::get_singleton()->erase_action("test_shared");
}

// The engaged mask across a modifier: the stick held as it goes down stops and is eaten until it
// rests, then drives the layer. The layer binds only the stick's x axis, yet owns the whole stick
// while held: the base look on the stick slot stays quiet. Letting go of the modifier mid-push
// engages the stick again, so the look only resumes after the stick has rested.
TEST_CASE("[SceneTree][InputRouter] A modifier engages a held stick until it rests") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> look = test_stick("test_look", JoyButton::INVALID);
	const Ref<RouteBindingResource> aim = test_axis("test_aim", JoyAxis::LEFT_X, 0, RouteBindingResource::TRIGGER_ANALOG, JoyButton::LEFT_SHOULDER);
	set_test_map(router, { look, aim });
	LocalVector<int> handles;
	handles.push_back(router->attach(look, "test_context", phase_logger(4)));
	handles.push_back(router->attach(aim, "test_context", phase_logger(5)));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_axis(router, JoyAxis::LEFT_X, 0.8f);
	CHECK(logged(4, InputRouter::PHASE_CHANGED));
	CHECK_FALSE(logged_any(5));

	phase_log.clear();
	frame_button(router, JoyButton::LEFT_SHOULDER, true);
	CHECK(logged(4, InputRouter::PHASE_CHANGED)); // The look stops.
	CHECK_FALSE(logged_any(5));
	frame_axis(router, JoyAxis::LEFT_X, 0.85f);
	frame_axis(router, JoyAxis::LEFT_Y, 0.4f); // The other axis of the same stick: eaten too.
	CHECK_FALSE(logged_any(5));

	frame_axis(router, JoyAxis::LEFT_Y, 0.0f);
	frame_axis(router, JoyAxis::LEFT_X, 0.0f); // At rest: the mask clears.
	phase_log.clear();
	frame_axis(router, JoyAxis::LEFT_X, 0.6f);
	CHECK(logged(5, InputRouter::PHASE_CHANGED));
	CHECK_FALSE(logged_any(4));

	phase_log.clear();
	frame_button(router, JoyButton::LEFT_SHOULDER, false);
	CHECK(logged(5, InputRouter::PHASE_CHANGED)); // The aim stops.
	frame_axis(router, JoyAxis::LEFT_X, 0.65f);
	CHECK_FALSE(logged_any(4));

	frame_axis(router, JoyAxis::LEFT_X, 0.0f);
	phase_log.clear();
	frame_axis(router, JoyAxis::LEFT_X, 0.5f);
	CHECK(logged(4, InputRouter::PHASE_CHANGED));
	CHECK_FALSE(logged_any(5));

	frame_axis(router, JoyAxis::LEFT_X, 0.0f);
	end_pad_case(router, handles);
}

// Inputs already held when the modifier goes down (a stick direction, a whole stick, a trigger)
// never fire its press chords, however they jitter, until they return to rest.
TEST_CASE("[SceneTree][InputRouter] Inputs held before their modifier never fire its press chords") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> flick = test_axis("test_flick", JoyAxis::LEFT_Y, -1, RouteBindingResource::TRIGGER_PRESS, JoyButton::X);
	const Ref<RouteBindingResource> push = test_stick("test_push", JoyButton::X);
	push->set_stick(RouteBindingResource::STICK_RIGHT);
	push->set_trigger(RouteBindingResource::TRIGGER_PRESS);
	const Ref<RouteBindingResource> pull = test_axis("test_pull", JoyAxis::TRIGGER_RIGHT, 1, RouteBindingResource::TRIGGER_PRESS, JoyButton::X);
	set_test_map(router, { flick, push, pull });
	LocalVector<int> handles;
	handles.push_back(router->attach(flick, "test_context", phase_logger(6)));
	handles.push_back(router->attach(push, "test_context", phase_logger(10)));
	handles.push_back(router->attach(pull, "test_context", phase_logger(11)));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_axis(router, JoyAxis::LEFT_Y, -0.9f);
	frame_axis(router, JoyAxis::RIGHT_X, 0.9f);
	frame_axis(router, JoyAxis::TRIGGER_RIGHT, 0.9f);
	frame_button(router, JoyButton::X, true);
	frame_axis(router, JoyAxis::LEFT_Y, -0.95f);
	frame_axis(router, JoyAxis::RIGHT_X, 0.95f);
	frame_axis(router, JoyAxis::TRIGGER_RIGHT, 0.95f);
	CHECK_FALSE(logged_any(6));
	CHECK_FALSE(logged_any(10));
	CHECK_FALSE(logged_any(11));

	frame_axis(router, JoyAxis::LEFT_Y, 0.0f);
	frame_axis(router, JoyAxis::RIGHT_X, 0.0f);
	frame_axis(router, JoyAxis::TRIGGER_RIGHT, 0.0f);
	frame_axis(router, JoyAxis::LEFT_Y, -0.9f);
	frame_axis(router, JoyAxis::RIGHT_X, 0.9f);
	frame_axis(router, JoyAxis::TRIGGER_RIGHT, 0.9f);
	CHECK(logged(6, InputRouter::PHASE_PRESSED));
	CHECK(logged(10, InputRouter::PHASE_PRESSED));
	CHECK(logged(11, InputRouter::PHASE_PRESSED));

	frame_axis(router, JoyAxis::LEFT_Y, 0.0f);
	frame_axis(router, JoyAxis::RIGHT_X, 0.0f);
	frame_axis(router, JoyAxis::TRIGGER_RIGHT, 0.0f);
	frame_button(router, JoyButton::X, false);
	end_pad_case(router, handles);
}

// Activating the pad engages everything it holds off rest: a stick pushed while the keyboard owned
// the frame is eaten until it rests, so the pad's waking press cannot start a look.
TEST_CASE("[SceneTree][InputRouter] Activating a pad engages what it holds until it rests") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> look = test_stick("test_wake_look", JoyButton::INVALID);
	set_test_map(router, { look });
	LocalVector<int> handles;
	handles.push_back(router->attach(look, "test_context", phase_logger(12)));
	start_pad_case(router, PackedStringArray({ "test_context" }));
	router->set_device(InputRouter::DEVICE_KBM, -1);
	frame_idle(router);
	REQUIRE(router->get_device() == InputRouter::DEVICE_KBM);

	frame_axis(router, JoyAxis::LEFT_X, 0.8f); // Sticks never wake a device.
	frame_button(router, JoyButton::A, true); // Wakes the pad.
	REQUIRE(router->get_device() == InputRouter::DEVICE_PAD);
	phase_log.clear();
	frame_axis(router, JoyAxis::LEFT_X, 0.85f);
	CHECK_FALSE(logged_any(12));

	frame_axis(router, JoyAxis::LEFT_X, 0.0f);
	frame_axis(router, JoyAxis::LEFT_X, 0.5f);
	CHECK(logged(12, InputRouter::PHASE_CHANGED));

	frame_axis(router, JoyAxis::LEFT_X, 0.0f);
	frame_button(router, JoyButton::A, false);
	end_pad_case(router, handles);
}

static int repeat_test_presses = 0;

static void count_repeat_press(const Ref<InputEvent> &p_event) {
	const Ref<InputEventAction> a = p_event;
	if (a.is_valid() && a->get_action() == StringName("test_repeat") && a->is_pressed()) {
		repeat_test_presses++;
	}
}

// Bug 9: every REPEATED edge re-presses the action, so held D-pad focus navigation steps.
TEST_CASE("[SceneTree][InputRouter] A held repeat binding re-presses its action") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	Window *root = SceneTree::get_singleton()->get_root();
	const Callable counter = callable_mp_static(&count_repeat_press);
	root->connect(StringName("window_input"), counter);
	InputMap::get_singleton()->add_action("test_repeat");
	const Ref<RouteBindingResource> b = test_binding("test_repeat", JoyButton::DPAD_DOWN);
	b->set_trigger(RouteBindingResource::TRIGGER_REPEAT);
	b->set_repeat_delay(0.01f);
	b->set_repeat_interval(0.01f);
	set_test_map(router, { b });
	LocalVector<int> handles;
	handles.push_back(router->attach(b, "test_context"));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	repeat_test_presses = 0;
	frame_button(router, JoyButton::DPAD_DOWN, true);
	CHECK(repeat_test_presses == 1);
	for (int i = 0; i < 3; i++) {
		OS::get_singleton()->delay_usec(15000);
		frame_idle(router);
	}
	CHECK(repeat_test_presses == 4); // The press, then one repeat per frame: no burst, none missed.
	frame_button(router, JoyButton::DPAD_DOWN, false);
	CHECK_FALSE(Input::get_singleton()->is_action_pressed("test_repeat"));

	end_pad_case(router, handles);
	root->disconnect(StringName("window_input"), counter);
	InputMap::get_singleton()->erase_action("test_repeat");
}

static int clear_test_handle = 0;

static void clear_own_callback(int p_phase, const Variant &p_value) {
	phase_log.push_back(7 * 16 + p_phase);
	InputRouter::get_singleton()->clear_callback(clear_test_handle);
}

// One frame with several pad 0 buttons pressed, in order.
static void frame_buttons(InputRouter *p_router, const Vector<JoyButton> &p_buttons) {
	for (JoyButton button : p_buttons) {
		Ref<InputEventJoypadButton> ev;
		ev.instantiate();
		ev->set_device(0);
		ev->set_button_index(button);
		ev->set_pressed(true);
		Input::get_singleton()->parse_input_event(ev);
	}
	p_router->input_poll_process_flip();
}

// Bug 6: a callback clearing its own named callback must not shift the others queued that frame.
TEST_CASE("[SceneTree][InputRouter] Clearing a named callback inside a callback skips only that one") {
	InputRouter *router = InputRouter::get_singleton();
	REQUIRE(router != nullptr);
	const Ref<RouteBindingResource> x = test_binding("test_cb_x", JoyButton::X);
	const Ref<RouteBindingResource> y = test_binding("test_cb_y", JoyButton::Y);
	const Ref<RouteBindingResource> z = test_binding("test_cb_z", JoyButton::B);
	x->set_action(StringName());
	y->set_action(StringName());
	z->set_action(StringName());
	set_test_map(router, { x, y, z });
	LocalVector<int> handles;
	handles.push_back(router->attach(x, "test_context"));
	handles.push_back(router->attach(y, "test_context"));
	handles.push_back(router->attach(z, "test_context"));
	clear_test_handle = router->set_callback("test_context", "test_cb_x", callable_mp_static(&clear_own_callback));
	const int y_handle = router->set_callback("test_context", "test_cb_y", phase_logger(8));
	const int z_handle = router->set_callback("test_context", "test_cb_z", phase_logger(9));
	start_pad_case(router, PackedStringArray({ "test_context" }));

	frame_buttons(router, { JoyButton::X, JoyButton::Y, JoyButton::B });
	CHECK(logged(7, InputRouter::PHASE_PRESSED));
	CHECK(logged(8, InputRouter::PHASE_PRESSED));
	CHECK(logged(9, InputRouter::PHASE_PRESSED));

	phase_log.clear();
	frame_button(router, JoyButton::X, false);
	frame_button(router, JoyButton::X, true);
	CHECK_FALSE(logged_any(7)); // Cleared.

	router->clear_callback(y_handle);
	router->clear_callback(z_handle);
	end_pad_case(router, handles);
}

} // namespace TestInputRouter
