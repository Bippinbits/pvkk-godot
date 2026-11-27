#ifdef TOOLS_ENABLED

#include "input_router_editor_plugin.h"

#include "input_router_export_plugin.h"

#include "../input_router.h"

#include "core/core_string_names.h"
#include "core/input/input.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/tree.h"

using namespace InputRouterInternal;

static const char *FAMILY_NAMES[] = { "Xbox", "PlayStation", "Nintendo", "Steam Deck" };
static const int FAMILIES[] = { InputRouter::PAD_FAMILY_XBOX, InputRouter::PAD_FAMILY_PLAYSTATION, InputRouter::PAD_FAMILY_NINTENDO, InputRouter::PAD_FAMILY_STEAM };
static const char *PRESET_NAMES[] = { "Linear", "Smooth", "Precise", "Aggressive" };
static constexpr float GLYPH_SCALE = 3.2f; // Over label text: glyphs stand about 2/3 em tall.
static constexpr float PREVIEW_GLYPH_SCALE = 5.0f;

// The project's glyph map (input_devices/router/glyph_map), or null.
static Ref<GlyphFontMap> glyph_map() {
	InputRouter *router = InputRouter::get_singleton();
	return router ? router->get_glyph_map() : Ref<GlyphFontMap>();
}

// Gate then input glyph for p_family; empty when either has none.
static String binding_glyphs(const GlyphFontMap &p_map, uint8_t p_input, uint8_t p_gate, int p_family) {
	const String input = p_input == NONE ? String() : p_map.get_input_glyph(p_input, p_family);
	if (p_gate == NONE || input.is_empty()) {
		return input;
	}
	const String gate = p_map.get_input_glyph(p_gate, p_family);
	return gate.is_empty() ? String() : gate + input;
}

// A compiled row's glyphs: gate and input, or the sequence's steps. Empty when one has none.
static String row_glyphs(const GlyphFontMap &p_map, const RouteBinding &p_row, int p_family) {
	if (p_row.trigger != RouteBindingResource::TRIGGER_SEQUENCE) {
		return binding_glyphs(p_map, p_row.input, p_row.gate, p_family);
	}
	String steps;
	for (uint8_t step : p_row.sequence) {
		if (step == NONE) {
			break;
		}
		const String glyph = p_map.get_input_glyph(step, p_family);
		if (glyph.is_empty()) {
			return String();
		}
		steps += glyph;
	}
	return steps;
}

// The family the editor shows: the first connected pad's, else generic.
static int editor_family() {
	const TypedArray<int> pads = Input::get_singleton()->get_connected_joypads();
	return InputRouter::detect_pad_family(pads.is_empty() ? -1 : (int)pads[0]);
}

static String fire_names(uint32_t p_bits) {
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = { { FIRE_PRESSED, "PRESSED" }, { FIRE_HELD, "HELD" }, { FIRE_TAPPED, "TAPPED" }, { FIRE_REPEATED, "REPEATED" }, { FIRE_CHANGED, "CHANGED" }, { FIRE_RELEASED, "RELEASED" } };
	String result;
	for (const auto &n : names) {
		if (p_bits & n.bit) {
			result += result.is_empty() ? n.name : String(" ") + n.name;
		}
	}
	return result;
}

// p_x right, p_y up, both 0..1 across p_graph.
static Vector2 graph_point(const Rect2 &p_graph, float p_x, float p_y) {
	return Vector2(p_graph.position.x + p_x * p_graph.size.x, p_graph.position.y + (1.0f - p_y) * p_graph.size.y);
}

static void add_cell(Node *p_grid, const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_grid->add_child(label);
}

// The glyph in p_font, its name as the tooltip. The name as text without a glyph or font.
static void add_glyph_cell(Node *p_grid, const String &p_glyph, const String &p_text, const Ref<Font> &p_font, int p_size) {
	if (p_glyph.is_empty() || p_font.is_null()) {
		add_cell(p_grid, p_text);
		return;
	}
	Label *glyph = memnew(Label);
	glyph->set_text(p_glyph);
	glyph->set_tooltip_text(p_text);
	glyph->set_mouse_filter(Control::MOUSE_FILTER_PASS);
	glyph->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	glyph->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	glyph->add_theme_font_override(SceneStringName(font), p_font);
	glyph->add_theme_font_size_override(SceneStringName(font_size), p_size);
	p_grid->add_child(glyph);
}

// Do sets p_value, undo restores the current one.
static void add_property_step(EditorUndoRedoManager *p_ur, Object *p_target, const StringName &p_property, const Variant &p_value) {
	p_ur->add_do_property(p_target, p_property, p_value);
	p_ur->add_undo_property(p_target, p_property, p_target->get(p_property));
}

// --- InputRouterPadPreview ---

InputRouterPadPreview::InputRouterPadPreview() {
	set_custom_minimum_size(Size2(0, 170) * EDSCALE);
}

void InputRouterPadPreview::set_binding(const Ref<RouteBindingResource> &p_binding) {
	const Callable on_changed = callable_mp(this, &InputRouterPadPreview::_compile);
	if (binding.is_valid() && binding->is_connected(CoreStringName(changed), on_changed)) {
		binding->disconnect(CoreStringName(changed), on_changed);
	}
	binding = p_binding;
	if (binding.is_valid()) {
		binding->connect(CoreStringName(changed), on_changed);
	}
	_compile();
}

void InputRouterPadPreview::set_pad(int p_pad) {
	pad = p_pad;
	claim = Claim();
	queue_redraw();
}

int InputRouterPadPreview::get_live_pad() const {
	if (pad >= 0) {
		return pad;
	}
	const TypedArray<int> connected = Input::get_singleton()->get_connected_joypads();
	return connected.is_empty() ? -1 : (int)connected[0];
}

// The router's compile step, so the preview shapes as the game does.
void InputRouterPadPreview::_compile() {
	error = String();
	valid = binding.is_valid() && compile_binding(**binding, row, lut, has_lut, error);
	claim = Claim();
	queue_redraw();
}

// Stock Input, shaped and stepped through the trigger. A gate that is not held zeroes the value.
void InputRouterPadPreview::_poll() {
	const int p = get_live_pad();
	if (!valid || p < 0) {
		return;
	}
	Input *input = Input::get_singleton();
	const float *l = has_lut ? lut : nullptr;
	float new_raw = 0.0f;
	float new_shaped = 0.0f;
	Vector2 new_stick;
	Vector2 new_shaped_stick;
	if (is_button_slot(row.input)) {
		new_raw = input->is_joy_button_pressed(p, JoyButton(row.input)) ? 1.0f : 0.0f;
		new_shaped = new_raw;
	} else if (is_axis_slot(row.input)) {
		new_raw = input->get_joy_axis(p, slot_axis(row.input));
		new_shaped = shape_axis(row, l, new_raw, slot_direction(row.input));
	} else {
		const int stick = row.input - STICK_BASE;
		new_stick = Vector2(input->get_joy_axis(p, JoyAxis(stick * 2)), input->get_joy_axis(p, JoyAxis(stick * 2 + 1)));
		new_shaped_stick = shape_stick(row, l, new_stick);
		new_raw = MIN(new_stick.length(), 1.0f);
		new_shaped = new_shaped_stick.length();
	}
	if (row.gate != NONE && !input->is_joy_button_pressed(p, JoyButton(row.gate))) {
		new_shaped = 0.0f;
		new_shaped_stick = Vector2();
	}

	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const uint32_t bits = row.trigger == RouteBindingResource::TRIGGER_SEQUENCE ? 0 : trigger_step(row, claim, new_shaped, now);
	bool changed = bits || (claim.bits & CLAIM_DOWN) || new_raw != raw || new_shaped != shaped || new_stick != raw_stick;
	if (bits) {
		fired = fire_names(bits);
		fired_usec = now;
	} else if (!fired.is_empty() && now - fired_usec > 1000000) {
		fired = String();
		changed = true;
	}
	raw = new_raw;
	shaped = new_shaped;
	raw_stick = new_stick;
	shaped_stick = new_shaped_stick;
	if (changed) {
		queue_redraw();
	}
}

// Output over raw magnitude: deadzone, saturation, thresholds, the curve and the live input.
void InputRouterPadPreview::_draw_response(const Rect2 &p_rect) {
	const Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
	const int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	const Color text = get_theme_color(SNAME("font_color"), SNAME("Label"));
	const Color dim(text.r, text.g, text.b, 0.3f);
	draw_rect(p_rect, Color(0, 0, 0, 0.2f));
	if (!valid) {
		draw_string(font, p_rect.position + Vector2(6, font_size + 4) * EDSCALE, error.is_empty() ? String("No binding.") : error, HORIZONTAL_ALIGNMENT_LEFT, p_rect.size.x - 12 * EDSCALE, font_size, Color(1, 0.45f, 0.45f));
		return;
	}
	Rect2 g = p_rect.grow(-8 * EDSCALE);
	g.size.y -= font_size + 6 * EDSCALE;
	const float *l = has_lut ? lut : nullptr;

	draw_rect(Rect2(g.position, Vector2(g.size.x, g.size.y)), dim, false);
	if (!is_button_slot(row.input)) {
		draw_rect(Rect2(g.position, Vector2(row.deadzone * g.size.x, g.size.y)), Color(1, 0.3f, 0.3f, 0.15f));
		draw_line(graph_point(g, row.saturation, 0), graph_point(g, row.saturation, 1), dim);
		draw_dashed_line(graph_point(g, 0, row.press), graph_point(g, 1, row.press), Color(1, 0.8f, 0.2f, 0.7f));
		draw_dashed_line(graph_point(g, 0, row.release), graph_point(g, 1, row.release), Color(0.4f, 0.8f, 1, 0.7f));
		PackedVector2Array curve;
		for (int i = 0; i <= 64; i++) {
			const float x = i / 64.0f;
			curve.push_back(graph_point(g, x, shape_magnitude(row, l, x)));
		}
		draw_polyline(curve, Color(0.45f, 0.75f, 1), 2 * EDSCALE, true);
		const float magnitude = MIN(Math::abs(raw), 1.0f);
		draw_circle(graph_point(g, magnitude, shape_magnitude(row, l, magnitude)), 4 * EDSCALE, Color(1, 1, 1));
	} else {
		draw_rect(Rect2(graph_point(g, 0, raw), Vector2(g.size.x, raw * g.size.y)), Color(0.45f, 0.75f, 1, 0.5f));
	}

	String line = vformat("raw %.2f  out %.2f", raw, shaped);
	if (claim.bits & CLAIM_DOWN) {
		line += vformat("  down %.2fs", (OS::get_singleton()->get_ticks_usec() - claim.down_usec) / 1000000.0);
	}
	if (!fired.is_empty()) {
		line += "  would fire: " + fired;
	}
	draw_string(font, Vector2(p_rect.position.x + 6 * EDSCALE, p_rect.get_end().y - 6 * EDSCALE), line, HORIZONTAL_ALIGNMENT_LEFT, p_rect.size.x - 12 * EDSCALE, font_size, text);
}

// Pad name and the binding's glyphs in that pad's family, then the stick with its rings, or raw
// and shaped bars for an axis or button.
void InputRouterPadPreview::_draw_stick(const Rect2 &p_rect) {
	const Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
	const int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	const Color text = get_theme_color(SNAME("font_color"), SNAME("Label"));
	const Color dim(text.r, text.g, text.b, 0.3f);
	const int p = get_live_pad();
	String title = "No pad connected.";
	if (p >= 0) {
		Input *input = Input::get_singleton();
		const String sdl_type = input->get_joy_info(p).get("sdl_type", String());
		title = vformat("%d: %s (%s)", p, input->get_joy_name(p), sdl_type.is_empty() ? String("generic") : sdl_type);
	}
	draw_string(font, p_rect.position + Vector2(0, font_size), title, HORIZONTAL_ALIGNMENT_LEFT, p_rect.size.x, font_size, text);
	if (!valid) {
		return;
	}
	const Ref<GlyphFontMap> map = glyph_map();
	const Ref<Font> glyph_font = map.is_valid() ? map->get_default_font() : Ref<Font>();
	if (glyph_font.is_valid()) {
		const int glyph_size = (int)(font_size * PREVIEW_GLYPH_SCALE);
		const String glyphs = row_glyphs(**map, row, InputRouter::detect_pad_family(p));
		draw_string(glyph_font, p_rect.position + Vector2(0, font_size + glyph_size), glyphs, HORIZONTAL_ALIGNMENT_RIGHT, p_rect.size.x, glyph_size, text);
	}
	const Rect2 area(p_rect.position + Vector2(0, font_size + 6 * EDSCALE), p_rect.size - Vector2(0, font_size + 6 * EDSCALE));
	if (is_stick_slot(row.input)) {
		const float radius = MIN(area.size.x, area.size.y) * 0.5f - 4 * EDSCALE;
		const Vector2 center = area.get_center();
		const float tau = (float)Math::TAU;
		draw_arc(center, radius, 0, tau, 48, dim, 1.5f * EDSCALE);
		draw_arc(center, radius * row.deadzone, 0, tau, 32, Color(1, 0.3f, 0.3f, 0.6f), 1 * EDSCALE);
		draw_arc(center, radius * row.saturation, 0, tau, 48, dim, 1 * EDSCALE);
		const Vector2 r = center + raw_stick.limit_length(1.0f) * radius;
		draw_line(r - Vector2(4, 4) * EDSCALE, r + Vector2(4, 4) * EDSCALE, text);
		draw_line(r - Vector2(4, -4) * EDSCALE, r + Vector2(4, -4) * EDSCALE, text);
		draw_circle(center + shaped_stick * radius, 4 * EDSCALE, Color(0.45f, 0.75f, 1));
	} else {
		const float w = 14 * EDSCALE;
		const float h = area.size.y - 4 * EDSCALE;
		const Vector2 base(area.position.x + area.size.x * 0.5f - w * 1.5f, area.get_end().y - 2 * EDSCALE);
		draw_rect(Rect2(base - Vector2(0, h), Vector2(w, h)), dim, false);
		draw_rect(Rect2(base - Vector2(0, h * MIN(Math::abs(raw), 1.0f)), Vector2(w, h * MIN(Math::abs(raw), 1.0f))), dim);
		const Vector2 base2 = base + Vector2(w * 2, 0);
		draw_rect(Rect2(base2 - Vector2(0, h), Vector2(w, h)), dim, false);
		draw_rect(Rect2(base2 - Vector2(0, h * MIN(Math::abs(shaped), 1.0f)), Vector2(w, h * MIN(Math::abs(shaped), 1.0f))), Color(0.45f, 0.75f, 1));
	}
}

void InputRouterPadPreview::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_VISIBILITY_CHANGED:
			set_process_internal(is_visible_in_tree());
			break;
		case NOTIFICATION_INTERNAL_PROCESS:
			_poll();
			break;
		case NOTIFICATION_DRAW: {
			const float split = get_size().x * 0.62f;
			_draw_response(Rect2(Point2(), Size2(split - 8 * EDSCALE, get_size().y)));
			_draw_stick(Rect2(Point2(split, 0), Size2(get_size().x - split, get_size().y)));
		} break;
	}
}

// --- InputRouterBindingPanel ---

InputRouterBindingPanel::InputRouterBindingPanel() {
	summary = memnew(Label);
	add_child(summary);

	labels = memnew(GridContainer);
	labels->set_columns(5);
	add_child(labels);

	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);
	listen = memnew(Button);
	listen->set_text("Listen");
	listen->set_toggle_mode(true);
	listen->set_tooltip_text("Press an input on the pad. Hold a button first to make it the gate of a chord.");
	listen->connect(SNAME("toggled"), callable_mp(this, &InputRouterBindingPanel::_listen_toggled));
	row->add_child(listen);
	for (int i = 0; i < 4; i++) {
		Button *preset = memnew(Button);
		preset->set_text(PRESET_NAMES[i]);
		preset->set_tooltip_text("Replace the response curve with this preset (undoable).");
		preset->connect(SNAME("pressed"), callable_mp(this, &InputRouterBindingPanel::_preset).bind(i));
		row->add_child(preset);
	}
	pads = memnew(OptionButton);
	pads->set_h_size_flags(SIZE_EXPAND_FILL);
	pads->connect(SNAME("item_selected"), callable_mp(this, &InputRouterBindingPanel::_pad_selected));
	row->add_child(pads);

	preview = memnew(InputRouterPadPreview);
	add_child(preview);
}

// The preview connects first, so it recompiles before _update() reads its input.
void InputRouterBindingPanel::set_binding(const Ref<RouteBindingResource> &p_binding) {
	binding = p_binding;
	preview->set_binding(binding);
	if (binding.is_valid()) {
		binding->connect(CoreStringName(changed), callable_mp(this, &InputRouterBindingPanel::_update));
	}
	_refresh_pads();
	_update();
}

// Summary, then the input and gate as each pad family shows and names them.
void InputRouterBindingPanel::_update() {
	if (binding.is_null()) {
		return;
	}
	summary->set_text(binding->describe() + (binding->get_id() == StringName() ? String("   [no id: the router refuses it]") : vformat("   id: %s", binding->get_id())));
	while (labels->get_child_count() > 0) {
		Node *child = labels->get_child(0);
		labels->remove_child(child);
		memdelete(child);
	}
	const Ref<GlyphFontMap> map = glyph_map();
	const Ref<Font> glyph_font = map.is_valid() ? map->get_default_font() : Ref<Font>();
	const int glyph_size = (int)(get_theme_font_size(SNAME("font_size"), SNAME("Label")) * GLYPH_SCALE);
	add_cell(labels, String());
	for (const char *name : FAMILY_NAMES) {
		add_cell(labels, name);
	}
	const uint8_t slot = preview->get_input();
	add_cell(labels, "Input");
	for (int family : FAMILIES) {
		const String glyph = map.is_valid() ? binding_glyphs(**map, slot, NONE, family) : String();
		add_glyph_cell(labels, glyph, slot == NONE ? String("?") : input_label(slot, family), glyph_font, glyph_size);
	}
	if (binding->get_gate() != JoyButton::INVALID) {
		const uint8_t gate = button_slot(binding->get_gate());
		add_cell(labels, "Gate");
		for (int family : FAMILIES) {
			const String glyph = map.is_valid() ? binding_glyphs(**map, gate, NONE, family) : String();
			add_glyph_cell(labels, glyph, button_label(binding->get_gate(), family), glyph_font, glyph_size);
		}
	}
}

void InputRouterBindingPanel::_refresh_pads() {
	pads->clear();
	pads->add_item("First connected pad", -1);
	const TypedArray<int> connected = Input::get_singleton()->get_connected_joypads();
	for (int i = 0; i < connected.size(); i++) {
		const int id = connected[i];
		pads->add_item(vformat("%d: %s", id, Input::get_singleton()->get_joy_name(id)), id);
	}
	pads->select(0);
	preview->set_pad(-1);
}

void InputRouterBindingPanel::_pad_selected(int p_index) {
	preview->set_pad(pads->get_item_id(p_index));
}

void InputRouterBindingPanel::_listen_toggled(bool p_on) {
	if (!p_on) {
		set_process_internal(false);
		return;
	}
	const int p = preview->get_live_pad();
	if (p < 0 || binding.is_null()) {
		listen->set_pressed_no_signal(false);
		return;
	}
	Input *input = Input::get_singleton();
	listen_start = 0;
	for (uint8_t b = 0; b < BUTTON_COUNT; b++) {
		if (input->is_joy_button_pressed(p, JoyButton(b))) {
			listen_start |= 1u << b;
		}
	}
	for (uint8_t a = 0; a < AXIS_COUNT; a++) {
		listen_axes[a] = input->get_joy_axis(p, JoyAxis(a));
	}
	listen_prev = listen_start;
	listen_first = -1;
	set_process_internal(true);
}

// A press released alone is the input; a second press while it is held makes it the gate. An
// axis past 0.6 is the input. Anything held when listening began is ignored until released.
void InputRouterBindingPanel::_poll_listen() {
	const int p = preview->get_live_pad();
	if (p < 0) {
		return;
	}
	Input *input = Input::get_singleton();
	uint32_t now = 0;
	for (uint8_t b = 0; b < BUTTON_COUNT; b++) {
		if (input->is_joy_button_pressed(p, JoyButton(b))) {
			now |= 1u << b;
		}
	}
	const uint32_t fresh = now & ~listen_prev & ~listen_start;
	for (uint8_t b = 0; b < BUTTON_COUNT; b++) {
		if (!(fresh & (1u << b))) {
			continue;
		}
		if (listen_first < 0) {
			listen_first = b;
		} else if (b != listen_first) {
			_commit_input(RouteBindingResource::SOURCE_BUTTON, b, 0, listen_first);
			return;
		}
	}
	if (listen_first >= 0 && !(now & (1u << listen_first))) {
		_commit_input(RouteBindingResource::SOURCE_BUTTON, listen_first, 0, -1);
		return;
	}
	for (uint8_t a = 0; a < AXIS_COUNT; a++) {
		const float v = input->get_joy_axis(p, JoyAxis(a));
		if (Math::abs(v) < 0.6f || Math::abs(listen_axes[a]) >= 0.3f) {
			continue;
		}
		const int stick = stick_of_axis(JoyAxis(a));
		if (binding->get_source() == RouteBindingResource::SOURCE_STICK && stick >= 0) {
			_commit_input(RouteBindingResource::SOURCE_STICK, stick, 0, listen_first);
		} else {
			_commit_input(RouteBindingResource::SOURCE_AXIS, a, stick < 0 ? 1 : (v < 0.0f ? -1 : 1), listen_first);
		}
		return;
	}
	listen_prev = now;
	listen_start &= now;
}

// One undo step for source, index, direction and gate.
void InputRouterBindingPanel::_commit_input(RouteBindingResource::Source p_source, int p_index, int p_direction, int p_gate) {
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	Object *target = binding.ptr();
	ur->create_action("Set Gamepad Input", UndoRedo::MERGE_DISABLE, target);
	add_property_step(ur, target, "source", p_source);
	if (p_source == RouteBindingResource::SOURCE_BUTTON) {
		add_property_step(ur, target, "button", p_index);
	} else if (p_source == RouteBindingResource::SOURCE_AXIS) {
		add_property_step(ur, target, "axis", p_index);
		add_property_step(ur, target, "axis_direction", p_direction);
	} else {
		add_property_step(ur, target, "stick", p_index);
	}
	add_property_step(ur, target, "gate", p_gate);
	ur->commit_action();
	listen->set_pressed(false);
}

void InputRouterBindingPanel::_preset(int p_preset) {
	if (binding.is_null()) {
		return;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action("Set Response Preset", UndoRedo::MERGE_DISABLE, binding.ptr());
	ur->add_do_property(binding.ptr(), "response", RouteBindingResource::make_response_curve(RouteBindingResource::ResponsePreset(p_preset)));
	ur->add_undo_property(binding.ptr(), "response", binding->get_response());
	ur->commit_action();
}

void InputRouterBindingPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			Input::get_singleton()->connect(SNAME("joy_connection_changed"), callable_mp(this, &InputRouterBindingPanel::_refresh_pads).unbind(2));
			break;
		case NOTIFICATION_EXIT_TREE:
			Input::get_singleton()->disconnect(SNAME("joy_connection_changed"), callable_mp(this, &InputRouterBindingPanel::_refresh_pads).unbind(2));
			break;
		case NOTIFICATION_THEME_CHANGED:
			_update();
			break;
		case NOTIFICATION_INTERNAL_PROCESS:
			if (listen->is_pressed()) {
				_poll_listen();
			}
			break;
	}
}

// --- InputRouterMapPanel ---

InputRouterMapPanel::InputRouterMapPanel() {
	tree = memnew(Tree);
	tree->set_columns(5);
	tree->set_column_titles_visible(true);
	tree->set_column_title(0, "Id");
	tree->set_column_title(1, "Binding");
	tree->set_column_title(2, "Pads");
	tree->set_column_title(3, "Action");
	tree->set_column_title(4, "Issues");
	tree->set_hide_root(true);
	tree->set_custom_minimum_size(Size2(0, 160) * EDSCALE);
	add_child(tree);
}

void InputRouterMapPanel::set_map(const Ref<GamepadMap> &p_map) {
	map = p_map;
	if (map.is_valid()) {
		map->connect(CoreStringName(changed), callable_mp(this, &InputRouterMapPanel::_update));
	}
	_update();
}

// Each family's gate and input glyphs, Xbox to Steam Deck. Empty if the binding cannot compile.
static String family_glyphs(const GlyphFontMap &p_map, const RouteBindingResource &p_binding) {
	RouteBinding row;
	float lut[LUT_STRIDE];
	bool has_lut = false;
	String error;
	if (!compile_binding(p_binding, row, lut, has_lut, error)) {
		return String();
	}
	PackedStringArray parts;
	for (int family : FAMILIES) {
		parts.push_back(row_glyphs(p_map, row, family));
	}
	return String("  ").join(parts);
}

void InputRouterMapPanel::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		_update();
	}
}

// One row per binding: its glyphs per family and the check_maps() findings for this map alone.
void InputRouterMapPanel::_update() {
	tree->clear();
	if (map.is_null()) {
		return;
	}
	const Ref<GlyphFontMap> glyphs = glyph_map();
	const Ref<Font> glyph_font = glyphs.is_valid() ? glyphs->get_default_font() : Ref<Font>();
	const int glyph_size = (int)(get_theme_font_size(SNAME("font_size"), SNAME("Tree")) * GLYPH_SCALE);
	TreeItem *root = tree->create_item();
	TypedArray<GamepadContext> contexts;
	if (map->get_context().is_valid()) {
		contexts.push_back(map->get_context());
	}
	const TypedArray<RouteBindingResource> bindings = map->get_bindings();
	for (int i = 0; i < bindings.size(); i++) {
		const Ref<RouteBindingResource> b = bindings[i];
		if (b.is_valid() && b->get_context().is_valid()) {
			contexts.push_back(b->get_context());
		}
	}
	TypedArray<GamepadMap> maps;
	maps.push_back(map);
	const TypedArray<Dictionary> findings = check_maps(contexts, maps, Array(), TypedArray<PackedStringArray>());

	for (int i = 0; i < bindings.size(); i++) {
		const Ref<RouteBindingResource> b = bindings[i];
		if (b.is_null()) {
			continue;
		}
		TreeItem *item = tree->create_item(root);
		const String id = b->get_id();
		item->set_text(0, id);
		item->set_text(1, b->describe());
		if (glyph_font.is_valid()) {
			item->set_text(2, family_glyphs(**glyphs, **b));
			item->set_custom_font(2, glyph_font);
			item->set_custom_font_size(2, glyph_size);
		}
		item->set_text(3, b->get_action());
		String issues;
		bool error = false;
		for (int f = 0; f < findings.size(); f++) {
			const Dictionary d = findings[f];
			if (String(d["a"]) == id || String(d["b"]) == id) {
				issues += (issues.is_empty() ? String() : String("; ")) + String(d["message"]);
				error = error || String(d["severity"]) == "error";
			}
		}
		item->set_text(4, issues);
		if (error) {
			item->set_custom_color(4, Color(1, 0.45f, 0.45f));
		}
	}
}

// --- Plugins ---

bool InputRouterInspectorPlugin::can_handle(Object *p_object) {
	return Object::cast_to<RouteBindingResource>(p_object) || Object::cast_to<GamepadMap>(p_object);
}

void InputRouterInspectorPlugin::parse_begin(Object *p_object) {
	if (RouteBindingResource *binding = Object::cast_to<RouteBindingResource>(p_object)) {
		InputRouterBindingPanel *panel = memnew(InputRouterBindingPanel);
		panel->set_binding(Ref<RouteBindingResource>(binding));
		add_custom_control(panel);
	} else if (GamepadMap *map = Object::cast_to<GamepadMap>(p_object)) {
		InputRouterMapPanel *panel = memnew(InputRouterMapPanel);
		panel->set_map(Ref<GamepadMap>(map));
		add_custom_control(panel);
	}
}

// The editor font with the glyph font behind it, so one label mixes words and glyphs. Rebuilt
// when the editor theme or the glyph map's font changes.
Ref<Font> InputRouterEditorPlugin::get_label_font(const Ref<Font> &p_glyphs) {
	const Ref<Font> base = EditorNode::get_singleton()->get_editor_theme()->get_font(SNAME("main"), EditorStringName(EditorFonts));
	if (p_glyphs.is_null()) {
		return base;
	}
	if (label_font.is_null() || label_font->get_base_font() != base || label_glyphs != p_glyphs) {
		label_font.instantiate();
		label_font->set_base_font(base);
		TypedArray<Font> fallbacks;
		fallbacks.push_back(p_glyphs);
		label_font->set_fallbacks(fallbacks);
		label_glyphs = p_glyphs;
	}
	return label_font;
}

// EditorResourcePicker label func: a collapsed RouteBindingResource row reads gate and input
// glyphs in the editor family, the trigger and the id. Without glyphs, describe() and the id.
bool InputRouterEditorPlugin::resource_label(const Ref<Resource> &p_resource, String &r_text, Ref<Font> &r_font) {
	const RouteBindingResource *binding = Object::cast_to<RouteBindingResource>(p_resource.ptr());
	if (!binding) {
		return false;
	}
	const Ref<GlyphFontMap> map = glyph_map();
	RouteBinding row;
	float lut[LUT_STRIDE];
	bool has_lut = false;
	String error;
	String glyphs;
	if (singleton && map.is_valid() && compile_binding(*binding, row, lut, has_lut, error)) {
		glyphs = row_glyphs(**map, row, editor_family());
	}
	if (glyphs.is_empty()) {
		r_text = binding->describe();
	} else {
		r_text = glyphs + "  " + trigger_name(row.trigger);
		r_font = singleton->get_label_font(map->get_default_font());
	}
	if (binding->get_id() != StringName()) {
		r_text += "   " + String(binding->get_id());
	}
	return true;
}

InputRouterEditorPlugin::InputRouterEditorPlugin() {
	singleton = this;
	Ref<InputRouterInspectorPlugin> inspector;
	inspector.instantiate();
	add_inspector_plugin(inspector);
	Ref<InputRouterExportPlugin> exporter;
	exporter.instantiate();
	add_export_plugin(exporter);
}

InputRouterEditorPlugin::~InputRouterEditorPlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

#endif // TOOLS_ENABLED
