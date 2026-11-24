#include "input_router_internal.hpp"

#include "input_router.h"

namespace InputRouterInternal {

// Context, scheme and LUT slot are where a row lives, not what it does.
bool RouteBinding::same_tuning(const RouteBinding &p_other) const {
	RouteBinding a = *this;
	RouteBinding b = p_other;
	a.context = b.context = NONE;
	a.scheme_mask = b.scheme_mask = 0xFF;
	a.lut = b.lut = NO_LUT;
	return memcmp(&a, &b, sizeof(RouteBinding)) == 0;
}

static float lut_sample(const float *p_lut, float p_t) {
	const float x = CLAMP(p_t, 0.0f, 1.0f) * LUT_SIZE;
	const uint32_t i = MIN((uint32_t)x, LUT_SIZE - 1);
	return Math::lerp(p_lut[i], p_lut[i + 1], x - i);
}

// Deadzone and saturation, the curve over the span between them, then anti-deadzone. Out 0..1.
float shape_magnitude(const RouteBinding &p_row, const float *p_lut, float p_magnitude) {
	if (p_magnitude <= p_row.deadzone) {
		return 0.0f;
	}
	const float span = MAX(p_row.saturation - p_row.deadzone, 0.0001f);
	float t = CLAMP((p_magnitude - p_row.deadzone) / span, 0.0f, 1.0f);
	if (p_lut) {
		t = lut_sample(p_lut, t);
	}
	return p_row.anti_deadzone + (1.0f - p_row.anti_deadzone) * t;
}

// Half axes read 0..1 in their direction; the full axis keeps its sign.
float shape_axis(const RouteBinding &p_row, const float *p_lut, float p_raw, int p_direction) {
	if (p_direction != 0) {
		return shape_magnitude(p_row, p_lut, MAX(0.0f, p_raw * p_direction));
	}
	return SIGN(p_raw) * shape_magnitude(p_row, p_lut, Math::abs(p_raw));
}

// Radial keeps direction. Axial shapes each axis alone. Hybrid is radial, then drops a component
// under half the deadzone.
Vector2 shape_stick(const RouteBinding &p_row, const float *p_lut, const Vector2 &p_raw) {
	if (p_row.flags & ROW_DEADZONE_AXIAL) {
		return Vector2(SIGN(p_raw.x) * shape_magnitude(p_row, p_lut, Math::abs(p_raw.x)), SIGN(p_raw.y) * shape_magnitude(p_row, p_lut, Math::abs(p_raw.y)));
	}
	const float magnitude = p_raw.length();
	if (magnitude <= 0.0f) {
		return Vector2();
	}
	Vector2 shaped = p_raw / magnitude * shape_magnitude(p_row, p_lut, MIN(magnitude, 1.0f));
	if (p_row.flags & ROW_DEADZONE_HYBRID) {
		const float cut = p_row.deadzone * 0.5f;
		if (Math::abs(p_raw.x) < cut) {
			shaped.x = 0.0f;
		}
		if (Math::abs(p_raw.y) < cut) {
			shaped.y = 0.0f;
		}
	}
	return shaped;
}

// One trigger step, on every value change and every tick, so hold and repeat fire on time.
// Down at press, up below release.
uint32_t trigger_step(const RouteBinding &p_row, Claim &r_claim, float p_shaped, uint64_t p_now_usec) {
	uint32_t fire = FIRE_NONE;
	const float magnitude = Math::abs(p_shaped);
	const bool was_down = r_claim.bits & CLAIM_DOWN;
	const bool is_down = was_down ? magnitude >= p_row.release : magnitude >= p_row.press;

	if (is_down && !was_down) {
		r_claim.bits = (r_claim.bits | CLAIM_DOWN) & ~CLAIM_HOLD_FIRED;
		r_claim.down_usec = p_now_usec;
		r_claim.next_usec = p_now_usec + p_row.repeat_delay_usec;
		if (p_row.trigger == RouteBindingResource::TRIGGER_PRESS || p_row.trigger == RouteBindingResource::TRIGGER_REPEAT) {
			fire |= FIRE_PRESSED;
		}
	} else if (!is_down && was_down) {
		r_claim.bits &= ~CLAIM_DOWN;
		switch (p_row.trigger) {
			case RouteBindingResource::TRIGGER_PRESS:
			case RouteBindingResource::TRIGGER_REPEAT:
			case RouteBindingResource::TRIGGER_RELEASE:
				fire |= FIRE_RELEASED;
				break;
			case RouteBindingResource::TRIGGER_HOLD:
				if (r_claim.bits & CLAIM_HOLD_FIRED) {
					fire |= FIRE_RELEASED;
				}
				break;
			case RouteBindingResource::TRIGGER_TAP:
				if (p_now_usec - r_claim.down_usec < p_row.tap_usec) {
					fire |= FIRE_TAPPED;
				}
				break;
			default:
				break;
		}
	} else if (is_down) {
		if (p_row.trigger == RouteBindingResource::TRIGGER_HOLD && !(r_claim.bits & CLAIM_HOLD_FIRED) && p_now_usec - r_claim.down_usec >= p_row.hold_usec) {
			r_claim.bits |= CLAIM_HOLD_FIRED;
			fire |= FIRE_HELD;
		}
		if (p_row.trigger == RouteBindingResource::TRIGGER_REPEAT && p_now_usec >= r_claim.next_usec) {
			fire |= FIRE_REPEATED;
			r_claim.next_usec += p_row.repeat_interval_usec;
			if (r_claim.next_usec <= p_now_usec) {
				r_claim.next_usec = p_now_usec + p_row.repeat_interval_usec; // Late frame: no burst.
			}
		}
	}

	if (p_row.trigger == RouteBindingResource::TRIGGER_ANALOG && Math::abs(p_shaped - r_claim.value) > 0.0005f) {
		fire |= FIRE_CHANGED;
	}
	r_claim.value = p_shaped;
	return fire;
}

// The row's input, then sequence[0..3], each within p_timeout of the last. A wrong press abandons,
// and restarts if it is the input. p_pressed NONE: tick.
uint32_t sequence_step(const RouteBinding &p_row, uint8_t &r_step, uint64_t &r_until, uint8_t p_pressed, uint64_t p_now_usec, uint64_t p_timeout_usec) {
	uint32_t fire = FIRE_NONE;
	if (r_step > 0 && p_now_usec > r_until) {
		r_step = 0;
		fire |= FIRE_SEQUENCE_ABANDONED;
	}
	if (p_pressed == NONE) {
		return fire;
	}
	if (r_step > 0) {
		if (p_pressed == p_row.sequence[r_step - 1]) {
			r_step++;
			if (r_step > 4 || p_row.sequence[r_step - 1] == NONE) {
				r_step = 0;
				return fire | FIRE_PRESSED;
			}
			r_until = p_now_usec + p_timeout_usec;
			return fire;
		}
		r_step = 0;
		fire |= FIRE_SEQUENCE_ABANDONED;
	}
	if (p_pressed == p_row.input) {
		r_step = 1;
		r_until = p_now_usec + p_timeout_usec;
	}
	return fire;
}

// The curve over its own domain and range, normalised to 0..1.
static bool bake_curve(const Ref<Curve> &p_curve, float *r_lut) {
	if (p_curve.is_null() || p_curve->get_point_count() == 0) {
		return false;
	}
	const float domain_min = p_curve->get_min_domain();
	const float domain_span = p_curve->get_max_domain() - domain_min;
	const float value_min = p_curve->get_min_value();
	const float value_span = MAX(p_curve->get_max_value() - value_min, 0.0001f);
	for (uint32_t i = 0; i < LUT_STRIDE; i++) {
		const float t = (float)i / LUT_SIZE;
		r_lut[i] = CLAMP((p_curve->sample_baked(domain_min + t * domain_span) - value_min) / value_span, 0.0f, 1.0f);
	}
	return true;
}

// Resource to row, tuning clamped. Context, scheme_mask and lut are the caller's.
bool compile_binding(const RouteBindingResource &p_binding, RouteBinding &r_row, float *r_lut, bool &r_has_lut, String &r_error) {
	r_row = RouteBinding();
	r_has_lut = false;

	switch (p_binding.get_source()) {
		case RouteBindingResource::SOURCE_BUTTON:
			r_row.input = button_slot(p_binding.get_button());
			break;
		case RouteBindingResource::SOURCE_AXIS:
			r_row.input = axis_slot(p_binding.get_axis(), p_binding.get_axis_direction());
			break;
		case RouteBindingResource::SOURCE_STICK:
			r_row.input = stick_slot(p_binding.get_stick());
			break;
	}
	if (r_row.input == NONE) {
		r_error = "invalid input";
		return false;
	}

	if (p_binding.get_gate() != JoyButton::INVALID) {
		r_row.gate = button_slot(p_binding.get_gate());
		if (r_row.gate == NONE || r_row.gate == r_row.input) {
			r_error = "gate must be a different button";
			return false;
		}
	}

	r_row.trigger = (uint8_t)p_binding.get_trigger();
	if (p_binding.get_trigger() == RouteBindingResource::TRIGGER_SEQUENCE) {
		const PackedInt32Array seq = p_binding.get_sequence();
		if (p_binding.get_source() != RouteBindingResource::SOURCE_BUTTON || seq.is_empty() || seq.size() > 4) {
			r_error = "sequence needs a button input and 1-4 sequence buttons";
			return false;
		}
		for (int i = 0; i < seq.size(); i++) {
			r_row.sequence[i] = button_slot(JoyButton(seq[i]));
			if (r_row.sequence[i] == NONE) {
				r_error = "invalid sequence button";
				return false;
			}
		}
	}

	if (p_binding.is_pointer()) {
		if (p_binding.get_source() != RouteBindingResource::SOURCE_STICK) {
			r_error = "pointer needs a stick";
			return false;
		}
		r_row.flags |= ROW_POINTER;
	}
	if (p_binding.get_deadzone_shape() == RouteBindingResource::DEADZONE_AXIAL) {
		r_row.flags |= ROW_DEADZONE_AXIAL;
	} else if (p_binding.get_deadzone_shape() == RouteBindingResource::DEADZONE_HYBRID) {
		r_row.flags |= ROW_DEADZONE_HYBRID;
	}
	if (p_binding.is_lock_axis() && p_binding.get_source() == RouteBindingResource::SOURCE_AXIS && stick_of_axis(p_binding.get_axis()) >= 0) {
		r_row.flags |= ROW_LOCK_AXIS;
	}

	r_row.priority = (int8_t)CLAMP(p_binding.get_priority(), -128, 127);
	r_row.press = CLAMP(p_binding.get_press_threshold(), 0.01f, 1.0f);
	r_row.release = CLAMP(p_binding.get_release_threshold(), 0.0f, r_row.press);
	r_row.deadzone = CLAMP(p_binding.get_deadzone(), 0.0f, 0.95f);
	r_row.saturation = CLAMP(p_binding.get_saturation(), r_row.deadzone + 0.01f, 1.0f);
	r_row.anti_deadzone = CLAMP(p_binding.get_anti_deadzone(), 0.0f, 0.95f);
	r_row.pointer_speed = MAX(p_binding.get_pointer_speed(), 0.0f);
	r_row.hold_usec = (uint32_t)(MAX(p_binding.get_hold_time(), 0.01f) * 1000000.0f);
	r_row.tap_usec = (uint32_t)(MAX(p_binding.get_tap_time(), 0.01f) * 1000000.0f);
	r_row.repeat_delay_usec = (uint32_t)(MAX(p_binding.get_repeat_delay(), 0.01f) * 1000000.0f);
	r_row.repeat_interval_usec = (uint32_t)(MAX(p_binding.get_repeat_interval(), 0.01f) * 1000000.0f);
	r_row.authored_input = r_row.input;
	r_row.authored_gate = r_row.gate;

	r_has_lut = bake_curve(p_binding.get_response(), r_lut);
	return true;
}

// Positional: JoyButton::A is always south. Paddles follow SDL: P1 is the Deck's R4.
static const char *BUTTON_LABELS[InputRouter::PAD_FAMILY_MAX][BUTTON_COUNT] = {
	{ "A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Misc", "P1", "P2", "P3", "P4", "Touchpad" },
	{ "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS", "RS", "LB", "RB", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Share", "P1", "P2", "P3", "P4", "Touchpad" },
	{ "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mute", "Right Back", "Left Back", "Right Fn", "Left Fn", "Touchpad" },
	{ "B", "A", "Y", "X", "-", "Home", "+", "LS", "RS", "L", "R", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Capture", "SR (R)", "SL (L)", "SL (R)", "SR (L)", "Touchpad" },
	{ "A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "...", "R4", "L4", "R5", "L5", "Touchpad" },
};

static const char *TRIGGER_LABELS[InputRouter::PAD_FAMILY_MAX][2] = {
	{ "LT", "RT" },
	{ "LT", "RT" },
	{ "L2", "R2" },
	{ "ZL", "ZR" },
	{ "L2", "R2" },
};

static const char *STICK_LABELS[InputRouter::PAD_FAMILY_MAX][2] = {
	{ "LS", "RS" },
	{ "LS", "RS" },
	{ "L Stick", "R Stick" },
	{ "L Stick", "R Stick" },
	{ "L Stick", "R Stick" },
};

static const char *TRIGGER_NAMES[] = { "press", "release", "hold", "tap", "repeat", "analog", "sequence" };

String button_label(JoyButton p_button, int p_family) {
	const uint8_t b = button_slot(p_button);
	if (b == NONE) {
		return String();
	}
	return BUTTON_LABELS[CLAMP(p_family, 0, InputRouter::PAD_FAMILY_MAX - 1)][b];
}

// "RT", "LS Up", "LS X". Stick Y is negative up.
String axis_label(JoyAxis p_axis, int p_direction, int p_family) {
	const int family = CLAMP(p_family, 0, InputRouter::PAD_FAMILY_MAX - 1);
	if (p_axis == JoyAxis::TRIGGER_LEFT || p_axis == JoyAxis::TRIGGER_RIGHT) {
		return TRIGGER_LABELS[family][p_axis == JoyAxis::TRIGGER_RIGHT];
	}
	const int stick = stick_of_axis(p_axis);
	if (stick < 0) {
		return String();
	}
	const bool y = p_axis == JoyAxis::LEFT_Y || p_axis == JoyAxis::RIGHT_Y;
	const char *suffix = p_direction == 0 ? (y ? " Y" : " X") : (y ? (p_direction < 0 ? " Up" : " Down") : (p_direction < 0 ? " Left" : " Right"));
	return String(STICK_LABELS[family][stick]) + suffix;
}

String input_label(uint8_t p_slot, int p_family) {
	if (is_button_slot(p_slot)) {
		return button_label(JoyButton(p_slot), p_family);
	}
	if (is_axis_slot(p_slot)) {
		return axis_label(slot_axis(p_slot), slot_direction(p_slot), p_family);
	}
	if (is_stick_slot(p_slot)) {
		return STICK_LABELS[CLAMP(p_family, 0, InputRouter::PAD_FAMILY_MAX - 1)][p_slot - STICK_BASE];
	}
	return String();
}

const char *trigger_name(int p_trigger) {
	return TRIGGER_NAMES[CLAMP(p_trigger, 0, (int)RouteBindingResource::TRIGGER_SEQUENCE)];
}

// Anything that changes how rows are read. A mismatch means re-export.
PackedInt32Array Baked::caps() {
	PackedInt32Array c;
	c.push_back((int)sizeof(RouteBinding));
	c.push_back(INPUT_COUNT);
	c.push_back(MAX_CONTEXTS);
	c.push_back(MAX_BAKED);
	c.push_back(MAX_SCHEMES);
	c.push_back(MAX_LUTS);
	c.push_back(LUT_STRIDE);
	return c;
}

Dictionary Baked::empty() {
	Dictionary d;
	d[KEY_FORMAT] = FORMAT_VERSION;
	d[KEY_CAPS] = caps();
	d[KEY_SCHEMES] = PackedStringArray();
	d[KEY_CONTEXT_IDS] = PackedStringArray();
	d[KEY_CONTEXT_INFO] = PackedInt32Array();
	d[KEY_IDS] = PackedStringArray();
	d[KEY_ACTIONS] = PackedStringArray();
	d[KEY_LABELS] = PackedStringArray();
	d[KEY_ROWS] = PackedByteArray();
	d[KEY_LUTS] = PackedFloat32Array();
	return d;
}

// Format, caps, array sizes, every context's fallback, then every row's indices.
String Baked::validate(const Dictionary &p_data) {
	if ((int)p_data.get(KEY_FORMAT, 0) != FORMAT_VERSION) {
		return "format mismatch";
	}
	if (PackedInt32Array(p_data.get(KEY_CAPS, PackedInt32Array())) != caps()) {
		return "caps differ from this build";
	}
	const PackedStringArray schemes = p_data.get(KEY_SCHEMES, PackedStringArray());
	const PackedStringArray context_ids = p_data.get(KEY_CONTEXT_IDS, PackedStringArray());
	const PackedInt32Array context_info = p_data.get(KEY_CONTEXT_INFO, PackedInt32Array());
	const PackedStringArray ids = p_data.get(KEY_IDS, PackedStringArray());
	const PackedStringArray actions = p_data.get(KEY_ACTIONS, PackedStringArray());
	const PackedStringArray labels = p_data.get(KEY_LABELS, PackedStringArray());
	const PackedByteArray rows = p_data.get(KEY_ROWS, PackedByteArray());
	const PackedFloat32Array luts = p_data.get(KEY_LUTS, PackedFloat32Array());
	const int n = ids.size();
	const int lut_count = luts.size() / LUT_STRIDE;
	if (schemes.size() > (int)MAX_SCHEMES || context_ids.size() > (int)MAX_CONTEXTS || n > (int)MAX_BAKED || lut_count > (int)MAX_LUTS) {
		return "over a cap";
	}
	if (context_info.size() != context_ids.size() * 2 || actions.size() != n || labels.size() != n || rows.size() != n * (int)sizeof(RouteBinding) || luts.size() % LUT_STRIDE != 0) {
		return "array sizes disagree";
	}
	for (int i = 0; i < context_ids.size(); i++) {
		const int fallback = context_info[i * 2];
		if (fallback < GamepadContext::FALLBACK_INHERIT || fallback > GamepadContext::FALLBACK_PASS) {
			return vformat("context %d ('%s') has an invalid fallback", i, context_ids[i]);
		}
	}
	for (int i = 0; i < n; i++) {
		RouteBinding row;
		memcpy(&row, rows.ptr() + i * sizeof(RouteBinding), sizeof(RouteBinding));
		const bool valid = row.input < INPUT_COUNT && (row.gate == NONE || row.gate < BUTTON_COUNT) &&
				row.authored_input < INPUT_COUNT && (row.authored_gate == NONE || row.authored_gate < BUTTON_COUNT) &&
				row.trigger <= RouteBindingResource::TRIGGER_SEQUENCE && (row.lut == NO_LUT || row.lut < lut_count);
		if (!valid) {
			return vformat("row %d ('%s') is invalid", i, ids[i]);
		}
	}
	return String();
}

// Adds the id if absent; an existing entry keeps its fallback and modal.
bool Baked::add_context(Dictionary &r_data, const StringName &p_id, int p_fallback, bool p_modal) {
	PackedStringArray ids = r_data.get(KEY_CONTEXT_IDS, PackedStringArray());
	if (ids.has(p_id)) {
		return true;
	}
	if (ids.size() >= (int)MAX_CONTEXTS) {
		return false;
	}
	PackedInt32Array info = r_data.get(KEY_CONTEXT_INFO, PackedInt32Array());
	ids.push_back(p_id);
	info.push_back(p_fallback);
	info.push_back(p_modal ? 1 : 0);
	r_data[KEY_CONTEXT_IDS] = ids;
	r_data[KEY_CONTEXT_INFO] = info;
	return true;
}

// Compiles into the (id, scheme) template, a new one if absent. Same curve content shares one
// LUT. The binding's own context joins the context table.
Baked::Added Baked::add_binding(Dictionary &r_data, const RouteBindingResource &p_binding, String &r_error) {
	const String id = p_binding.get_id();
	if (id.is_empty()) {
		r_error = "no id";
		return ADD_FAILED;
	}
	RouteBinding row;
	float lut[LUT_STRIDE];
	bool has_lut = false;
	if (!compile_binding(p_binding, row, lut, has_lut, r_error)) {
		return ADD_FAILED;
	}

	PackedStringArray schemes = r_data.get(KEY_SCHEMES, PackedStringArray());
	int scheme_index = -1;
	if (p_binding.get_scheme() != StringName()) {
		scheme_index = schemes.find(p_binding.get_scheme());
		if (scheme_index < 0) {
			if (schemes.size() >= (int)MAX_SCHEMES) {
				r_error = vformat("more than %d schemes", MAX_SCHEMES);
				return ADD_FAILED;
			}
			schemes.push_back(p_binding.get_scheme());
			scheme_index = schemes.size() - 1;
		}
	}
	row.scheme_mask = scheme_index < 0 ? 0xFF : (uint8_t)(1 << scheme_index);

	PackedFloat32Array luts = r_data.get(KEY_LUTS, PackedFloat32Array());
	if (has_lut) {
		const int count = luts.size() / LUT_STRIDE;
		for (int i = 0; i < count && row.lut == NO_LUT; i++) {
			if (memcmp(luts.ptr() + i * LUT_STRIDE, lut, sizeof(float) * LUT_STRIDE) == 0) {
				row.lut = (uint16_t)i;
			}
		}
		if (row.lut == NO_LUT) {
			if (count >= (int)MAX_LUTS) {
				r_error = vformat("more than %d response curves", MAX_LUTS);
				return ADD_FAILED;
			}
			const int base = luts.size();
			luts.resize(base + LUT_STRIDE);
			memcpy(luts.ptrw() + base, lut, sizeof(float) * LUT_STRIDE);
			row.lut = (uint16_t)count;
		}
	}

	PackedStringArray ids = r_data.get(KEY_IDS, PackedStringArray());
	PackedStringArray actions = r_data.get(KEY_ACTIONS, PackedStringArray());
	PackedStringArray labels = r_data.get(KEY_LABELS, PackedStringArray());
	PackedByteArray rows = r_data.get(KEY_ROWS, PackedByteArray());
	int index = -1;
	for (int i = 0; i < ids.size() && index < 0; i++) {
		if (ids[i] == id && rows[i * sizeof(RouteBinding) + offsetof(RouteBinding, scheme_mask)] == row.scheme_mask) {
			index = i;
		}
	}
	const String action = p_binding.get_action();
	const String label = p_binding.get_label();
	Added added = ADD_NEW;
	if (index >= 0) {
		RouteBinding old;
		memcpy(&old, rows.ptr() + index * sizeof(RouteBinding), sizeof(RouteBinding));
		if (old.same_tuning(row) && old.lut == row.lut && actions[index] == action && labels[index] == label) {
			return ADD_SAME;
		}
		added = ADD_REPLACED;
		memcpy(rows.ptrw() + index * sizeof(RouteBinding), &row, sizeof(RouteBinding));
		actions.set(index, action);
		labels.set(index, label);
	} else {
		if (ids.size() >= (int)MAX_BAKED) {
			r_error = vformat("more than %d bindings", MAX_BAKED);
			return ADD_FAILED;
		}
		ids.push_back(id);
		actions.push_back(action);
		labels.push_back(label);
		const int base = rows.size();
		rows.resize(base + sizeof(RouteBinding));
		memcpy(rows.ptrw() + base, &row, sizeof(RouteBinding));
	}
	r_data[KEY_SCHEMES] = schemes;
	r_data[KEY_LUTS] = luts;
	r_data[KEY_IDS] = ids;
	r_data[KEY_ACTIONS] = actions;
	r_data[KEY_LABELS] = labels;
	r_data[KEY_ROWS] = rows;

	const Ref<GamepadContext> own = p_binding.get_context();
	if (own.is_valid() && own->get_id() != StringName()) {
		add_context(r_data, own->get_id(), own->get_fallback(), own->is_modal()); // A full table refuses; the baker reports it.
	}
	return added;
}

} // namespace InputRouterInternal
