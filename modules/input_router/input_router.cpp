#include "input_router.h"

#include "input_router_internal.hpp"
#include "line_vector.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/fixed_vector.h"
#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "core/templates/safe_refcount.h"

#include <atomic>

#include "bit_pack.h" // Last: its macros (ALIGN, PACK, FFS...) stay out of other headers.

typedef InputRouter IR;

// Rows: one 64 B RouteBinding per attached binding, a 64 B RouteBindingInfo beside it, both on
// line-aligned storage. A dead row is tombstoned (input NONE) and reused, so row ids never move.
// Routes: live rows of routable contexts sorted by slot into `active`; slot s routes
// active[slot_first[s] .. slot_first[s + 1]). One Claim per slot of the active pad.
namespace InputRouterInternal {

constexpr uint32_t MAX_ACTIVE = 255; // u8 slot offsets.
constexpr uint32_t MAX_OPEN = 32;
constexpr uint32_t MAX_SEQUENCES = 32;
constexpr uint32_t MAX_WAITING_CHORDS = 8; // Gated rows one press can open.
constexpr uint32_t MAX_POOLED_ACTIONS = 32;
constexpr uint32_t MAX_PULSES = 32;
constexpr uint32_t MAX_QUEUED = 256; // Callbacks and signals, per frame.
constexpr uint32_t MAX_HELD_KEYS = 32;
constexpr uint32_t MAX_UNUSUAL_KEYS = 8; // Suppressed keys outside KeySet's bits.
constexpr uint32_t MAX_TOUCHES = 10;
constexpr uint32_t MAX_CONNECTION_CHANGES = 64; // Ring; a power of two.
constexpr uint32_t FRAME_RING = 4; // Frames; a power of two. A reader's frame holds FRAME_RING - 2 frame times at least.
constexpr uint32_t FRAME_MASK = FRAME_RING - 1;
static_assert(FRAME_RING >= 3 && (FRAME_RING & FRAME_MASK) == 0, "Read, write and at least one sealed frame of grace; flip_seq wraps cleanly.");
constexpr uint32_t MAX_WARNED_IDS = 64;
constexpr uint32_t MAX_OVERRIDES = 64;
constexpr uint32_t MAX_NAMED_CALLBACKS = 256;
constexpr uint32_t PATH_HISTORY = 32;
constexpr float SUPPRESS_RELEASE = 0.25f; // Suppressed axis slots clear below this.
constexpr float SEQUENCE_TIMEOUT_DEFAULT = 0.8f;
constexpr float SEQUENCE_TIMEOUT_MIN = 0.3f;
constexpr float SEQUENCE_TIMEOUT_MAX = 2.0f;
constexpr float CHORD_WINDOW_DEFAULT = 0.05f;
constexpr float CHORD_WINDOW_MAX = 0.12f;

// Cold side of a row, one line each. A fire reads only the first 32 bytes.
struct RouteBindingInfo {
	StringName action;
	Callable target;
	uint32_t handle = 0; // attach() or load_map() handle. 0: dead.
	int16_t named = -1; // named_callbacks index.
	StringName id;
	String label;
#ifdef TOOLS_ENABLED
	ObjectID source; // Live retune.
	uint8_t reserved[8] = {};
#else
	uint8_t reserved[16] = {};
#endif
};
static_assert(sizeof(RouteBindingInfo) == CACHE_LINE, "One line per row.");

// A chord waiting on its gate, or a sequence waiting on its next press. Input and gate copy the
// row's, so chord and sequence scans skip the row. Four per line.
struct alignas(16) OpenMatch {
	uint16_t row = NO_ROW;
	uint8_t input = NONE;
	uint8_t gate = NONE;
	uint8_t step = 0; // Sequence presses matched.
	uint64_t until = 0;
};
static_assert(sizeof(OpenMatch) == 16, "Four per line.");

enum SlotFlags : uint8_t {
	SLOT_FALLBACK = 0x03, // GamepadContext::Fallback.
	SLOT_CHORD_GATE = 1 << 2, // Gate of a routed row: its own row resolves at release.
};

// One line per pad.
struct PadState {
	uint8_t family = InputRouter::PAD_FAMILY_GENERIC; // Set on connect, so the read side carries it.
	uint32_t down = 0; // Bit per button.
	uint32_t pressed_edges = 0;
	uint32_t released_edges = 0;
	uint32_t passed_buttons = 0; // Handed to Godot: a release is owed.
	uint8_t passed_axes = 0;
	uint64_t suppressed = 0; // Bit per slot, eaten until released.
	float axes[AXIS_COUNT] = {};
	uint8_t reserved[8] = {};

	_FORCE_INLINE_ bool is_down(uint8_t p_button) const { return down & (1u << p_button); }
	_FORCE_INLINE_ bool is_suppressed(uint8_t p_slot) const { return suppressed & slot_bit(p_slot); }
	Vector2 stick(int p_stick) const { return p_stick == 0 ? Vector2(axes[0], axes[1]) : Vector2(axes[2], axes[3]); }

	// Raw: button 0/1, axis half 0..1, full axis -1..1, stick magnitude.
	float slot_value(uint8_t p_slot) const {
		if (is_button_slot(p_slot)) {
			return is_down(p_slot) ? 1.0f : 0.0f;
		}
		if (is_axis_slot(p_slot)) {
			const float v = axes[(int)slot_axis(p_slot)];
			const int dir = slot_direction(p_slot);
			return dir == 0 ? v : MAX(0.0f, v * dir);
		}
		if (is_stick_slot(p_slot)) {
			return MIN(stick(p_slot - STICK_BASE).length(), 1.0f);
		}
		return 0.0f;
	}
};
static_assert(sizeof(PadState) == CACHE_LINE, "One line per pad.");

struct PadInfo {
	String name;
	String guid;
	String sdl_type;
	int vendor_id = 0;
	int product_id = 0;
	bool steam_input = false;

	static PadInfo from_input(int p_pad);
};

// Valve ids first (SDL has no Deck type), then the SDL type, which names the physical pad even
// behind Steam Input, then the name.
static uint8_t family_for(const PadInfo &p_info) {
	if (p_info.vendor_id == 0x28DE && (p_info.product_id == 0x1205 || p_info.product_id == 0x1102 || p_info.product_id == 0x1142)) {
		return InputRouter::PAD_FAMILY_STEAM;
	}
	const String &t = p_info.sdl_type;
	if (t.begins_with("xbox")) {
		return InputRouter::PAD_FAMILY_XBOX;
	}
	if (t.begins_with("ps")) {
		return InputRouter::PAD_FAMILY_PLAYSTATION;
	}
	if (t.begins_with("switch") || t.begins_with("joycon")) {
		return InputRouter::PAD_FAMILY_NINTENDO;
	}
	const String name = p_info.name.to_lower();
	if (name.contains("xbox")) {
		return InputRouter::PAD_FAMILY_XBOX;
	}
	if (name.contains("playstation") || name.contains("dualsense") || name.contains("dualshock")) {
		return InputRouter::PAD_FAMILY_PLAYSTATION;
	}
	if (name.contains("nintendo") || name.contains("joy-con")) {
		return InputRouter::PAD_FAMILY_NINTENDO;
	}
	if (name.contains("steam deck") || OS::get_singleton()->get_environment("SteamDeck") == "1") {
		return InputRouter::PAD_FAMILY_STEAM;
	}
	return InputRouter::PAD_FAMILY_GENERIC;
}

// p_pad came from Input's connected list or its connection callback.
PadInfo PadInfo::from_input(int p_pad) {
	PadInfo info;
	Input *input = Input::get_singleton();
	info.name = input->get_joy_name(p_pad);
	info.guid = input->get_joy_guid(p_pad);
	const Dictionary joy = input->get_joy_info(p_pad);
	info.sdl_type = joy.get("sdl_type", String());
	info.vendor_id = String(joy.get("vendor_id", "0")).to_int();
	info.product_id = String(joy.get("product_id", "0")).to_int();
	info.steam_input = joy.has("steam_input_index");
	return info;
}

struct ContextInfo {
	StringName id;
	uint8_t fallback = GamepadContext::FALLBACK_INHERIT;
	bool modal = false;
};

struct PathEntry {
	uint8_t context = NONE;
	uint32_t handle = 0; // 0: base context.
};

struct Override {
	StringName binding_id;
	uint8_t input = NONE;
	uint8_t gate = NONE;
};

struct NamedCallback {
	uint32_t handle = 0;
	StringName context;
	StringName binding_id;
	Callable target;
};

// Where a row goes, by name: kept across a route map rebuild, and for an attach the map lacks
// until the next frame's merge.
struct Placement {
	StringName id;
	StringName scheme;
	StringName context;
	Callable target;
	uint32_t handle = 0;
#ifdef TOOLS_ENABLED
	ObjectID source;
#endif
};

struct HeldKey {
	Key keycode = Key::NONE;
	Key physical_keycode = Key::NONE;
	Key key_label = Key::NONE;
	char32_t unicode = 0;
	KeyLocation location = KeyLocation::UNSPECIFIED;
	int64_t window_id = 0;
};

// Latin-1 and special keys as bits, anything else in a short list. Plain data: it lives in HEAD.
struct KeySet {
	uint64_t bits[6] = {};
	Key overflow[MAX_UNUSUAL_KEYS] = {};
	uint8_t overflow_count = 0;

	bool has(Key p_key) const;
	void insert(Key p_key);
	void erase(Key p_key);
};

// Bit index for Latin-1 and special keys, -1 for the rest.
static int key_index(Key p_key) {
	const uint32_t k = (uint32_t)p_key;
	if (k < 0x100) {
		return k;
	}
	if ((k & (uint32_t)Key::SPECIAL) && (k & ~(uint32_t)Key::SPECIAL) < 0x80) {
		return 0x100 + (k & ~(uint32_t)Key::SPECIAL);
	}
	return -1;
}

bool KeySet::has(Key p_key) const {
	const int i = key_index(p_key);
	if (i >= 0) {
		return bits[i >> 6] & (uint64_t(1) << (i & 63));
	}
	for (uint32_t j = 0; j < overflow_count; j++) {
		if (overflow[j] == p_key) {
			return true;
		}
	}
	return false;
}

void KeySet::insert(Key p_key) {
	const int i = key_index(p_key);
	if (i >= 0) {
		bits[i >> 6] |= uint64_t(1) << (i & 63);
		return;
	}
	if (has(p_key)) {
		return;
	}
	ERR_FAIL_COND_MSG(overflow_count == MAX_UNUSUAL_KEYS, "InputRouter: too many unusual keys held; one release may leak.");
	overflow[overflow_count++] = p_key;
}

void KeySet::erase(Key p_key) {
	const int i = key_index(p_key);
	if (i >= 0) {
		bits[i >> 6] &= ~(uint64_t(1) << (i & 63));
		return;
	}
	for (uint32_t j = 0; j < overflow_count; j++) {
		if (overflow[j] == p_key) {
			overflow[j] = overflow[--overflow_count];
			return;
		}
	}
}

struct TouchState {
	int32_t index = -1;
	bool suppressed = false; // Eaten until lifted.
	bool passed = false; // Handed to Godot: a release is owed.
	Vector2 position;
};

// An event's type, found once per flush. PASS: script actions, MIDI, shortcuts; never wakes,
// always passes.
enum EventKind : uint8_t {
	KIND_PASS,
	KIND_PAD_BUTTON,
	KIND_PAD_MOTION,
	KIND_KEY,
	KIND_MOUSE_BUTTON,
	KIND_MOUSE_MOTION,
	KIND_GESTURE,
	KIND_TOUCH,
	KIND_DRAG,
};

enum SignalKind : uint8_t {
	SIGNAL_DEVICE_CHANGED,
	SIGNAL_PAD_CONNECTED,
	SIGNAL_PAD_DISCONNECTED,
	SIGNAL_ACTIVE_PAD_LOST,
	SIGNAL_CONTEXT_CHANGED,
	SIGNAL_ROUTES_CHANGED,
	SIGNAL_SCHEME_CHANGED,
	SIGNAL_TOUCH_ROUTED,
};

// A callback (code: Phase) or a signal (code: SignalKind) in the BODY of the frame that queued it.
// Both run after the flip that seals that frame, callbacks first, so they see it as the read frame.
struct Queued {
	uint8_t code = 0;
	bool signal = false;
	uint16_t row = NO_ROW;
	int32_t i[4] = {}; // Callback: row handle, vector flag. Touch: index. Signal: arguments.
	Vector2 v; // Callback value, touch position.
	float f = 0.0f; // Touch pressure.
};
static_assert(sizeof(Queued) == 32, "Queued grew.");

enum Outcome : uint8_t {
	OUTCOME_ROUTED,
	OUTCOME_PASSED,
	OUTCOME_SWALLOWED,
	OUTCOME_SUPPRESSED,
	OUTCOME_DROPPED_SWITCH,
	OUTCOME_SEQUENCE_ABANDONED,
	OUTCOME_SWITCH,
	OUTCOME_CANCELED,
};

static const char *OUTCOME_NAMES[] = { "routed", "passed", "swallowed", "suppressed", "dropped_switch", "sequence_abandoned", "switch", "canceled" };

struct TraceRecord {
	uint64_t usec = 0;
	uint32_t frame = 0;
	uint32_t path_version = 0;
	int16_t row = -1;
	int8_t pad = -1;
	uint8_t input = NONE;
	uint8_t phase = 0;
	uint8_t outcome = 0;
	uint8_t context = NONE;
	float value = 0.0f;
};

// HEAD: fixed layout, copied whole from the read frame by begin_frame(), then updated in place.
// Line 0 is what most getters read. Line 1 counts the BODY sections, masks the live entries of the
// pad and claim tables, and holds the FOOT: which copy of each State table the frame resolves against.
struct alignas(CACHE_LINE) FrameHead {
	// Line 0.
	uint64_t frame = 0;
	uint16_t connected_pads = 0; // Bit per pad.
	uint16_t live_pads = 0; // Pads whose Frame::pads entry is current: connected, or sent events.
	bool enabled = false;
	bool switch_frame = false;
	uint8_t device = InputRouter::DEVICE_NONE;
	int8_t active_pad = -1;
	uint8_t policy = InputRouter::DEVICE_POLICY_AUTO;
	int8_t scheme = -1; // Into NameTable::schemes; -1: scheme-less rows only.

	// Line 1.
	alignas(CACHE_LINE) uint64_t claimed_slots = 0; // Live entries of Frame::claims.
	uint64_t now = 0;
	uint64_t last_tick = 0;
	uint32_t path_version = 0;
	uint16_t edge_pads = 0; // Pads with edges this frame.
	uint16_t queued_count = 0;
	uint8_t open_count = 0;
	uint8_t sequence_count = 0;
	uint8_t touch_count = 0;
	uint8_t key_count = 0;
	uint8_t queued_signals = 0; // Collapsing SignalKinds queued this frame.
	uint8_t path_size = 0;
	uint8_t base_count = 0; // Path entries set_gamepad_base_contexts() owns.
	uint8_t routable_from = 0; // Innermost modal path entry; path_changed() sets it.
	uint8_t foot_names = 0;
	uint8_t foot_rows = 0;
	uint8_t foot_routes = 0;

	// Line 2: pointer and mouse.
	alignas(CACHE_LINE) Vector2 mouse_position;
	Vector2 pointer_velocity;
	Vector2 pointer_delta;
	Vector2 stick_vectors[2]; // Last shaped vector of each stick claim.
	int64_t mouse_window = 0;
	uint32_t mouse_down = 0;
	uint32_t mouse_suppressed = 0;

	// Lines 3-4: the context path, outer to inner.
	alignas(CACHE_LINE) PathEntry path[MAX_PATH_DEPTH];

	// Lines 5-6.
	alignas(CACHE_LINE) KeySet keys_suppressed;
};
static_assert(sizeof(FrameHead) == 7 * CACHE_LINE, "HEAD is seven lines.");
static_assert(std::is_trivially_copyable_v<FrameHead>, "begin_frame() copies HEAD whole.");

// One frame of router state: S(t + 1) = F(S(t), input). HEAD is fixed layout, rebuilt in place
// every frame; its pad and claim tables are indexed by pad and by slot, and only entries its masks
// call live are copied or read. BODY sections pack records 0..count - 1, counts in HEAD line 1,
// capacities fixed at compile time, so a frame never allocates. FOOT is the three table indices in
// HEAD line 1. A ring of FRAME_RING frames: the write frame, forked from the read frame by
// begin_frame() and filled by the frame update and by mutators until the next flip; the read
// frame, sealed by the last flip; and older sealed frames a reader on another thread may still hold.
struct alignas(CACHE_LINE) Frame : FrameHead {
	PadState pads[Input::JOYPADS_MAX]; // Live per live_pads.
	Claim claims[INPUT_COUNT]; // Live per claimed_slots. Two per line.

	// BODY.
	alignas(CACHE_LINE) OpenMatch open[MAX_OPEN]; // Chords waiting on their gate, range order.
	OpenMatch sequences[MAX_SEQUENCES];
	TouchState touches[MAX_TOUCHES];
	HeldKey keys_down[MAX_HELD_KEYS];
	alignas(CACHE_LINE) Queued queued[MAX_QUEUED]; // Run after the flip that seals the frame.
};
static_assert(std::is_trivially_copyable_v<PadState> && std::is_trivially_copyable_v<Claim> && std::is_trivially_copyable_v<OpenMatch> && std::is_trivially_copyable_v<TouchState> && std::is_trivially_copyable_v<HeldKey>, "begin_frame() copies by memcpy.");

// FOOT tables: large, changed by mutators and map rebuilds rather than every frame. State keeps two
// copies of each and a frame names one. The write frame shares the read frame's copy until it
// first edits it after a flip, which copies it into the other slot (copy on write). Main thread
// only, and only through the read and write frames: an older frame in the ring may still name the
// other slot, but nothing reads FOOT through it.
struct NameTable {
	FixedVector<ContextInfo, MAX_CONTEXTS> contexts;
	FixedVector<StringName, MAX_SCHEMES> schemes;
	PadInfo pad_info[Input::JOYPADS_MAX];
};

struct RowTable {
	LineVector<RouteBinding> rows;
	LineVector<RouteBindingInfo> info;
	LocalVector<uint16_t> free_rows;
};

// Path, rows and scheme resolved by rebuild_routes(): slot s routes active[slot_first[s] ..
// slot_first[s + 1]).
struct alignas(CACHE_LINE) RouteTable {
	uint8_t slot_first[INPUT_COUNT + 1] = {};
	alignas(CACHE_LINE) uint8_t slot_flags[INPUT_COUNT] = {}; // SlotFlags.
	alignas(CACHE_LINE) uint16_t active[MAX_ACTIVE] = {}; // Row ids by packed key.
};

// The engine behind InputRouter: the frame ring, the FOOT tables, settings, inboxes for the next
// frame and scratch. Main thread, except the last group. InputRouter allocates it line-aligned.
struct State {
	// Line 0: the frames, settings a press reads, flags.
	Frame *write_state = &frames[1]; // Main thread only.
	const Frame *read_state = &frames[0]; // Main thread's read_frame(), without the atomic.
	uint64_t chord_window_usec = uint64_t(CHORD_WINDOW_DEFAULT * 1000000);
	uint64_t sequence_timeout_usec = uint64_t(SEQUENCE_TIMEOUT_DEFAULT * 1000000);
	float trigger_wake_threshold = 0.5f;
	InputRouter::MouseInPad mouse_in_pad = InputRouter::MOUSE_IN_PAD_DROP;
	InputRouter::TouchMode touch_mode = InputRouter::TOUCH_AS_POINTER;
	uint8_t action_pool_next = 0;
	bool in_frame = false; // Inside input_poll_process_flip().
	bool routes_dirty = true;
	bool callbacks_dirty = true;
	bool disable_requested = false;
	bool trace_enabled = false;

	Frame frames[FRAME_RING];
	NameTable name_tables[2];
	RowTable row_tables[2];
	RouteTable route_tables[2];

	// Scratch: Input's events this frame then the routed ones it parses, their kinds, Godot's
	// events from the router, rebuild sort keys, pooled action events, action releases owed.
	LocalVector<Ref<InputEvent>> batch;
	LocalVector<EventKind> kinds;
	LocalVector<Ref<InputEvent>> out; // Godot's at the next flip.
	LocalVector<uint64_t> sort_keys;
	FixedVector<Ref<InputEventAction>, MAX_POOLED_ACTIONS> action_pool;
	FixedVector<StringName, MAX_PULSES> pulses; // Released next frame.

	// Keyed by name, inputs to the frame update rather than frame state.
	LocalVector<NamedCallback> named_callbacks;
	LocalVector<Override> overrides;

	// Route map: the current templates and what the next frame rebuilds from. Only the writer
	// reads them; the read side never resolves a LUT or a template.
	Dictionary map_data;
	PackedByteArray templates; // 64 B rows, context NONE.
	LocalVector<StringName> template_ids;
	LocalVector<StringName> template_actions;
	PackedStringArray template_labels;
	LocalVector<float> luts;
	Dictionary pending_map; // set_route_map().
	LocalVector<Ref<RouteBindingResource>> pending_bindings; // Missing or changed: merged next frame.
	LocalVector<Placement> pending_placements; // Waiting on that merge.
	bool map_tried = false; // The project's map was looked for.
	FixedVector<StringName, MAX_WARNED_IDS> warned_ids;

	// Trace and glyphs.
	LocalVector<TraceRecord> trace;
	uint32_t trace_head = 0;
	uint32_t trace_count = 0;
	LocalVector<Pair<uint32_t, PackedStringArray>> path_history;
	uint32_t path_history_head = 0;
	Ref<GamepadGlyphProvider> glyph_provider;
	bool glyph_provider_loaded = false;
	InputRouter *owner = nullptr;
#ifdef TOOLS_ENABLED
	Callable binding_changed; // owner's _binding_changed(), unbound.
#endif

	// Any thread, off the main thread's lines. Connection changes: a ring with one producer at a
	// time (Input's lock serializes joy_connection_changed()) and one consumer (_poll()).
	// flip_seq: & FRAME_MASK picks the read frame; flip() bumps it, once per frame.
	alignas(CACHE_LINE) std::atomic<uint32_t> connection_tail{ 0 };
	std::atomic<uint32_t> connection_head{ 0 };
	std::atomic<bool> connection_overflow{ false };
	Pair<int8_t, bool> connection_ring[MAX_CONNECTION_CHANGES];
	SafeNumeric<uint32_t> next_handle;
	alignas(CACHE_LINE) std::atomic<uint32_t> flip_seq{ 0 };

	// Frames.
	void flip();
	void begin_frame();
	const Frame *read_frame() const { return &frames[flip_seq.load(std::memory_order_acquire) & FRAME_MASK]; } // Any thread.
	bool enable();
	void disable();
	void deliver_out();

	// FOOT. The router resolves against the write frame's copies, getters against the read frame's.
	const NameTable &names_of(const FrameHead &p_frame) const { return name_tables[p_frame.foot_names]; }
	const RowTable &rows_of(const FrameHead &p_frame) const { return row_tables[p_frame.foot_rows]; }
	const RouteTable &routes_of(const FrameHead &p_frame) const { return route_tables[p_frame.foot_routes]; }
	const NameTable &names() const { return names_of(*write_state); }
	const RowTable &rows() const { return rows_of(*write_state); }
	const RouteTable &routes() const { return routes_of(*write_state); }
	NameTable &edit_names();
	RowTable &edit_rows();
	RowTable &clear_rows();
	RouteTable &edit_routes();

	// Frame.
	void poll_connections();
	void route_frame();
	uint64_t capture(const InputEvent *p_event, EventKind p_kind);
	void switch_to(InputRouter::Device p_device, int p_pad);
	void suppress_dropped();
	void release_pads(bool p_emit_releases);
	void release_all(bool p_emit_releases);
	void route_button(int p_pad, uint8_t p_slot, bool p_pressed, const Ref<InputEvent> &p_event, uint64_t p_eaten);
	bool press_slot(int p_pad, uint8_t p_slot);
	void route_axis(int p_pad, JoyAxis p_axis, const Ref<InputEvent> &p_event, uint64_t p_eaten);
	bool route_kbm(const InputEvent *p_event, EventKind p_kind);
	bool route_touch(const InputEvent *p_event, EventKind p_kind);
	void tick();
	void feed_sequences(uint8_t p_slot);

	// Chords.
	void open_chords(const uint16_t *p_rows, uint32_t p_count);
	void complete_chords(int p_pad, uint8_t p_gate);
	void drop_chords(uint64_t p_inputs);
	void flush_held_back(uint64_t p_inputs);
	void use_gate(uint8_t p_gate);

	// Claims and output.
	Claim &claim(uint8_t p_slot, uint16_t p_row);
	void end_claim(uint8_t p_slot);
	void cancel_claim(uint8_t p_slot);
	void cancel_claims();
	void fire(uint8_t p_slot, uint32_t p_bits);
	float shaped_for(const RouteBinding &p_row, int p_pad, uint8_t p_slot, Vector2 *r_vector) const;
	void emit_action(const StringName &p_action, bool p_pressed, float p_strength, int p_device);
	Queued *push_queued(bool p_signal);
	void queue_callback(uint16_t p_row, InputRouter::Phase p_phase, const Vector2 &p_value, bool p_vector);
	void queue_touch(int32_t p_index, InputRouter::Phase p_phase, const Vector2 &p_position, float p_pressure);
	void queue_signal(SignalKind p_kind, int32_t p_a = 0, int32_t p_b = 0, int32_t p_c = 0, int32_t p_d = 0);
	void run_callback(const Queued &p_queued);
	void run_signal(const Queued &p_queued);

	// Contexts, rows, routes.
	int ensure_context(const StringName &p_id);
	int scheme_mask_of(const StringName &p_scheme);
	StringName scheme_of_mask(uint8_t p_mask) const;
	void apply_override(RouteBinding &r_row, const StringName &p_id) const;
	void reapply_override(const StringName &p_id);
	uint16_t add_row(const RouteBinding &p_row, const RouteBindingInfo &p_info);
	void kill_row(uint16_t p_row);
	bool release_handle(uint32_t p_handle);
	void path_changed();
	void rebuild_routes();

	// Route map.
	void ensure_map();
	void load_tables(const Dictionary &p_data);
	void rebuild_from_map();
	int find_template(const StringName &p_id, uint8_t p_scheme_mask, RouteBinding &r_row) const;
	bool place(const Placement &p_placement);
	void queue_binding(const Ref<RouteBindingResource> &p_binding, const char *p_why);
#ifdef TOOLS_ENABLED
	void watch_binding(ObjectID p_source, bool p_watch);
#endif

	// Trace.
	void trace_event(uint8_t p_input, int p_pad, uint8_t p_phase, float p_value, int p_row, uint8_t p_context, Outcome p_outcome);
	void record_path();
};
static_assert(alignof(State) == CACHE_LINE, "InputRouter allocates State line-aligned.");

} // namespace InputRouterInternal

using namespace InputRouterInternal;

// Bound mutators off the main thread re-run on it; getters without a snapshot refuse.
#define MAIN_OR_DEFER(m_call)               \
	if (unlikely(!Thread::is_main_thread())) { \
		_defer(m_call);                        \
		return;                                \
	}
#define MAIN_OR_RETURN(m_ret) ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), m_ret, "InputRouter: main thread only.")

// p_bits must not be zero.
static _FORCE_INLINE_ uint32_t lowest_bit(uint64_t p_bits) {
	return FFS64(p_bits) - 1;
}

// p_frame's state of p_pad, or of its active pad for -1. Null when that pad has no live state.
static const PadState *pad_of(const Frame *p_frame, int p_pad) {
	const int pad = p_pad < 0 ? p_frame->active_pad : p_pad;
	return pad >= 0 && pad < Input::JOYPADS_MAX && (p_frame->live_pads & (1u << pad)) ? &p_frame->pads[pad] : nullptr;
}

// Events the router makes block Resource::changed, so their setters skip the per-object signal
// mutex (and, in debug builds, ClassDB's signal lookup). The action pool and its fallback.
static Ref<InputEventAction> make_action_event() {
	Ref<InputEventAction> ev;
	ev.instantiate();
	ev->set_block_signals(true);
	return ev;
}

// Zeroes an axis Godot saw move, so its last value does not stick.
static Ref<InputEventJoypadMotion> make_axis_zero(int p_pad, JoyAxis p_axis) {
	Ref<InputEventJoypadMotion> ev;
	ev.instantiate();
	ev->set_block_signals(true);
	ev->set_device(p_pad);
	ev->set_axis(p_axis);
	ev->set_axis_value(0.0f);
	return ev;
}

// Ends a touch Godot saw begin.
static Ref<InputEventScreenTouch> make_touch_cancel(const TouchState &p_touch) {
	Ref<InputEventScreenTouch> ev;
	ev.instantiate();
	ev->set_block_signals(true);
	ev->set_index(p_touch.index);
	ev->set_position(p_touch.position);
	ev->set_pressed(false);
	ev->set_canceled(true);
	return ev;
}

static uint64_t held_slots(const PadState &p_pad) {
	uint64_t held = p_pad.down;
	for (uint8_t a = 0; a < AXIS_COUNT; a++) {
		if (-p_pad.axes[a] >= SUPPRESS_RELEASE) {
			held |= slot_bit(axis_slot(JoyAxis(a), -1));
		}
		if (p_pad.axes[a] >= SUPPRESS_RELEASE) {
			held |= slot_bit(axis_slot(JoyAxis(a), 1));
		}
	}
	return held;
}

static JoyAxis partner_axis(JoyAxis p_axis) {
	switch (p_axis) {
		case JoyAxis::LEFT_X:
			return JoyAxis::LEFT_Y;
		case JoyAxis::LEFT_Y:
			return JoyAxis::LEFT_X;
		case JoyAxis::RIGHT_X:
			return JoyAxis::RIGHT_Y;
		case JoyAxis::RIGHT_Y:
			return JoyAxis::RIGHT_X;
		default:
			return p_axis;
	}
}

static Key key_of(const InputEventKey *p_key) {
	return p_key->get_physical_keycode() != Key::NONE ? p_key->get_physical_keycode() : p_key->get_keycode();
}

static uint32_t mouse_bit(MouseButton p_button) {
	switch (p_button) {
		case MouseButton::LEFT:
		case MouseButton::RIGHT:
		case MouseButton::MIDDLE:
		case MouseButton::MB_XBUTTON1:
		case MouseButton::MB_XBUTTON2:
			return (uint32_t)mouse_button_to_mask(p_button);
		default:
			return 0; // Wheel: instantaneous, never held.
	}
}

// Slot, context depth (inner first), gated first, priority (high first), row id.
static _FORCE_INLINE_ uint64_t active_key(const RouteBinding &p_row, uint8_t p_depth, uint16_t p_row_id) {
	return uint64_t(p_row.input) << 40 | uint64_t(p_depth) << 32 | uint64_t(p_row.gate == NONE) << 24 |
			uint64_t(uint8_t(127 - p_row.priority)) << 16 | p_row_id;
}
static _FORCE_INLINE_ uint8_t key_slot(uint64_t p_key) { return (uint8_t)(p_key >> 40); }
static _FORCE_INLINE_ uint8_t key_depth(uint64_t p_key) { return (uint8_t)(p_key >> 32); }
static _FORCE_INLINE_ uint16_t key_row(uint64_t p_key) { return (uint16_t)p_key; }

// Commonest first: sticks, mouse motion, buttons.
static EventKind kind_of(const InputEvent *p_event) {
	if (Object::cast_to<InputEventJoypadMotion>(p_event)) {
		return KIND_PAD_MOTION;
	}
	if (Object::cast_to<InputEventMouseMotion>(p_event)) {
		return KIND_MOUSE_MOTION;
	}
	if (Object::cast_to<InputEventJoypadButton>(p_event)) {
		return KIND_PAD_BUTTON;
	}
	if (Object::cast_to<InputEventKey>(p_event)) {
		return KIND_KEY;
	}
	if (Object::cast_to<InputEventMouseButton>(p_event)) {
		return KIND_MOUSE_BUTTON;
	}
	if (Object::cast_to<InputEventScreenDrag>(p_event)) {
		return KIND_DRAG;
	}
	if (Object::cast_to<InputEventScreenTouch>(p_event)) {
		return KIND_TOUCH;
	}
	if (Object::cast_to<InputEventGesture>(p_event)) {
		return KIND_GESTURE;
	}
	return KIND_PASS;
}

// A pad button press, a trigger crossing trigger_wake_threshold, a key, mouse button or touch
// press. Sticks and mouse motion never wake; the device policy filters the rest. r_down and
// r_triggers follow the batch, so edges inside it are exact.
static IR::Device wake_of(const State &p_s, const InputEvent *p_event, EventKind p_kind, int &r_pad, uint32_t *r_down, float (*r_triggers)[2]) {
	IR::Device w = IR::DEVICE_NONE;
	switch (p_kind) {
		case KIND_PAD_BUTTON:
		case KIND_PAD_MOTION: {
			const int pad = p_event->get_device();
			if (pad < 0 || pad >= Input::JOYPADS_MAX) {
				break;
			}
			r_pad = pad;
			if (p_kind == KIND_PAD_BUTTON) {
				const InputEventJoypadButton *jb = static_cast<const InputEventJoypadButton *>(p_event);
				const uint8_t b = button_slot(jb->get_button_index());
				if (b == NONE) {
					break;
				}
				const uint32_t bit = 1u << b;
				if (jb->is_pressed() && !(r_down[pad] & bit)) {
					w = IR::DEVICE_PAD;
				}
				r_down[pad] = jb->is_pressed() ? (r_down[pad] | bit) : (r_down[pad] & ~bit);
			} else {
				const InputEventJoypadMotion *jm = static_cast<const InputEventJoypadMotion *>(p_event);
				const JoyAxis axis = jm->get_axis();
				if (axis != JoyAxis::TRIGGER_LEFT && axis != JoyAxis::TRIGGER_RIGHT) {
					break;
				}
				float &prev = r_triggers[pad][axis == JoyAxis::TRIGGER_RIGHT];
				if (prev < p_s.trigger_wake_threshold && jm->get_axis_value() >= p_s.trigger_wake_threshold) {
					w = IR::DEVICE_PAD;
				}
				prev = jm->get_axis_value();
			}
		} break;
		case KIND_KEY: {
			const InputEventKey *k = static_cast<const InputEventKey *>(p_event);
			if (k->is_pressed() && !k->is_echo()) {
				w = IR::DEVICE_KBM;
			}
		} break;
		case KIND_MOUSE_BUTTON:
			if (static_cast<const InputEventMouseButton *>(p_event)->is_pressed()) {
				w = IR::DEVICE_KBM;
			}
			break;
		case KIND_TOUCH:
			if (!static_cast<const InputEventScreenTouch *>(p_event)->is_pressed()) {
				break;
			}
			if (p_s.touch_mode == IR::TOUCH_AS_POINTER) {
				w = IR::DEVICE_KBM;
			} else if (p_s.touch_mode == IR::TOUCH_AS_PAD) {
				const Frame &f = *p_s.write_state;
				w = IR::DEVICE_PAD;
				r_pad = f.active_pad >= 0 ? f.active_pad : (f.connected_pads ? (int)lowest_bit(f.connected_pads) : -1);
			}
			break;
		default:
			break;
	}
	const uint8_t policy = p_s.write_state->policy;
	if ((policy == IR::DEVICE_POLICY_PAD && w == IR::DEVICE_KBM) || (policy == IR::DEVICE_POLICY_KBM && w == IR::DEVICE_PAD)) {
		return IR::DEVICE_NONE;
	}
	return w;
}

// --- State: frames ---

// Seals the write frame and makes it the read frame: one release store per frame, no copy. A
// reader loads flip_seq once (acquire) and reads frames[seq & FRAME_MASK] through a const pointer,
// no copy, no retry. begin_frame() only rewrites the frame sealed FRAME_RING - 1 flips earlier, and
// only input_poll_process_flip() flips, once per frame, so a reader's frame holds until the third
// flip after its load: two frame times at least (33 ms at 60 Hz, 14 ms at 144 Hz). Enabling and
// disabling wait for that frame too. Any-thread getters read plain frame data only, never FOOT, so
// a reader stalled past that sees a newer frame's bytes, never freed memory. A sealed frame's
// routes match its rows, path and scheme.
void State::flip() {
	if (routes_dirty) {
		rebuild_routes();
	}
	const uint32_t seq = flip_seq.load(std::memory_order_relaxed) + 1;
	flip_seq.store(seq, std::memory_order_release);
	read_state = write_state;
	write_state = &frames[(seq + 1) & FRAME_MASK];
	begin_frame();
}

// Start of the frame update: the write frame becomes a copy of the read frame. HEAD whole, the pad
// and claim tables by mask, BODY sections by count, FOOT shared. Per-frame parts start empty: edges,
// switch_frame, the queue. Typically ten lines.
void State::begin_frame() {
	Frame &f = *write_state;
	const Frame &p = *read_state;
	static_cast<FrameHead &>(f) = p;
	for (uint32_t bits = p.live_pads; bits; bits &= bits - 1) {
		const uint32_t pad = lowest_bit(bits);
		f.pads[pad] = p.pads[pad];
		f.pads[pad].pressed_edges = 0;
		f.pads[pad].released_edges = 0;
	}
	for (uint64_t bits = p.claimed_slots; bits; bits &= bits - 1) {
		const uint32_t slot = lowest_bit(bits);
		f.claims[slot] = p.claims[slot];
	}
	memcpy(f.open, p.open, p.open_count * sizeof(OpenMatch));
	memcpy(f.sequences, p.sequences, p.sequence_count * sizeof(OpenMatch));
	memcpy(f.touches, p.touches, p.touch_count * sizeof(TouchState));
	memcpy(f.keys_down, p.keys_down, p.key_count * sizeof(HeldKey));
	f.edge_pads = 0;
	f.switch_frame = false;
	f.queued_count = 0;
	f.queued_signals = 0;
}

// --- State: FOOT ---

// Element-wise copy instead of NameTable's operator=: FixedVector's copy assignment assigns into
// slots past the target's size, which hold no constructed element, so a table that grew since the
// slot was last written unrefs whatever bytes were there (StringName::unref crash, bug 13).
// clear() destroys and push_back() constructs in place.
static void copy_names(NameTable &r_to, const NameTable &p_from) {
	r_to.contexts.clear();
	for (const ContextInfo &ci : p_from.contexts) {
		r_to.contexts.push_back(ci);
	}
	r_to.schemes.clear();
	for (const StringName &scheme : p_from.schemes) {
		r_to.schemes.push_back(scheme);
	}
	for (int i = 0; i < Input::JOYPADS_MAX; i++) {
		r_to.pad_info[i] = p_from.pad_info[i];
	}
}

// Copy on write. The write frame shares the read frame's copy until its first edit after a flip,
// which copies it into the other slot (fixed capacity, no allocation).
NameTable &State::edit_names() {
	Frame &f = *write_state;
	if (f.foot_names == read_state->foot_names) {
		copy_names(name_tables[f.foot_names ^ 1], name_tables[f.foot_names]);
		f.foot_names ^= 1;
	}
	return name_tables[f.foot_names];
}

RowTable &State::edit_rows() {
	Frame &f = *write_state;
	if (f.foot_rows == read_state->foot_rows) {
		const RowTable &from = row_tables[f.foot_rows];
		RowTable &to = row_tables[f.foot_rows ^ 1];
		to.rows.clear();
		to.info.clear();
		for (uint32_t i = 0; i < from.rows.size(); i++) {
			to.rows.push_back(from.rows[i]);
			to.info.push_back(from.info[i]);
		}
		to.free_rows = from.free_rows;
		f.foot_rows ^= 1;
	}
	return row_tables[f.foot_rows];
}

// A map rebuild's: the write frame's own RowTable, emptied, with nothing copied.
RowTable &State::clear_rows() {
	Frame &f = *write_state;
	if (f.foot_rows == read_state->foot_rows) {
		f.foot_rows ^= 1;
	}
	RowTable &rt = row_tables[f.foot_rows];
	rt.rows.clear();
	rt.info.clear();
	rt.free_rows.clear();
	return rt;
}

RouteTable &State::edit_routes() {
	Frame &f = *write_state;
	if (f.foot_routes == read_state->foot_routes) {
		route_tables[f.foot_routes ^ 1] = route_tables[f.foot_routes];
		f.foot_routes ^= 1;
	}
	return route_tables[f.foot_routes];
}

// --- State: lifecycle ---

// Reserves what a frame writes, fills the event pool, reads the pads. Lands in the write frame like
// any mutator; the next frame routes and its flip publishes, so nothing flips between frames.
bool State::enable() {
	Frame &f = *write_state;
	if (f.enabled) {
		return true;
	}
	ERR_FAIL_COND_V_MSG(Engine::get_singleton()->is_editor_hint(), false, "InputRouter: not available in the editor; the inspector preview reads pads directly.");
	Input *input = Input::get_singleton();
	input->set_frame_filter(owner);
	ERR_FAIL_COND_V_MSG(input->get_frame_filter() != owner, false, "InputRouter: another Input frame filter is installed.");
	out.reserve(64);
	batch.reserve(64);
	kinds.reserve(64);
	sort_keys.reserve(MAX_ROWS);
	while (!action_pool.is_full()) {
		action_pool.push_back(make_action_event());
	}
	NameTable &nt = edit_names();
	for (int i = 0; i < Input::JOYPADS_MAX; i++) {
		nt.pad_info[i] = PadInfo();
	}
	f.connected_pads = 0;
	f.live_pads = 0;
	f.edge_pads = 0;
	connection_head.store(connection_tail.load(std::memory_order_acquire), std::memory_order_relaxed); // The list below is newer.
	connection_overflow.store(false, std::memory_order_relaxed);
	const TypedArray<int> connected = input->get_connected_joypads();
	for (int i = 0; i < connected.size(); i++) {
		const int pad = connected[i];
		if (pad < 0 || pad >= Input::JOYPADS_MAX) {
			continue;
		}
		PadState &ps = f.pads[pad];
		ps = PadState();
		for (int a = 0; a < AXIS_COUNT; a++) {
			ps.axes[a] = input->get_joy_axis(pad, JoyAxis(a));
		}
		nt.pad_info[pad] = PadInfo::from_input(pad);
		ps.family = family_for(nt.pad_info[pad]);
		f.connected_pads |= 1u << pad;
		f.live_pads |= 1u << pad;
	}
	f.enabled = true;
	routes_dirty = true;
	f.now = OS::get_singleton()->get_ticks_usec();
	f.last_tick = f.now;
	return true;
}

// Runs in place of a frame, so its flip is that frame's only one. Releases, and anything still
// owed, go to Godot through parse_input_event() once the filter is off, then the frame's buffered
// events, which Input now flushes itself. The write frame's queued callbacks and signals drop.
void State::disable() {
	Frame &f = *write_state;
	if (!f.enabled) {
		return;
	}
	release_all(true);
	for (const StringName &action : pulses) {
		emit_action(action, false, 0.0f, f.active_pad);
	}
	pulses.clear();
	Input *input = Input::get_singleton();
	input->clear_frame_filter(owner);
	f.enabled = false;
	disable_requested = false;
	deliver_out();
	input->flush_buffered_events();
	f.queued_count = 0;
	f.queued_signals = 0;
	flip();
}

void State::deliver_out() {
	for (const Ref<InputEvent> &e : out) {
		Input::get_singleton()->parse_input_event(e);
	}
	out.clear();
}

// --- State: frame ---

// The frame's batch, after _poll() took it. Wakes decide the frame's device; the device in charge
// keeps it if it woke too. A switch drops the batch (script actions pass), releases what the old
// device held and suppresses what the batch pressed. Otherwise every event routes in order into
// `out`, after anything mutators queued since the last flip. Then tick.
void State::route_frame() {
	if (routes_dirty) {
		rebuild_routes();
	}
	Frame &f = *write_state;
	const LocalVector<Ref<InputEvent>> &events = batch;
	kinds.resize(events.size());
	for (uint32_t i = 0; i < events.size(); i++) {
		kinds[i] = kind_of(events[i].ptr());
	}

	uint32_t down[Input::JOYPADS_MAX] = {};
	float triggers[Input::JOYPADS_MAX][2] = {};
	for (uint32_t bits = f.live_pads; bits; bits &= bits - 1) {
		const uint32_t p = lowest_bit(bits);
		down[p] = f.pads[p].down;
		triggers[p][0] = f.pads[p].axes[(int)JoyAxis::TRIGGER_LEFT];
		triggers[p][1] = f.pads[p].axes[(int)JoyAxis::TRIGGER_RIGHT];
	}
	IR::Device wake = IR::DEVICE_NONE;
	int wake_pad = -1;
	bool current_woke = false;
	for (uint32_t i = 0; i < events.size(); i++) {
		int pad = -1;
		const IR::Device w = wake_of(*this, events[i].ptr(), kinds[i], pad, down, triggers);
		if (w == IR::DEVICE_NONE) {
			continue;
		}
		if (w == f.device && (w != IR::DEVICE_PAD || pad == f.active_pad || pad < 0)) {
			current_woke = true;
		} else if (wake == IR::DEVICE_NONE) {
			wake = w;
			wake_pad = pad;
		}
	}

	if (wake != IR::DEVICE_NONE && !current_woke) {
		const bool adopt = f.device == IR::DEVICE_NONE || (f.device == IR::DEVICE_PAD && wake == IR::DEVICE_PAD && f.active_pad < 0);
		if (adopt) {
			const uint8_t prev = f.device;
			const int prev_pad = f.active_pad;
			f.device = wake;
			if (wake_pad >= 0) {
				f.active_pad = wake_pad;
			}
			queue_signal(SIGNAL_DEVICE_CHANGED, f.device, f.active_pad, prev, prev_pad);
		} else {
			for (uint32_t i = 0; i < events.size(); i++) {
				const EventKind kind = kinds[i];
				if (kind == KIND_PASS) {
					out.push_back(events[i]);
				} else if (kind == KIND_PAD_BUTTON || kind == KIND_PAD_MOTION) {
					capture(events[i].ptr(), kind);
				}
			}
			switch_to(wake, wake_pad);
			suppress_dropped();
			trace_event(NONE, f.active_pad, 0, 0.0f, -1, NONE, OUTCOME_DROPPED_SWITCH);
			tick();
			return;
		}
	}

	for (uint32_t i = 0; i < events.size(); i++) {
		const Ref<InputEvent> &e = events[i];
		const EventKind kind = kinds[i];
		switch (kind) {
			case KIND_PASS:
				out.push_back(e);
				break;
			case KIND_PAD_BUTTON:
			case KIND_PAD_MOTION: {
				const uint64_t eaten = capture(e.ptr(), kind);
				const int pad = e->get_device();
				if (f.device != IR::DEVICE_PAD || pad != f.active_pad) {
					break;
				}
				if (kind == KIND_PAD_BUTTON) {
					const InputEventJoypadButton *jb = static_cast<const InputEventJoypadButton *>(e.ptr());
					const uint8_t slot = button_slot(jb->get_button_index());
					if (slot != NONE) {
						route_button(pad, slot, jb->is_pressed(), e, eaten);
					}
				} else {
					const JoyAxis axis = static_cast<const InputEventJoypadMotion *>(e.ptr())->get_axis();
					if ((uint32_t)axis < AXIS_COUNT) {
						route_axis(pad, axis, e, eaten);
					}
				}
			} break;
			case KIND_TOUCH:
			case KIND_DRAG:
				if (route_touch(e.ptr(), kind)) {
					out.push_back(e);
				}
				break;
			default:
				if (route_kbm(e.ptr(), kind)) {
					out.push_back(e);
				}
				break;
		}
	}
	tick();
}

// Updates the pad's state, whoever owns the frame. Returns the event's slots that stay
// suppressed. A press of a suppressed button that is not down is a lost release and clears.
uint64_t State::capture(const InputEvent *p_event, EventKind p_kind) {
	const int pad = p_event->get_device();
	if (pad < 0 || pad >= Input::JOYPADS_MAX) {
		return 0;
	}
	Frame &f = *write_state;
	if (!(f.live_pads & (1u << pad))) {
		f.pads[pad] = PadState(); // Events before its connection was reported.
		f.live_pads |= 1u << pad;
	}
	PadState &ps = f.pads[pad];
	if (p_kind == KIND_PAD_BUTTON) {
		const InputEventJoypadButton *jb = static_cast<const InputEventJoypadButton *>(p_event);
		const uint8_t b = button_slot(jb->get_button_index());
		if (b == NONE) {
			return 0;
		}
		const uint32_t bit = 1u << b;
		const bool was_down = ps.down & bit;
		if (jb->is_pressed()) {
			if (!was_down) {
				ps.pressed_edges |= bit;
				ps.suppressed &= ~slot_bit(b);
				f.edge_pads |= 1u << pad;
			}
			ps.down |= bit;
			return ps.suppressed & slot_bit(b);
		}
		if (was_down) {
			ps.released_edges |= bit;
			f.edge_pads |= 1u << pad;
		}
		ps.down &= ~bit;
		const uint64_t eaten = ps.suppressed & slot_bit(b);
		ps.suppressed &= ~slot_bit(b);
		return eaten;
	}
	const InputEventJoypadMotion *jm = static_cast<const InputEventJoypadMotion *>(p_event);
	const JoyAxis axis = jm->get_axis();
	if ((uint32_t)axis >= AXIS_COUNT) {
		return 0;
	}
	ps.axes[(int)axis] = jm->get_axis_value();
	uint64_t eaten = 0;
	for (int dir = -1; dir <= 1; dir += 2) {
		const uint8_t slot = axis_slot(axis, dir);
		if (!ps.is_suppressed(slot)) {
			continue;
		}
		if (ps.slot_value(slot) < SUPPRESS_RELEASE) {
			ps.suppressed &= ~slot_bit(slot);
		} else {
			eaten |= slot_bit(slot);
		}
	}
	return eaten;
}

// Releases what the old device holds; suppresses what the new pad holds, the wake among it.
void State::switch_to(IR::Device p_device, int p_pad) {
	Frame &f = *write_state;
	const uint8_t prev = f.device;
	const int prev_pad = f.active_pad;
	if (prev == p_device && (p_device != IR::DEVICE_PAD || p_pad < 0 || prev_pad == p_pad)) {
		return;
	}
	release_all(true);
	f.device = p_device;
	if (p_device == IR::DEVICE_PAD && p_pad >= 0) {
		f.active_pad = p_pad;
	}
	if (f.device == IR::DEVICE_PAD && f.active_pad >= 0 && (f.live_pads & (1u << f.active_pad))) {
		f.pads[f.active_pad].suppressed |= held_slots(f.pads[f.active_pad]);
	}
	f.switch_frame = true;
	queue_signal(SIGNAL_DEVICE_CHANGED, f.device, f.active_pad, prev, prev_pad);
	trace_event(NONE, f.active_pad, 0, 0.0f, -1, NONE, OUTCOME_SWITCH);
}

// Presses in a dropped batch are suppressed, so their releases never reach Godot.
void State::suppress_dropped() {
	Frame &f = *write_state;
	const LocalVector<Ref<InputEvent>> &events = batch;
	for (uint32_t e = 0; e < events.size(); e++) {
		if (kinds[e] == KIND_KEY) {
			const InputEventKey *k = static_cast<const InputEventKey *>(events[e].ptr());
			if (k->is_pressed()) {
				f.keys_suppressed.insert(key_of(k));
			} else {
				f.keys_suppressed.erase(key_of(k));
			}
		} else if (kinds[e] == KIND_MOUSE_BUTTON) {
			const InputEventMouseButton *mb = static_cast<const InputEventMouseButton *>(events[e].ptr());
			const uint32_t bit = mouse_bit(mb->get_button_index());
			f.mouse_suppressed = mb->is_pressed() ? (f.mouse_suppressed | bit) : (f.mouse_suppressed & ~bit);
		} else if (kinds[e] == KIND_TOUCH) {
			const InputEventScreenTouch *t = static_cast<const InputEventScreenTouch *>(events[e].ptr());
			int found = -1;
			for (uint32_t i = 0; i < f.touch_count; i++) {
				if (f.touches[i].index == t->get_index()) {
					found = i;
				}
			}
			if (t->is_pressed() && found < 0 && f.touch_count < MAX_TOUCHES) {
				TouchState &ts = f.touches[f.touch_count++];
				ts = TouchState();
				ts.index = t->get_index();
				ts.suppressed = true;
			} else if (!t->is_pressed() && found >= 0) {
				f.touches[found] = f.touches[--f.touch_count];
			}
		}
	}
}

// Ends the pad side in flight: claims and sequences cancel, passed pad input releases, held pad
// input is suppressed, the pointer stops. p_emit_releases false: Input already dropped its state.
void State::release_pads(bool p_emit_releases) {
	Frame &f = *write_state;
	if (!p_emit_releases) {
		for (uint64_t held = f.claimed_slots; held; held &= held - 1) {
			f.claims[lowest_bit(held)].bits &= ~CLAIM_ACTION_DOWN;
		}
	}
	cancel_claims();
	for (uint32_t i = 0; i < f.sequence_count; i++) {
		f.sequences[i].step = 0;
	}

	// Pads with live state: connected ones, and any that sent events, the active pad among them.
	for (uint32_t bits = f.live_pads; bits; bits &= bits - 1) {
		const int p = lowest_bit(bits);
		PadState &ps = f.pads[p];
		if (p_emit_releases) {
			for (uint32_t held = ps.passed_buttons; held; held &= held - 1) {
				Ref<InputEventJoypadButton> ev;
				ev.instantiate();
				ev->set_block_signals(true);
				ev->set_device(p);
				ev->set_button_index(JoyButton(lowest_bit(held)));
				ev->set_pressed(false);
				out.push_back(ev);
			}
			for (uint32_t held = ps.passed_axes; held; held &= held - 1) {
				out.push_back(make_axis_zero(p, JoyAxis(lowest_bit(held))));
			}
		}
		ps.passed_buttons = 0;
		ps.passed_axes = 0;
	}
	if (f.active_pad >= 0 && (f.live_pads & (1u << f.active_pad))) {
		f.pads[f.active_pad].suppressed |= held_slots(f.pads[f.active_pad]);
	}
	f.pointer_velocity = Vector2();
	f.pointer_delta = Vector2();
}

// Ends everything in flight: the pad side as release_pads(), then keys and mouse buttons release
// and stay suppressed until pressed again, and touches cancel.
// p_emit_releases false: Input already dropped its own state (focus loss).
void State::release_all(bool p_emit_releases) {
	Frame &f = *write_state;
	release_pads(p_emit_releases);

	for (uint32_t i = 0; i < f.key_count; i++) {
		const HeldKey &k = f.keys_down[i];
		if (p_emit_releases) {
			Ref<InputEventKey> ev;
			ev.instantiate();
			ev->set_block_signals(true);
			ev->set_keycode(k.keycode);
			ev->set_physical_keycode(k.physical_keycode);
			ev->set_key_label(k.key_label);
			ev->set_unicode(k.unicode);
			ev->set_location(k.location);
			ev->set_window_id(k.window_id);
			ev->set_pressed(false);
			out.push_back(ev);
		}
		f.keys_suppressed.insert(k.physical_keycode != Key::NONE ? k.physical_keycode : k.keycode);
	}
	f.key_count = 0;

	if (p_emit_releases) {
		for (uint32_t bit = 1; bit && bit <= f.mouse_down; bit <<= 1) {
			if (!(f.mouse_down & bit)) {
				continue;
			}
			Ref<InputEventMouseButton> ev;
			ev.instantiate();
			ev->set_block_signals(true);
			ev->set_button_index(MouseButton(lowest_bit(bit) + 1));
			ev->set_pressed(false);
			ev->set_position(f.mouse_position);
			ev->set_global_position(f.mouse_position);
			ev->set_window_id(f.mouse_window);
			out.push_back(ev);
		}
	}
	f.mouse_suppressed |= f.mouse_down;
	f.mouse_down = 0;

	for (uint32_t i = 0; i < f.touch_count; i++) {
		TouchState &t = f.touches[i];
		if (t.passed && p_emit_releases) {
			out.push_back(make_touch_cancel(t));
		} else if (!t.passed && !t.suppressed) {
			queue_touch(t.index, IR::PHASE_CANCELED, t.position, 0.0f);
		}
		t.passed = false;
		t.suppressed = true;
	}
}

// Drains the ring joy_connection_changed() fills. A quiet frame costs two atomic loads. After an
// overflow the ring is skipped and Input's connected list rebuilds the difference. A change the
// frame already shows is skipped: enable() reads Input's list after it empties the ring, so a pad
// that connects in between arrives twice.
void State::poll_connections() {
	const uint32_t tail = connection_tail.load(std::memory_order_acquire);
	uint32_t head = connection_head.load(std::memory_order_relaxed);
	if (head == tail && !connection_overflow.load(std::memory_order_relaxed)) {
		return;
	}
	FixedVector<Pair<int8_t, bool>, MAX_CONNECTION_CHANGES> changes;
	for (; head != tail; head++) {
		changes.push_back(connection_ring[head % MAX_CONNECTION_CHANGES]);
	}
	connection_head.store(tail, std::memory_order_release);
	Frame &f = *write_state;
	if (connection_overflow.exchange(false, std::memory_order_acquire)) {
		changes.clear();
		const TypedArray<int> connected = Input::get_singleton()->get_connected_joypads();
		for (int p = 0; p < Input::JOYPADS_MAX; p++) {
			const bool now_connected = connected.has(p);
			if (now_connected != bool(f.connected_pads & (1u << p)) && !changes.is_full()) {
				changes.push_back(Pair<int8_t, bool>(p, now_connected));
			}
		}
	}
	for (const Pair<int8_t, bool> &c : changes) {
		const int p = c.first;
		if (p < 0 || p >= Input::JOYPADS_MAX || c.second == bool(f.connected_pads & (1u << p))) {
			continue;
		}
		if (c.second) {
			NameTable &nt = edit_names();
			nt.pad_info[p] = PadInfo::from_input(p);
			f.pads[p] = PadState();
			f.pads[p].family = family_for(nt.pad_info[p]);
			f.connected_pads |= 1u << p;
			f.live_pads |= 1u << p;
			queue_signal(SIGNAL_PAD_CONNECTED, p);
		} else {
			if (p == f.active_pad) {
				cancel_claims();
			}
			f.pads[p] = PadState();
			f.connected_pads &= ~(1u << p);
			f.live_pads &= ~(1u << p);
			edit_names().pad_info[p] = PadInfo();
			queue_signal(SIGNAL_PAD_DISCONNECTED, p);
			if (p == f.active_pad) {
				f.active_pad = -1;
				queue_signal(SIGNAL_ACTIVE_PAD_LOST, p);
			}
		}
	}
}

// --- State: pad routing ---

// Press: sequences observe, the slot's range claims, chords waiting on this gate complete.
// Release: the slot's chords close and its claim resolves; an unclaimed passed press passes.
void State::route_button(int p_pad, uint8_t p_slot, bool p_pressed, const Ref<InputEvent> &p_event, uint64_t p_eaten) {
	Frame &f = *write_state;
	PadState &ps = f.pads[p_pad];
	if (p_eaten & slot_bit(p_slot)) {
		trace_event(p_slot, p_pad, p_pressed ? IR::PHASE_PRESSED : IR::PHASE_RELEASED, p_pressed, -1, NONE, OUTCOME_SUPPRESSED);
		return;
	}

	if (!p_pressed) {
		drop_chords(slot_bit(p_slot));
		if (f.claimed_slots & slot_bit(p_slot)) {
			Claim &c = f.claims[p_slot];
			if (!((c.bits & CLAIM_GATE_DEFERRED) && (c.bits & CLAIM_GATE_USED))) { // A used gate's own row never fires.
				const uint32_t held = c.deferred; // Fired before the step, which zeroes the value.
				c.deferred = 0;
				fire(p_slot, held);
				fire(p_slot, trigger_step(rows().rows[c.row], c, 0.0f, f.now));
			}
			end_claim(p_slot);
		} else if (ps.passed_buttons & (1u << p_slot)) {
			ps.passed_buttons &= ~(1u << p_slot);
			out.push_back(p_event);
			trace_event(p_slot, p_pad, IR::PHASE_RELEASED, 0.0f, -1, NONE, OUTCOME_PASSED);
		} else {
			trace_event(p_slot, p_pad, IR::PHASE_RELEASED, 0.0f, -1, NONE, OUTCOME_SWALLOWED);
		}
		return;
	}

	if (f.claimed_slots & slot_bit(p_slot)) {
		// Its release was lost. Not cancel_claim: that would suppress the slot now down.
		if (f.claims[p_slot].bits & CLAIM_ACTION_DOWN) {
			emit_action(rows().info[f.claims[p_slot].row].action, false, 0.0f, f.active_pad);
		}
		drop_chords(slot_bit(p_slot));
		end_claim(p_slot);
	}
	feed_sequences(p_slot);
	const bool matched = press_slot(p_pad, p_slot);
	complete_chords(p_pad, p_slot);
	if (matched) {
		return;
	}
	if ((routes().slot_flags[p_slot] & SLOT_FALLBACK) == GamepadContext::FALLBACK_PASS) {
		ps.passed_buttons |= 1u << p_slot;
		out.push_back(p_event);
		trace_event(p_slot, p_pad, IR::PHASE_PRESSED, 1.0f, -1, NONE, OUTCOME_PASSED);
	} else {
		trace_event(p_slot, p_pad, IR::PHASE_PRESSED, 1.0f, -1, NONE, OUTCOME_SWALLOWED);
	}
}

// The first claim in the slot's range wins. Gated rows with the gate up wait; if a plain row
// then claims, they open as chords and the claim is held back for chord_window.
bool State::press_slot(int p_pad, uint8_t p_slot) {
	const Frame &f = *write_state;
	const PadState &ps = f.pads[p_pad];
	const RouteTable &table = routes();
	const RowTable &rt = rows();
	uint16_t waiting[MAX_WAITING_CHORDS];
	uint32_t waiting_count = 0;
	for (uint32_t i = table.slot_first[p_slot]; i < table.slot_first[p_slot + 1]; i++) {
		const uint16_t r = table.active[i];
		const RouteBinding &row = rt.rows[r];
		if (row.gate != NONE) {
			if (ps.is_down(row.gate) && !ps.is_suppressed(row.gate)) {
				use_gate(row.gate);
				Claim &c = claim(p_slot, r);
				fire(p_slot, trigger_step(row, c, 1.0f, f.now));
				return true;
			}
			if (waiting_count < MAX_WAITING_CHORDS) {
				waiting[waiting_count++] = r;
			}
			continue;
		}
		Claim &c = claim(p_slot, r);
		const uint32_t bits = trigger_step(row, c, 1.0f, f.now);
		if (waiting_count > 0 && chord_window_usec > 0) {
			open_chords(waiting, waiting_count);
			c.bits |= CLAIM_HELD_BACK;
			c.deferred = bits;
		} else if (table.slot_flags[p_slot] & SLOT_CHORD_GATE) {
			c.bits |= CLAIM_GATE_DEFERRED;
			c.deferred = bits;
		} else {
			fire(p_slot, bits);
		}
		return true;
	}
	return false;
}

// Steps the axis's halves and full slot, plus its stick pair. The raw event passes only when no
// own slot is bound and one falls back to PASS; an axis that stops passing gets a zero.
void State::route_axis(int p_pad, JoyAxis p_axis, const Ref<InputEvent> &p_event, uint64_t p_eaten) {
	Frame &f = *write_state;
	PadState &ps = f.pads[p_pad];
	const RouteTable &table = routes();
	const RowTable &rt = rows();
	uint8_t slots[8];
	int slot_count = 0;
	const int stick = stick_of_axis(p_axis);
	const JoyAxis axes[2] = { p_axis, partner_axis(p_axis) };
	for (int a = 0; a < (stick >= 0 ? 2 : 1); a++) {
		for (int dir = -1; dir <= 1; dir++) {
			slots[slot_count++] = axis_slot(axes[a], dir);
		}
	}
	if (stick >= 0) {
		slots[slot_count++] = stick_slot(stick);
	}

	bool bound = false;
	bool pass = false;
	for (int si = 0; si < slot_count; si++) {
		const uint8_t slot = slots[si];
		const bool own = si < 3 || slot == stick_slot(stick);
		if ((p_eaten & slot_bit(slot)) || ps.is_suppressed(slot)) {
			bound = bound || own;
			continue;
		}

		if (f.claimed_slots & slot_bit(slot)) {
			bound = bound || own;
			Claim &c = f.claims[slot];
			const RouteBinding &row = rt.rows[c.row];
			Vector2 vec;
			const float v = shaped_for(row, p_pad, slot, &vec);
			if ((c.bits & CLAIM_HELD_BACK) && Math::abs(v) < row.release) {
				// Released inside the chord window: fire what was held.
				c.bits &= ~CLAIM_HELD_BACK;
				const uint32_t held = c.deferred;
				c.deferred = 0;
				fire(slot, held);
			}
			if (is_stick_slot(slot)) {
				f.stick_vectors[slot - STICK_BASE] = vec;
			}
			const uint32_t bits = trigger_step(row, c, v, f.now);
			if (c.bits & CLAIM_HELD_BACK) {
				c.deferred |= bits;
			} else {
				fire(slot, bits);
			}
			const bool continuous = row.trigger == RouteBindingResource::TRIGGER_ANALOG || (row.flags & ROW_POINTER);
			if (continuous ? (v == 0.0f && vec == Vector2()) : !(c.bits & CLAIM_DOWN)) {
				drop_chords(slot_bit(slot));
				end_claim(slot);
			}
		} else {
			uint16_t waiting[MAX_WAITING_CHORDS];
			uint32_t waiting_count = 0;
			for (uint32_t i = table.slot_first[slot]; i < table.slot_first[slot + 1]; i++) {
				const uint16_t r = table.active[i];
				const RouteBinding &row = rt.rows[r];
				if (row.gate != NONE && (!ps.is_down(row.gate) || ps.is_suppressed(row.gate))) {
					if (waiting_count < MAX_WAITING_CHORDS) {
						waiting[waiting_count++] = r;
					}
					continue;
				}
				bound = bound || own;
				Vector2 vec;
				const float v = shaped_for(row, p_pad, slot, &vec);
				const bool continuous = row.trigger == RouteBindingResource::TRIGGER_ANALOG || (row.flags & ROW_POINTER);
				if (continuous ? (v != 0.0f || vec != Vector2()) : Math::abs(v) >= row.press) {
					if (row.gate != NONE) {
						use_gate(row.gate);
					}
					Claim &c = claim(slot, r);
					if (is_stick_slot(slot)) {
						f.stick_vectors[slot - STICK_BASE] = vec;
					}
					const uint32_t bits = trigger_step(row, c, v, f.now);
					if (row.gate == NONE && waiting_count > 0 && chord_window_usec > 0 && !continuous) {
						open_chords(waiting, waiting_count);
						c.bits |= CLAIM_HELD_BACK;
						c.deferred = bits;
					} else {
						fire(slot, bits);
					}
				}
				break; // The first eligible row decides.
			}
		}
		if (own && (table.slot_flags[slot] & SLOT_FALLBACK) == GamepadContext::FALLBACK_PASS) {
			pass = true;
		}
	}

	const uint8_t axis_bit = 1u << (uint8_t)p_axis;
	if (pass && !bound) {
		ps.passed_axes |= axis_bit;
		out.push_back(p_event);
	} else if (ps.passed_axes & axis_bit) {
		ps.passed_axes &= ~axis_bit;
		out.push_back(make_axis_zero(p_pad, p_axis));
	}
}

// Once per frame, with last values: hold and repeat, chord windows, sequence timeouts, the pointer.
void State::tick() {
	Frame &f = *write_state;
	const RowTable &rt = rows();
	const float dt = MIN((f.now - f.last_tick) / 1000000.0f, 0.1f);
	f.last_tick = f.now;
	Vector2 velocity;
	for (uint64_t held = f.claimed_slots; held; held &= held - 1) {
		const uint8_t slot = (uint8_t)lowest_bit(held);
		Claim &c = f.claims[slot];
		const RouteBinding &row = rt.rows[c.row];
		const uint32_t bits = trigger_step(row, c, c.value, f.now);
		if (c.bits & (CLAIM_HELD_BACK | CLAIM_GATE_DEFERRED)) {
			c.deferred |= bits;
		} else {
			fire(slot, bits);
		}
		if ((row.flags & ROW_POINTER) && is_stick_slot(slot)) {
			velocity += f.stick_vectors[slot - STICK_BASE] * row.pointer_speed;
		}
	}
	f.pointer_velocity = velocity;
	f.pointer_delta += velocity * dt;

	uint64_t expired = 0;
	uint32_t kept = 0;
	for (uint32_t i = 0; i < f.open_count; i++) {
		if (f.now >= f.open[i].until) {
			expired |= slot_bit(f.open[i].input);
		} else {
			f.open[kept++] = f.open[i];
		}
	}
	f.open_count = (uint8_t)kept;
	flush_held_back(expired);

	for (uint32_t i = 0; i < f.sequence_count; i++) {
		OpenMatch &m = f.sequences[i];
		if (m.step == 0 || f.now <= m.until) {
			continue; // Idle or in time: only a timeout reads the row.
		}
		const RouteBinding &row = rt.rows[m.row];
		if (sequence_step(row, m.step, m.until, NONE, f.now, sequence_timeout_usec) & FIRE_SEQUENCE_ABANDONED) {
			trace_event(row.input, f.active_pad, 0, 0.0f, m.row, row.context, OUTCOME_SEQUENCE_ABANDONED);
		}
	}
}

// Sequences observe every routed press and never claim. An idle one reads its row only when this
// press starts it. A completed one pulses its action.
void State::feed_sequences(uint8_t p_slot) {
	Frame &f = *write_state;
	const RowTable &rt = rows();
	for (uint32_t i = 0; i < f.sequence_count; i++) {
		OpenMatch &m = f.sequences[i];
		if (m.step == 0 && m.input != p_slot) {
			continue;
		}
		const RouteBinding &row = rt.rows[m.row];
		const uint32_t bits = sequence_step(row, m.step, m.until, p_slot, f.now, sequence_timeout_usec);
		if (bits & FIRE_SEQUENCE_ABANDONED) {
			trace_event(row.input, f.active_pad, 0, 0.0f, m.row, row.context, OUTCOME_SEQUENCE_ABANDONED);
		}
		if (!(bits & FIRE_PRESSED)) {
			continue;
		}
		queue_callback(m.row, IR::PHASE_PRESSED, Vector2(1.0f, 0.0f), false);
		const StringName &action = rt.info[m.row].action;
		if (action != StringName()) {
			emit_action(action, true, 1.0f, f.active_pad);
			if (!pulses.is_full()) {
				pulses.push_back(action);
			} else {
				emit_action(action, false, 0.0f, f.active_pad);
			}
		}
		trace_event(row.input, f.active_pad, IR::PHASE_PRESSED, 1.0f, m.row, row.context, OUTCOME_ROUTED);
	}
}

// --- State: chords ---

// Gated rows a press passed over; their input's claim stays held back until the window closes.
void State::open_chords(const uint16_t *p_rows, uint32_t p_count) {
	Frame &f = *write_state;
	const RowTable &rt = rows();
	const uint64_t until = f.now + chord_window_usec;
	for (uint32_t i = 0; i < p_count; i++) {
		if (f.open_count == MAX_OPEN) {
			ERR_PRINT_ONCE(vformat("InputRouter: more than %d chords waiting; a rolled chord may miss.", MAX_OPEN));
			return;
		}
		OpenMatch &m = f.open[f.open_count++];
		m = OpenMatch();
		m.row = p_rows[i];
		m.input = rt.rows[m.row].input;
		m.gate = rt.rows[m.row].gate;
		m.until = until;
	}
}

// Each waiting input's held-back claim ends silently; the first chord in range order claims it.
void State::complete_chords(int p_pad, uint8_t p_gate) {
	Frame &f = *write_state;
	if (!(routes().slot_flags[p_gate] & SLOT_CHORD_GATE) || f.open_count == 0) {
		return;
	}
	const RowTable &rt = rows();
	uint64_t done = 0;
	for (uint32_t i = 0; i < f.open_count; i++) {
		const OpenMatch m = f.open[i];
		if (m.gate != p_gate || (done & slot_bit(m.input))) {
			continue;
		}
		done |= slot_bit(m.input);
		use_gate(p_gate);
		end_claim(m.input);
		const RouteBinding &row = rt.rows[m.row];
		Claim &c = claim(m.input, m.row);
		Vector2 vec;
		const float v = shaped_for(row, p_pad, m.input, &vec);
		if (is_stick_slot(m.input)) {
			f.stick_vectors[m.input - STICK_BASE] = vec;
		}
		fire(m.input, trigger_step(row, c, v, f.now));
	}
	drop_chords(done);
}

// Ordered removal, so `open` stays in range order.
void State::drop_chords(uint64_t p_inputs) {
	if (!p_inputs) {
		return;
	}
	Frame &f = *write_state;
	uint32_t kept = 0;
	for (uint32_t i = 0; i < f.open_count; i++) {
		if (!(p_inputs & slot_bit(f.open[i].input))) {
			f.open[kept++] = f.open[i];
		}
	}
	f.open_count = (uint8_t)kept;
}

// A held-back claim with no chord left waiting on its input fires what it held.
void State::flush_held_back(uint64_t p_inputs) {
	Frame &f = *write_state;
	for (uint64_t pending = p_inputs & f.claimed_slots; pending; pending &= pending - 1) {
		const uint8_t slot = (uint8_t)lowest_bit(pending);
		Claim &c = f.claims[slot];
		if (!(c.bits & CLAIM_HELD_BACK)) {
			continue;
		}
		bool waiting = false;
		for (uint32_t i = 0; i < f.open_count; i++) {
			waiting = waiting || f.open[i].input == slot;
		}
		if (waiting) {
			continue;
		}
		c.bits &= ~CLAIM_HELD_BACK;
		const uint32_t bits = c.deferred;
		c.deferred = 0;
		fire(slot, bits);
	}
}

void State::use_gate(uint8_t p_gate) {
	Frame &f = *write_state;
	if ((f.claimed_slots & slot_bit(p_gate)) && (f.claims[p_gate].bits & CLAIM_GATE_DEFERRED)) {
		f.claims[p_gate].bits |= CLAIM_GATE_USED;
	}
}

// --- State: KB/M and touch ---

// True passes the event. KB/M passes while it owns the frame; passed keys and buttons are tracked
// for release on a switch. In a pad frame, mouse motion may feed the pointer.
bool State::route_kbm(const InputEvent *p_event, EventKind p_kind) {
	Frame &f = *write_state;
	const bool owns = f.device != IR::DEVICE_PAD;
	if (p_kind == KIND_KEY) {
		const InputEventKey *k = static_cast<const InputEventKey *>(p_event);
		const Key key = key_of(k);
		if (f.keys_suppressed.has(key)) {
			if (k->is_pressed() && !k->is_echo()) {
				f.keys_suppressed.erase(key); // Stale: its release was lost.
			} else {
				if (!k->is_pressed()) {
					f.keys_suppressed.erase(key);
				}
				return false;
			}
		}
		if (!owns) {
			return false;
		}
		if (k->is_pressed() && !k->is_echo()) {
			bool held = false;
			for (uint32_t i = 0; i < f.key_count; i++) {
				const HeldKey &h = f.keys_down[i];
				if ((h.physical_keycode != Key::NONE ? h.physical_keycode : h.keycode) == key) {
					held = true;
				}
			}
			if (!held && f.key_count < MAX_HELD_KEYS) {
				HeldKey &h = f.keys_down[f.key_count++];
				h.keycode = k->get_keycode();
				h.physical_keycode = k->get_physical_keycode();
				h.key_label = k->get_key_label();
				h.unicode = k->get_unicode();
				h.location = k->get_location();
				h.window_id = k->get_window_id();
			}
		} else if (!k->is_pressed()) {
			for (uint32_t i = 0; i < f.key_count; i++) {
				const HeldKey &h = f.keys_down[i];
				if ((h.physical_keycode != Key::NONE ? h.physical_keycode : h.keycode) == key) {
					f.keys_down[i] = f.keys_down[--f.key_count];
					break;
				}
			}
		}
		return true;
	}
	if (p_kind == KIND_MOUSE_BUTTON) {
		const InputEventMouseButton *mb = static_cast<const InputEventMouseButton *>(p_event);
		const uint32_t bit = mouse_bit(mb->get_button_index());
		if (f.mouse_suppressed & bit) {
			f.mouse_suppressed &= ~bit;
			if (!mb->is_pressed()) {
				return false;
			}
		}
		if (!owns) {
			return false;
		}
		f.mouse_down = mb->is_pressed() ? (f.mouse_down | bit) : (f.mouse_down & ~bit);
		f.mouse_position = mb->get_position();
		f.mouse_window = mb->get_window_id();
		return true;
	}
	if (p_kind == KIND_MOUSE_MOTION) {
		const InputEventMouseMotion *mm = static_cast<const InputEventMouseMotion *>(p_event);
		f.mouse_position = mm->get_position();
		f.mouse_window = mm->get_window_id();
		if (!owns) {
			if (mouse_in_pad == IR::MOUSE_IN_PAD_POINTER) {
				f.pointer_delta += mm->get_relative();
			}
			return false;
		}
		return true;
	}
	return owns;
}

// AS_POINTER passes in KB/M frames (Godot emulates the mouse). AS_PAD becomes touch_routed in pad
// frames. Suppressed touches are eaten until lifted.
bool State::route_touch(const InputEvent *p_event, EventKind p_kind) {
	Frame &f = *write_state;
	const InputEventScreenTouch *t = p_kind == KIND_TOUCH ? static_cast<const InputEventScreenTouch *>(p_event) : nullptr;
	const InputEventScreenDrag *d = p_kind == KIND_DRAG ? static_cast<const InputEventScreenDrag *>(p_event) : nullptr;
	const int index = t ? t->get_index() : d->get_index();
	int found = -1;
	for (uint32_t i = 0; i < f.touch_count; i++) {
		if (f.touches[i].index == index) {
			found = i;
			break;
		}
	}
	const bool lift = t && !t->is_pressed();
	if (found >= 0 && f.touches[found].suppressed) {
		if (lift) {
			f.touches[found] = f.touches[--f.touch_count];
		}
		return false;
	}
	if (touch_mode == IR::TOUCH_IGNORE) {
		return false;
	}

	const bool as_pad = touch_mode == IR::TOUCH_AS_PAD;
	if ((as_pad && f.device != IR::DEVICE_PAD) || (!as_pad && f.device == IR::DEVICE_PAD)) {
		return false;
	}
	const Vector2 position = t ? t->get_position() : d->get_position();
	if (t && t->is_pressed()) {
		if (found < 0) {
			if (f.touch_count == MAX_TOUCHES) {
				return false;
			}
			found = f.touch_count++;
			f.touches[found] = TouchState();
			f.touches[found].index = index;
		}
		f.touches[found].passed = !as_pad;
		f.touches[found].position = position;
	} else if (found < 0) {
		return false; // Drag or lift of a touch that began elsewhere.
	} else {
		f.touches[found].position = position;
	}

	if (as_pad) {
		const IR::Phase phase = t ? (t->is_pressed() ? IR::PHASE_PRESSED : (t->is_canceled() ? IR::PHASE_CANCELED : IR::PHASE_RELEASED)) : IR::PHASE_CHANGED;
		queue_touch(index, phase, position, d ? d->get_pressure() : (t && t->is_pressed() ? 1.0f : 0.0f));
	}
	if (lift) {
		f.touches[found] = f.touches[--f.touch_count];
	}
	return !as_pad;
}

// --- State: claims and output ---

Claim &State::claim(uint8_t p_slot, uint16_t p_row) {
	Frame &f = *write_state;
	Claim &c = f.claims[p_slot];
	c = Claim();
	c.row = p_row;
	if (is_stick_slot(p_slot)) {
		f.stick_vectors[p_slot - STICK_BASE] = Vector2();
	}
	f.claimed_slots |= slot_bit(p_slot);
	return c;
}

// Silent: no callback, no action release.
void State::end_claim(uint8_t p_slot) {
	Frame &f = *write_state;
	f.claims[p_slot].row = NO_ROW;
	f.claimed_slots &= ~slot_bit(p_slot);
}

// PHASE_CANCELED, an owed action release, and the input suppressed while held. Its chords close:
// a suppressed input never delivers the release that would end them.
void State::cancel_claim(uint8_t p_slot) {
	Frame &f = *write_state;
	const Claim c = f.claims[p_slot];
	end_claim(p_slot);
	drop_chords(slot_bit(p_slot));
	if (c.row == NO_ROW) {
		return;
	}
	if (!(c.bits & (CLAIM_HELD_BACK | CLAIM_GATE_DEFERRED))) {
		queue_callback(c.row, IR::PHASE_CANCELED, Vector2(), false);
	}
	if (c.bits & CLAIM_ACTION_DOWN) {
		emit_action(rows().info[c.row].action, false, 0.0f, f.active_pad);
	}
	trace_event(p_slot, f.active_pad, IR::PHASE_CANCELED, 0.0f, c.row, rows().rows[c.row].context, OUTCOME_CANCELED);
	const bool suppressible = is_button_slot(p_slot) || (is_axis_slot(p_slot) && slot_direction(p_slot) != 0); // Sticks and full axes resume.
	if (f.active_pad >= 0 && (f.live_pads & (1u << f.active_pad)) && suppressible && f.pads[f.active_pad].slot_value(p_slot) >= SUPPRESS_RELEASE) {
		f.pads[f.active_pad].suppressed |= slot_bit(p_slot);
	}
}

void State::cancel_claims() {
	Frame &f = *write_state;
	f.open_count = 0;
	while (f.claimed_slots) {
		cancel_claim((uint8_t)lowest_bit(f.claimed_slots));
	}
}

// Callbacks run after the next flip; actions go out with it. Pulses release the frame after.
void State::fire(uint8_t p_slot, uint32_t p_bits) {
	if (!p_bits) {
		return;
	}
	Frame &f = *write_state;
	const RowTable &rt = rows();
	Claim &c = f.claims[p_slot];
	const RouteBinding &row = rt.rows[c.row];
	const bool vector = is_stick_slot(p_slot);
	const Vector2 value = vector ? f.stick_vectors[p_slot - STICK_BASE] : Vector2(c.value, 0.0f);
	const float strength = MIN(vector ? value.length() : Math::abs(c.value), 1.0f);

	static const struct {
		uint32_t bit;
		IR::Phase phase;
	} order[] = {
		{ FIRE_PRESSED, IR::PHASE_PRESSED },
		{ FIRE_HELD, IR::PHASE_HELD },
		{ FIRE_TAPPED, IR::PHASE_TAPPED },
		{ FIRE_REPEATED, IR::PHASE_REPEATED },
		{ FIRE_CHANGED, IR::PHASE_CHANGED },
		{ FIRE_RELEASED, IR::PHASE_RELEASED },
	};
	for (const auto &o : order) {
		if (p_bits & o.bit) {
			queue_callback(c.row, o.phase, value, vector);
			if (o.bit != FIRE_CHANGED) {
				trace_event(p_slot, f.active_pad, o.phase, strength, c.row, row.context, OUTCOME_ROUTED);
			}
		}
	}

	const StringName &action = rt.info[c.row].action;
	if (action == StringName()) {
		return;
	}
	if (p_bits & FIRE_PRESSED) {
		emit_action(action, true, MAX(strength, 0.01f), f.active_pad);
		c.bits |= CLAIM_ACTION_DOWN;
	}
	if (p_bits & FIRE_CHANGED) {
		emit_action(action, strength > 0.0f, strength, f.active_pad);
		c.bits = strength > 0.0f ? (c.bits | CLAIM_ACTION_DOWN) : (c.bits & ~CLAIM_ACTION_DOWN);
	}
	// A RELEASE row's only edge is RELEASED, so it pulses too.
	const uint32_t pulse = FIRE_HELD | FIRE_TAPPED | FIRE_REPEATED | (row.trigger == RouteBindingResource::TRIGGER_RELEASE ? FIRE_RELEASED : 0);
	if ((p_bits & pulse) && !(c.bits & CLAIM_ACTION_DOWN)) {
		emit_action(action, true, 1.0f, f.active_pad);
		if (!pulses.is_full()) {
			pulses.push_back(action);
		} else {
			emit_action(action, false, 0.0f, f.active_pad);
		}
	}
	if ((p_bits & FIRE_RELEASED) && (c.bits & CLAIM_ACTION_DOWN)) {
		emit_action(action, false, 0.0f, f.active_pad);
		c.bits &= ~CLAIM_ACTION_DOWN;
	}
}

// lock_axis zeroes a stick axis while its partner is larger, so a diagonal drives one control.
float State::shaped_for(const RouteBinding &p_row, int p_pad, uint8_t p_slot, Vector2 *r_vector) const {
	const PadState &ps = write_state->pads[p_pad];
	const float *lut = p_row.lut == NO_LUT ? nullptr : luts.ptr() + p_row.lut * LUT_STRIDE;
	if (is_button_slot(p_slot)) {
		return ps.is_down(p_slot) ? 1.0f : 0.0f;
	}
	if (is_axis_slot(p_slot)) {
		const JoyAxis axis = slot_axis(p_slot);
		float raw = ps.axes[(int)axis];
		if ((p_row.flags & ROW_LOCK_AXIS) && Math::abs(ps.axes[(int)partner_axis(axis)]) > Math::abs(raw)) {
			raw = 0.0f;
		}
		return shape_axis(p_row, lut, raw, slot_direction(p_slot));
	}
	const Vector2 shaped = shape_stick(p_row, lut, ps.stick(p_slot - STICK_BASE));
	if (r_vector) {
		*r_vector = shaped;
	}
	return shaped.length();
}

// Takes a pooled event only the pool still holds: Input has dispatched it and nobody kept it.
// Falls back to a new event while every pooled one is in flight.
void State::emit_action(const StringName &p_action, bool p_pressed, float p_strength, int p_device) {
	Ref<InputEventAction> ev;
	for (uint32_t n = 0; n < action_pool.size() && ev.is_null(); n++) {
		const Ref<InputEventAction> &pooled = action_pool[action_pool_next];
		action_pool_next = (action_pool_next + 1) % action_pool.size();
		if (pooled->get_reference_count() == 1) {
			ev = pooled;
		}
	}
	if (ev.is_null()) {
		ev = make_action_event();
	}
	ev->set_event_index(-1);
	ev->set_action(p_action);
	ev->set_pressed(p_pressed);
	ev->set_strength(p_pressed ? CLAMP(p_strength, 0.0f, 1.0f) : 0.0f);
	ev->set_device(p_device);
	out.push_back(ev);
}

// Into the write frame's BODY. Nothing queues while disabled: no frame would run it.
Queued *State::push_queued(bool p_signal) {
	Frame &f = *write_state;
	if (!f.enabled) {
		return nullptr;
	}
	if (f.queued_count == MAX_QUEUED) {
		ERR_PRINT_ONCE(vformat("InputRouter: more than %d callbacks and signals in one frame; dropping.", MAX_QUEUED));
		return nullptr;
	}
	Queued *q = &f.queued[f.queued_count++];
	*q = Queued();
	q->signal = p_signal;
	return q;
}

void State::queue_callback(uint16_t p_row, IR::Phase p_phase, const Vector2 &p_value, bool p_vector) {
	const RouteBindingInfo &ri = rows().info[p_row];
	if (!ri.target.is_valid() && ri.named < 0) {
		return;
	}
	Queued *q = push_queued(false);
	if (q) {
		q->code = (uint8_t)p_phase;
		q->row = p_row;
		q->i[0] = (int32_t)ri.handle;
		q->i[1] = p_vector ? 1 : 0;
		q->v = p_value;
	}
}

void State::queue_touch(int32_t p_index, IR::Phase p_phase, const Vector2 &p_position, float p_pressure) {
	Queued *q = push_queued(true);
	if (q) {
		q->code = SIGNAL_TOUCH_ROUTED;
		q->i[0] = p_index;
		q->i[1] = (int32_t)p_phase;
		q->v = p_position;
		q->f = p_pressure;
	}
}

// Context, routes and scheme changes collapse to one per frame.
void State::queue_signal(SignalKind p_kind, int32_t p_a, int32_t p_b, int32_t p_c, int32_t p_d) {
	const bool collapses = p_kind == SIGNAL_CONTEXT_CHANGED || p_kind == SIGNAL_ROUTES_CHANGED || p_kind == SIGNAL_SCHEME_CHANGED;
	if (collapses && (write_state->queued_signals & (1u << p_kind))) {
		return;
	}
	Queued *q = push_queued(true);
	if (!q) {
		return;
	}
	if (collapses) {
		write_state->queued_signals |= 1u << p_kind;
	}
	q->code = p_kind;
	q->i[0] = p_a;
	q->i[1] = p_b;
	q->i[2] = p_c;
	q->i[3] = p_d;
}

// Skipped if the row died or was reused (handle mismatch) in the write frame's rows, which an
// earlier callback may have changed; rechecked after the first call, which may change them too.
void State::run_callback(const Queued &p_queued) {
	if (p_queued.row >= rows().info.size() || rows().info[p_queued.row].handle != (uint32_t)p_queued.i[0]) {
		return;
	}
	const Variant value = p_queued.i[1] ? Variant(p_queued.v) : Variant(p_queued.v.x);
	const Callable target = rows().info[p_queued.row].target;
	if (target.is_valid()) {
		target.call((int)p_queued.code, value);
	}
	const RowTable &rt = rows();
	if (p_queued.row >= rt.info.size() || rt.info[p_queued.row].handle != (uint32_t)p_queued.i[0]) {
		return;
	}
	const int named = rt.info[p_queued.row].named;
	if (named >= 0 && named < (int)named_callbacks.size()) {
		const Callable named_target = named_callbacks[named].target;
		if (named_target.is_valid()) {
			named_target.call((int)p_queued.code, value);
		}
	}
}

void State::run_signal(const Queued &p_queued) {
	switch ((SignalKind)p_queued.code) {
		case SIGNAL_DEVICE_CHANGED:
			owner->emit_signal(SNAME("device_changed"), p_queued.i[0], p_queued.i[1], p_queued.i[2], p_queued.i[3]);
			break;
		case SIGNAL_PAD_CONNECTED:
			owner->emit_signal(SNAME("pad_connected"), p_queued.i[0]);
			break;
		case SIGNAL_PAD_DISCONNECTED:
			owner->emit_signal(SNAME("pad_disconnected"), p_queued.i[0]);
			break;
		case SIGNAL_ACTIVE_PAD_LOST:
			owner->emit_signal(SNAME("active_pad_lost"), p_queued.i[0]);
			break;
		case SIGNAL_CONTEXT_CHANGED:
			owner->emit_signal(SNAME("gamepad_context_changed"), owner->get_gamepad_context_path());
			break;
		case SIGNAL_ROUTES_CHANGED:
			owner->emit_signal(SNAME("routes_changed"));
			break;
		case SIGNAL_SCHEME_CHANGED:
			owner->emit_signal(SNAME("scheme_changed"), owner->get_scheme());
			break;
		case SIGNAL_TOUCH_ROUTED:
			owner->emit_signal(SNAME("touch_routed"), p_queued.i[0], p_queued.i[1], p_queued.v, p_queued.f);
			break;
	}
}

// --- State: contexts, rows, routes ---

// An id used before its resource was registered acts as FALLBACK_INHERIT, non-modal.
int State::ensure_context(const StringName &p_id) {
	ERR_FAIL_COND_V_MSG(p_id == StringName(), -1, "InputRouter: empty gamepad context id.");
	const NameTable &nt = names();
	for (uint32_t i = 0; i < nt.contexts.size(); i++) {
		if (nt.contexts[i].id == p_id) {
			return i;
		}
	}
	ERR_FAIL_COND_V_MSG(nt.contexts.is_full(), -1, vformat("InputRouter: more than %d gamepad contexts.", MAX_CONTEXTS));
	WARN_VERBOSE(vformat("InputRouter: gamepad context '%s' used before registration; inheriting.", p_id));
	ContextInfo ci;
	ci.id = p_id;
	NameTable &edit = edit_names();
	edit.contexts.push_back(ci);
	return edit.contexts.size() - 1;
}

// Empty: every scheme. A new name takes the next free bit.
int State::scheme_mask_of(const StringName &p_scheme) {
	if (p_scheme == StringName()) {
		return 0xFF;
	}
	const NameTable &nt = names();
	for (uint32_t i = 0; i < nt.schemes.size(); i++) {
		if (nt.schemes[i] == p_scheme) {
			return 1 << i;
		}
	}
	ERR_FAIL_COND_V_MSG(nt.schemes.is_full(), -1, vformat("InputRouter: more than %d schemes.", MAX_SCHEMES));
	NameTable &edit = edit_names();
	edit.schemes.push_back(p_scheme);
	return 1 << (edit.schemes.size() - 1);
}

StringName State::scheme_of_mask(uint8_t p_mask) const {
	if (p_mask == 0xFF || p_mask == 0) {
		return StringName();
	}
	const NameTable &nt = names();
	const uint32_t bit = lowest_bit(p_mask);
	return bit < nt.schemes.size() ? nt.schemes[bit] : StringName();
}

// Authored input and gate, then the player's rebind if it fits: stick for stick.
void State::apply_override(RouteBinding &r_row, const StringName &p_id) const {
	r_row.input = r_row.authored_input;
	r_row.gate = r_row.authored_gate;
	for (const Override &o : overrides) {
		if (o.binding_id == p_id && is_stick_slot(o.input) == is_stick_slot(r_row.authored_input)) {
			r_row.input = o.input;
			r_row.gate = o.gate;
			return;
		}
	}
}

// The id's override changed: claims its rows hold cancel, then the rows take the new input.
void State::reapply_override(const StringName &p_id) {
	Frame &f = *write_state;
	for (uint64_t held = f.claimed_slots; held; held &= held - 1) {
		const uint8_t slot = (uint8_t)lowest_bit(held);
		if (rows().info[f.claims[slot].row].id == p_id) {
			cancel_claim(slot);
		}
	}
	RowTable &rt = edit_rows();
	for (uint32_t r = 0; r < rt.rows.size(); r++) {
		if (rt.rows[r].input != NONE && rt.info[r].id == p_id) {
			apply_override(rt.rows[r], p_id);
		}
	}
	routes_dirty = true;
	queue_signal(SIGNAL_ROUTES_CHANGED);
}

// Reuses a tombstone first.
uint16_t State::add_row(const RouteBinding &p_row, const RouteBindingInfo &p_info) {
	RowTable &rt = edit_rows();
	uint16_t r = NO_ROW;
	if (!rt.free_rows.is_empty()) {
		r = rt.free_rows[rt.free_rows.size() - 1];
		rt.free_rows.resize(rt.free_rows.size() - 1);
	} else {
		ERR_FAIL_COND_V_MSG(rt.rows.size() >= MAX_ROWS, NO_ROW, vformat("InputRouter: more than %d attached bindings.", MAX_ROWS));
		r = (uint16_t)rt.rows.size();
		rt.rows.push_back(RouteBinding());
		rt.info.push_back(RouteBindingInfo());
	}
	rt.rows[r] = p_row;
	rt.info[r] = p_info;
	apply_override(rt.rows[r], p_info.id);
	routes_dirty = true;
	callbacks_dirty = true;
	queue_signal(SIGNAL_ROUTES_CHANGED);
	return r;
}

// Tombstone. Its chords close and its claim cancels first.
void State::kill_row(uint16_t p_row) {
	Frame &f = *write_state;
	uint64_t inputs = 0;
	uint32_t kept = 0;
	for (uint32_t i = 0; i < f.open_count; i++) {
		if (f.open[i].row == p_row) {
			inputs |= slot_bit(f.open[i].input);
		} else {
			f.open[kept++] = f.open[i];
		}
	}
	f.open_count = (uint8_t)kept;
	for (uint64_t held = f.claimed_slots; held; held &= held - 1) {
		const uint8_t slot = (uint8_t)lowest_bit(held);
		if (f.claims[slot].row == p_row) {
			cancel_claim(slot);
		}
	}
	flush_held_back(inputs);
	kept = 0;
	for (uint32_t i = 0; i < f.sequence_count; i++) {
		if (f.sequences[i].row != p_row) {
			f.sequences[kept++] = f.sequences[i];
		}
	}
	f.sequence_count = (uint8_t)kept;
	RowTable &rt = edit_rows();
#ifdef TOOLS_ENABLED
	watch_binding(rt.info[p_row].source, false);
#endif
	rt.rows[p_row] = RouteBinding();
	rt.info[p_row] = RouteBindingInfo();
	rt.free_rows.push_back(p_row);
	routes_dirty = true;
	queue_signal(SIGNAL_ROUTES_CHANGED);
}

// One row for attach(), a map's worth for load_map(), and placements still waiting on a merge.
// kill_row() may move the write frame to its own copy of the rows, so each pass looks it up.
bool State::release_handle(uint32_t p_handle) {
	if (p_handle == 0) {
		return false;
	}
	bool found = false;
	for (uint32_t r = 0; r < rows().rows.size(); r++) {
		if (rows().info[r].handle == p_handle) {
			kill_row((uint16_t)r);
			found = true;
		}
	}
	for (uint32_t i = 0; i < pending_placements.size();) {
		if (pending_placements[i].handle == p_handle) {
#ifdef TOOLS_ENABLED
			watch_binding(pending_placements[i].source, false);
#endif
			pending_placements.remove_at(i);
			found = true;
		} else {
			i++;
		}
	}
	return found;
}

// Routable: inner to outer, up to the innermost modal. Claims outside it cancel.
void State::path_changed() {
	Frame &f = *write_state;
	const NameTable &nt = names();
	uint64_t routable = 0;
	f.routable_from = 0;
	for (int i = f.path_size - 1; i >= 0; i--) {
		routable |= uint64_t(1) << f.path[i].context;
		if (nt.contexts[f.path[i].context].modal) {
			f.routable_from = (uint8_t)i;
			break;
		}
	}
	for (uint64_t held = f.claimed_slots; held; held &= held - 1) {
		const uint8_t slot = (uint8_t)lowest_bit(held);
		if (!(routable & (uint64_t(1) << rows().rows[f.claims[slot].row].context))) {
			cancel_claim(slot);
		}
	}
	f.path_version++;
	routes_dirty = true;
	queue_signal(SIGNAL_CONTEXT_CHANGED);
	queue_signal(SIGNAL_ROUTES_CHANGED);
	record_path();
}

// Path, rows and scheme to active, slot_first and slot_flags. Per slot, contexts inner to outer;
// one stops the walk at its first plain row, when modal, or when its fallback is not INHERIT.
// The stopping context's fallback applies; PASS past the outermost. Named callbacks resolve first.
void State::rebuild_routes() {
	routes_dirty = false;
	Frame &f = *write_state;
	const NameTable &nt = names();
	if (callbacks_dirty) {
		callbacks_dirty = false;
		RowTable &edit = edit_rows();
		for (uint32_t r = 0; r < edit.rows.size(); r++) {
			RouteBindingInfo &ri = edit.info[r];
			ri.named = -1;
			if (edit.rows[r].input == NONE) {
				continue;
			}
			const StringName &ctx = nt.contexts[edit.rows[r].context].id;
			for (uint32_t n = 0; n < named_callbacks.size(); n++) {
				if (named_callbacks[n].binding_id == ri.id && named_callbacks[n].context == ctx) {
					ri.named = (int16_t)n;
					break;
				}
			}
		}
	}
	const RowTable &rt = rows();

	uint8_t depth[MAX_CONTEXTS];
	memset(depth, 0xFF, sizeof(depth));
	uint8_t order[MAX_PATH_DEPTH];
	uint32_t order_count = 0;
	for (int i = f.path_size - 1; i >= 0; i--) {
		const uint8_t ctx = f.path[i].context;
		if (depth[ctx] == 0xFF) {
			depth[ctx] = (uint8_t)order_count;
			order[order_count++] = ctx;
		}
		if (nt.contexts[ctx].modal) {
			break;
		}
	}

	f.open_count = 0;
	flush_held_back(f.claimed_slots);

	const uint8_t scheme_bit = f.scheme >= 0 ? (uint8_t)(1 << f.scheme) : 0;
	sort_keys.clear();
	f.sequence_count = 0;
	for (uint32_t r = 0; r < rt.rows.size(); r++) {
		const RouteBinding &row = rt.rows[r];
		if (row.input == NONE || row.context >= MAX_CONTEXTS || depth[row.context] == 0xFF) {
			continue;
		}
		if (row.scheme_mask != 0xFF && !(row.scheme_mask & scheme_bit)) {
			continue;
		}
		if (row.trigger == RouteBindingResource::TRIGGER_SEQUENCE) {
			if (f.sequence_count == MAX_SEQUENCES) {
				ERR_PRINT_ONCE(vformat("InputRouter: more than %d sequences on the path.", MAX_SEQUENCES));
				continue;
			}
			OpenMatch &m = f.sequences[f.sequence_count++];
			m = OpenMatch();
			m.row = (uint16_t)r;
			m.input = row.input;
			continue;
		}
		sort_keys.push_back(active_key(row, depth[row.context], (uint16_t)r));
	}
	sort_keys.sort();

	RouteTable &table = edit_routes();
	uint32_t active_count = 0;
	uint32_t k = 0;
	for (uint8_t slot = 0; slot < INPUT_COUNT; slot++) {
		table.slot_first[slot] = (uint8_t)active_count;
		uint8_t fallback = GamepadContext::FALLBACK_PASS;
		uint32_t end = k;
		while (end < sort_keys.size() && key_slot(sort_keys[end]) == slot) {
			end++;
		}
		for (uint32_t o = 0; o < order_count; o++) {
			bool plain = false;
			for (; k < end && key_depth(sort_keys[k]) == o; k++) {
				const uint16_t r = key_row(sort_keys[k]);
				if (plain) {
					continue; // Below the context's first plain row: unreachable.
				}
				if (active_count == MAX_ACTIVE) {
					ERR_PRINT_ONCE(vformat("InputRouter: more than %d routed bindings on the path.", MAX_ACTIVE));
					continue;
				}
				table.active[active_count++] = r;
				plain = rt.rows[r].gate == NONE;
			}
			const ContextInfo &ci = nt.contexts[order[o]];
			if (plain) {
				break;
			}
			if (ci.modal) {
				fallback = ci.fallback == GamepadContext::FALLBACK_INHERIT ? GamepadContext::FALLBACK_SWALLOW : ci.fallback;
				break;
			}
			if (ci.fallback != GamepadContext::FALLBACK_INHERIT) {
				fallback = ci.fallback;
				break;
			}
		}
		k = end;
		table.slot_flags[slot] = fallback;
	}
	table.slot_first[INPUT_COUNT] = (uint8_t)active_count;

	for (uint32_t i = 0; i < active_count; i++) {
		const uint16_t r = table.active[i];
		if (rt.rows[r].gate != NONE) {
			table.slot_flags[rt.rows[r].gate] |= SLOT_CHORD_GATE;
		}
	}
}

// --- State: route map ---

// The project's map (input_devices/router/baked_map), once, before the first row, in every
// build. Unreadable or stale, the router starts from an empty map. The read frame keeps its own
// tables until the next flip, so nothing flips here.
void State::ensure_map() {
	if (map_tried) {
		return;
	}
	map_tried = true;
	pending_map = Baked::empty();
	const String file = GLOBAL_GET("input_devices/router/baked_map");
	if (!file.is_empty() && ResourceLoader::exists(file)) {
		const Ref<GamepadBakedMaps> map = ResourceLoader::load(file);
		const String invalid = map.is_valid() ? Baked::validate(map->get_data()) : String("not a GamepadBakedMaps");
		if (invalid.is_empty()) {
			pending_map = map->get_data();
		} else {
			ERR_PRINT(vformat("InputRouter: route map '%s': %s; starting empty. Rebake.", file, invalid));
		}
	}
	rebuild_from_map();
}

// The caller validated p_data and cleared every row.
void State::load_tables(const Dictionary &p_data) {
	map_data = p_data;
	const PackedStringArray scheme_names = p_data[Baked::KEY_SCHEMES];
	const PackedStringArray context_ids = p_data[Baked::KEY_CONTEXT_IDS];
	const PackedInt32Array context_info = p_data[Baked::KEY_CONTEXT_INFO];
	const PackedStringArray ids = p_data[Baked::KEY_IDS];
	const PackedStringArray actions = p_data[Baked::KEY_ACTIONS];
	const PackedFloat32Array lut_values = p_data[Baked::KEY_LUTS];
	NameTable &nt = edit_names();
	nt.schemes.clear();
	for (int i = 0; i < scheme_names.size(); i++) {
		nt.schemes.push_back(scheme_names[i]);
	}
	nt.contexts.clear();
	for (int i = 0; i < context_ids.size(); i++) {
		ContextInfo ci;
		ci.id = context_ids[i];
		ci.fallback = (uint8_t)context_info[i * 2];
		ci.modal = context_info[i * 2 + 1] != 0;
		nt.contexts.push_back(ci);
	}
	templates = p_data[Baked::KEY_ROWS];
	template_labels = p_data[Baked::KEY_LABELS];
	template_ids.resize(ids.size());
	template_actions.resize(ids.size());
	for (int i = 0; i < ids.size(); i++) {
		template_ids[i] = ids[i];
		template_actions[i] = actions[i];
	}
	luts.resize(lut_values.size());
	if (lut_values.size()) {
		memcpy(luts.ptr(), lut_values.ptr(), sizeof(float) * lut_values.size());
	}
}

// _poll(): destroys and recreates what the map owns. Pending bindings merge into the pending (or
// current) map first. The pad side ends as on a device switch (keyboard, mouse and touch keep
// their state: the map never routes them), every row goes and the tables reload;
// then placements, the path, the scheme and contexts the map lacks come back by name. Overrides
// and named callbacks are keyed by name already. A map that fails validation changes nothing.
void State::rebuild_from_map() {
	Dictionary data = (pending_map.is_empty() ? map_data : pending_map).duplicate();
	pending_map = Dictionary();
	if (data.is_empty()) {
		data = Baked::empty();
	}
	for (const Ref<RouteBindingResource> &b : pending_bindings) {
		String error;
		if (Baked::add_binding(data, **b, error) == Baked::ADD_FAILED) {
			ERR_PRINT(vformat("InputRouter: binding '%s': %s.", b->get_id(), error));
		}
	}
	pending_bindings.clear();
	const String invalid = Baked::validate(data);
	if (!invalid.is_empty()) {
		ERR_PRINT(vformat("InputRouter: route map refused: %s; keeping the current one.", invalid));
		return;
	}

	Frame &f = *write_state;
	const RowTable &rt = rows();
	const NameTable &nt = names();
	LocalVector<Placement> placements;
	for (uint32_t r = 0; r < rt.rows.size(); r++) {
		if (rt.rows[r].input == NONE) {
			continue;
		}
		Placement p;
		p.id = rt.info[r].id;
		p.scheme = scheme_of_mask(rt.rows[r].scheme_mask);
		p.context = nt.contexts[rt.rows[r].context].id;
		p.target = rt.info[r].target;
		p.handle = rt.info[r].handle;
#ifdef TOOLS_ENABLED
		p.source = rt.info[r].source;
#endif
		placements.push_back(p);
	}
	for (const Placement &p : pending_placements) {
		placements.push_back(p);
	}
	pending_placements.clear();
	const FixedVector<ContextInfo, MAX_CONTEXTS> old_contexts = nt.contexts;
	FixedVector<StringName, MAX_PATH_DEPTH> path_ids;
	for (uint32_t i = 0; i < f.path_size; i++) {
		path_ids.push_back(nt.contexts[f.path[i].context].id);
	}
	const StringName scheme_name = f.scheme >= 0 ? nt.schemes[f.scheme] : StringName();

	// rt and nt are not read again: the edits below move the write frame to its own copies. The
	// routes stay stale until path_changed() marks them dirty; nothing routes before the rebuild.
	release_pads(true);
	clear_rows();
	f.sequence_count = 0;
	load_tables(data);

	NameTable &edit = edit_names();
	for (const ContextInfo &old : old_contexts) {
		bool kept = false;
		for (const ContextInfo &ci : edit.contexts) {
			kept = kept || ci.id == old.id;
		}
		if (!kept && !edit.contexts.is_full()) {
			edit.contexts.push_back(old);
		}
	}
	f.scheme = -1;
	if (scheme_name != StringName()) {
		const int mask = scheme_mask_of(scheme_name);
		f.scheme = mask > 0 ? (int8_t)lowest_bit((uint64_t)mask) : -1;
	}
	uint32_t path_kept = 0;
	for (uint32_t i = 0; i < f.path_size; i++) {
		const int ctx = ensure_context(path_ids[i]);
		if (ctx >= 0) {
			f.path[path_kept] = f.path[i];
			f.path[path_kept].context = (uint8_t)ctx;
			path_kept++;
		}
	}
	f.path_size = (uint8_t)path_kept;
	for (const Placement &p : placements) {
		if (!place(p)) {
			ERR_PRINT(vformat("InputRouter: binding '%s' is not in the route map; its placement is dropped.", p.id));
#ifdef TOOLS_ENABLED
			watch_binding(p.source, false);
#endif
		}
	}
	callbacks_dirty = true;
	path_changed();
}

// StringName compares; only an id match copies its row out.
int State::find_template(const StringName &p_id, uint8_t p_scheme_mask, RouteBinding &r_row) const {
	for (uint32_t i = 0; i < template_ids.size(); i++) {
		if (template_ids[i] != p_id) {
			continue;
		}
		memcpy(&r_row, templates.ptr() + i * sizeof(RouteBinding), sizeof(RouteBinding));
		if (r_row.scheme_mask == p_scheme_mask) {
			return i;
		}
	}
	return -1;
}

// A row from the map's template in the placement's context. False only when the map lacks it;
// a full table has already reported.
bool State::place(const Placement &p_placement) {
	const int mask = scheme_mask_of(p_placement.scheme);
	const int ctx = ensure_context(p_placement.context);
	if (mask < 0 || ctx < 0) {
		return true;
	}
	RouteBinding row;
	const int t = find_template(p_placement.id, (uint8_t)mask, row);
	if (t < 0) {
		return false;
	}
	row.context = (uint8_t)ctx;
	RouteBindingInfo ri;
	ri.id = p_placement.id;
	ri.action = template_actions[t];
	ri.label = template_labels[t];
	ri.target = p_placement.target;
	ri.handle = p_placement.handle;
#ifdef TOOLS_ENABLED
	ri.source = p_placement.source;
#endif
	add_row(row, ri);
	return true;
}

// The next frame merges p_binding into the map and rebuilds. Each id says so once: the editor
// build asks for a rebake, an export build reports a stale export.
void State::queue_binding(const Ref<RouteBindingResource> &p_binding, const char *p_why) {
	for (const Ref<RouteBindingResource> &b : pending_bindings) {
		if (b == p_binding) {
			return;
		}
	}
	pending_bindings.push_back(p_binding);
	const StringName id = p_binding->get_id();
	for (const StringName &w : warned_ids) {
		if (w == id) {
			return;
		}
	}
	if (!warned_ids.is_full()) {
		warned_ids.push_back(id);
	}
#ifdef TOOLS_ENABLED
	WARN_PRINT(vformat("InputRouter: binding '%s' %s the route map; compiled into a new map. Rebake it.", id, p_why));
#else
	ERR_PRINT(vformat("InputRouter: binding '%s' %s the route map; compiled into a new map. Re-export.", id, p_why));
#endif
}

#ifdef TOOLS_ENABLED
// Live tuning. Each placement, a row or a pending one, holds one reference on its resource's
// reference-counted changed connection: attach takes it, whatever ends the placement drops it, so
// a resource placed twice stays watched until its last placement goes.
void State::watch_binding(ObjectID p_source, bool p_watch) {
	Resource *res = Object::cast_to<Resource>(ObjectDB::get_instance(p_source));
	if (!res) {
		return;
	}
	const Callable changed = binding_changed.bind(p_source);
	if (p_watch) {
		res->connect_changed(changed, Object::CONNECT_REFERENCE_COUNTED);
	} else {
		res->disconnect_changed(changed);
	}
}
#endif

// --- State: trace ---

void State::trace_event(uint8_t p_input, int p_pad, uint8_t p_phase, float p_value, int p_row, uint8_t p_context, Outcome p_outcome) {
	if (!trace_enabled || trace.is_empty()) {
		return;
	}
	const Frame &f = *write_state;
	TraceRecord &r = trace[trace_head];
	r.usec = f.now;
	r.frame = (uint32_t)f.frame;
	r.path_version = f.path_version;
	r.row = (int16_t)p_row;
	r.pad = (int8_t)p_pad;
	r.input = p_input;
	r.phase = p_phase;
	r.outcome = p_outcome;
	r.context = p_context;
	r.value = p_value;
	trace_head = (trace_head + 1) % trace.size();
	trace_count = MIN(trace_count + 1, trace.size());
}

void State::record_path() {
	if (!trace_enabled || path_history.is_empty()) {
		return;
	}
	const Frame &f = *write_state;
	const NameTable &nt = names();
	PackedStringArray ids;
	for (uint32_t i = 0; i < f.path_size; i++) {
		ids.push_back(nt.contexts[f.path[i].context].id);
	}
	path_history[path_history_head] = Pair<uint32_t, PackedStringArray>(f.path_version, ids);
	path_history_head = (path_history_head + 1) % path_history.size();
}

// --- InputRouter ---

// State is line-aligned, so it takes an aligned allocation. Auto-enable rides the main
// MessageQueue; --test has none, so the router stays off there.
InputRouter::InputRouter() {
	singleton = this;
	s = memnew_placement(Memory::alloc_aligned_static(sizeof(State), alignof(State)), State);
	s->owner = this;
#ifdef TOOLS_ENABLED
	s->binding_changed = callable_mp(this, &InputRouter::_binding_changed);
#endif
	GLOBAL_DEF("input_devices/router/enabled", false);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "input_devices/router/baked_map", PROPERTY_HINT_FILE, "*.res"), "res://gamepad_baked_maps.res");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "input_devices/router/glyph_provider", PROPERTY_HINT_FILE, "*.tres,*.res"), "");
	if (MessageQueue::get_main_singleton()) {
		MessageQueue::get_main_singleton()->push_callable(callable_mp(this, &InputRouter::_auto_enable));
	}
}

InputRouter::~InputRouter() {
	if (Input::get_singleton()) {
		Input::get_singleton()->clear_frame_filter(this);
	}
	s->~State();
	Memory::free_aligned_static(s);
	singleton = nullptr;
}

void InputRouter::_defer(const Callable &p_call) {
	CallQueue *queue = MessageQueue::get_main_singleton();
	ERR_FAIL_NULL_MSG(queue, "InputRouter: no main MessageQueue, call dropped.");
	queue->push_callable(p_call);
}

// First main loop iteration, when the editor hint is reliable.
void InputRouter::_auto_enable() {
	if (Engine::get_singleton()->is_editor_hint() || !GLOBAL_GET("input_devices/router/enabled")) {
		return;
	}
	set_enabled(true);
}

// Both land at the next frame, which publishes them with its one flip: enable() writes the write
// frame, a disable replaces that frame.
void InputRouter::set_enabled(bool p_enabled) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_enabled).bind(p_enabled));
	if (!p_enabled) {
		s->disable_requested = s->write_state->enabled;
		return;
	}
	s->disable_requested = false;
	s->ensure_map();
	s->enable();
}

bool InputRouter::is_enabled() const {
	return s->read_frame()->enabled;
}

// --- Device ---

InputRouter::Device InputRouter::get_device() const {
	return (Device)s->read_frame()->device;
}

int InputRouter::get_active_pad() const {
	return s->read_frame()->active_pad;
}

bool InputRouter::is_switch_frame() const {
	return s->read_frame()->switch_frame;
}

// Same hand-off as a wake: claims cancel, passed input releases, held input is suppressed. Getters
// see it after the next flip.
void InputRouter::set_device(Device p_device, int p_pad) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_device).bind(p_device, p_pad));
	ERR_FAIL_INDEX(p_device, 3);
	ERR_FAIL_COND(p_pad >= Input::JOYPADS_MAX);
	s->switch_to(p_device, p_pad);
}

// If the device in charge is no longer allowed, the hand-off runs now.
void InputRouter::set_device_policy(DevicePolicy p_policy) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_device_policy).bind(p_policy));
	ERR_FAIL_INDEX(p_policy, 3);
	Frame &f = *s->write_state;
	f.policy = p_policy;
	if (p_policy == DEVICE_POLICY_PAD && f.device != DEVICE_PAD) {
		s->switch_to(DEVICE_PAD, f.active_pad);
	} else if (p_policy == DEVICE_POLICY_KBM && f.device != DEVICE_KBM) {
		s->switch_to(DEVICE_KBM, -1);
	}
}

InputRouter::DevicePolicy InputRouter::get_device_policy() const {
	return (DevicePolicy)s->read_frame()->policy;
}

void InputRouter::set_mouse_in_pad(MouseInPad p_mode) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_mouse_in_pad).bind(p_mode));
	s->mouse_in_pad = p_mode;
}

InputRouter::MouseInPad InputRouter::get_mouse_in_pad() const {
	MAIN_OR_RETURN(MOUSE_IN_PAD_DROP);
	return s->mouse_in_pad;
}

void InputRouter::set_trigger_wake_threshold(float p_threshold) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_trigger_wake_threshold).bind(p_threshold));
	s->trigger_wake_threshold = CLAMP(p_threshold, 0.05f, 1.0f);
}

float InputRouter::get_trigger_wake_threshold() const {
	MAIN_OR_RETURN(0.5f);
	return s->trigger_wake_threshold;
}

// --- Pad identity ---

// A pad with live state but no reported connection keeps PadState's GENERIC.
InputRouter::PadFamily InputRouter::get_pad_family(int p_pad) const {
	const PadState *pad = pad_of(s->read_frame(), p_pad);
	return pad ? (PadFamily)pad->family : PAD_FAMILY_GENERIC;
}

// The read frame's pad, if connected. Main thread: the read frame holds until the next flip, and
// its NameTable copy has the pad's name.
static int connected_pad(const FrameHead &p_read, int p_pad) {
	const int pad = p_pad < 0 ? p_read.active_pad : p_pad;
	return pad >= 0 && pad < Input::JOYPADS_MAX && (p_read.connected_pads & (1u << pad)) ? pad : -1;
}

String InputRouter::get_pad_name(int p_pad) const {
	MAIN_OR_RETURN(String());
	const Frame &f = *s->read_state;
	const int pad = connected_pad(f, p_pad);
	return pad >= 0 ? s->names_of(f).pad_info[pad].name : String();
}

// {device, name, guid, sdl_type, vendor_id, product_id, steam_input, family}. -1: active pad.
Dictionary InputRouter::_get_pad_info(int p_pad) const {
	MAIN_OR_RETURN(Dictionary());
	const Frame &f = *s->read_state;
	const int pad = connected_pad(f, p_pad);
	Dictionary d;
	if (pad >= 0) {
		const PadInfo &info = s->names_of(f).pad_info[pad];
		d["device"] = pad;
		d["name"] = info.name;
		d["guid"] = info.guid;
		d["sdl_type"] = info.sdl_type;
		d["vendor_id"] = info.vendor_id;
		d["product_id"] = info.product_id;
		d["steam_input"] = info.steam_input;
		d["family"] = f.pads[pad].family;
	}
	return d;
}

PackedInt32Array InputRouter::_get_connected_pads() const {
	PackedInt32Array result;
	for (uint32_t bits = s->read_frame()->connected_pads; bits; bits &= bits - 1) {
		result.push_back(lowest_bit(bits));
	}
	return result;
}

String InputRouter::get_input_label(int p_input, PadFamily p_family) {
	ERR_FAIL_INDEX_V(p_input, INPUT_COUNT, String());
	return input_label((uint8_t)p_input, p_family);
}

// The project's GamepadGlyphProvider (input_devices/router/glyph_provider), loaded once.
Ref<Texture2D> InputRouter::get_input_glyph(int p_input, PadFamily p_family) {
	MAIN_OR_RETURN(Ref<Texture2D>());
	if (!s->glyph_provider_loaded) {
		s->glyph_provider_loaded = true;
		const String file = GLOBAL_GET("input_devices/router/glyph_provider");
		if (!file.is_empty()) {
			s->glyph_provider = ResourceLoader::load(file);
		}
	}
	return s->glyph_provider.is_valid() ? s->glyph_provider->get_glyph(p_family, p_input) : Ref<Texture2D>();
}

// --- Captured pad state ---

bool InputRouter::is_pad_button_down(JoyButton p_button, int p_pad) const {
	const uint8_t b = button_slot(p_button);
	ERR_FAIL_COND_V(b == NONE, false);
	const PadState *pad = pad_of(s->read_frame(), p_pad);
	return pad && pad->is_down(b);
}

bool InputRouter::was_pad_button_pressed(JoyButton p_button, int p_pad) const {
	const uint8_t b = button_slot(p_button);
	ERR_FAIL_COND_V(b == NONE, false);
	const PadState *pad = pad_of(s->read_frame(), p_pad);
	return pad && (pad->pressed_edges & (1u << b));
}

float InputRouter::get_pad_axis(JoyAxis p_axis, int p_pad) const {
	ERR_FAIL_COND_V((uint32_t)p_axis >= AXIS_COUNT, 0.0f);
	const PadState *pad = pad_of(s->read_frame(), p_pad);
	return pad ? pad->axes[(int)p_axis] : 0.0f;
}

// Radial deadzone, rescaled so the output starts at 0 past it.
Vector2 InputRouter::get_pad_stick(RouteBindingResource::Stick p_stick, float p_deadzone, int p_pad) const {
	const PadState *pad = pad_of(s->read_frame(), p_pad);
	if (!pad) {
		return Vector2();
	}
	RouteBinding row;
	row.deadzone = CLAMP(p_deadzone, 0.0f, 0.95f);
	row.saturation = 1.0f;
	return shape_stick(row, nullptr, pad->stick(p_stick));
}

uint64_t InputRouter::get_frame() const {
	return s->read_frame()->frame;
}

// --- Gamepad context ---

// Adds or updates by id. A fallback or modal change on a context in the path rebuilds routes.
void InputRouter::register_gamepad_context(const Ref<GamepadContext> &p_context) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::register_gamepad_context).bind(p_context));
	ERR_FAIL_COND(p_context.is_null());
	const int index = s->ensure_context(p_context->get_id());
	if (index < 0) {
		return;
	}
	const ContextInfo &current = s->names().contexts[index];
	const uint8_t fallback = (uint8_t)p_context->get_fallback();
	if (current.fallback == fallback && current.modal == p_context->is_modal()) {
		return;
	}
	ContextInfo &ci = s->edit_names().contexts[index];
	ci.fallback = fallback;
	ci.modal = p_context->is_modal();
	const Frame &f = *s->write_state;
	for (uint32_t i = 0; i < f.path_size; i++) {
		if (f.path[i].context == index) {
			s->path_changed();
			return;
		}
	}
}

// Base contexts mirror the game's own stack, outer to inner. Pushed contexts stay above them.
void InputRouter::set_gamepad_base_contexts(const PackedStringArray &p_contexts) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_gamepad_base_contexts).bind(p_contexts));
	PathEntry next[MAX_PATH_DEPTH];
	uint32_t count = 0;
	for (int i = 0; i < p_contexts.size(); i++) {
		const int ctx = s->ensure_context(p_contexts[i]);
		ERR_CONTINUE(ctx < 0);
		ERR_FAIL_COND_MSG(count == MAX_PATH_DEPTH, vformat("InputRouter: gamepad context path longer than %d.", MAX_PATH_DEPTH));
		next[count].context = (uint8_t)ctx;
		next[count].handle = 0;
		count++;
	}
	const uint32_t base = count;
	Frame &f = *s->write_state;
	for (uint32_t i = 0; i < f.path_size; i++) {
		if (f.path[i].handle != 0) {
			ERR_FAIL_COND_MSG(count == MAX_PATH_DEPTH, vformat("InputRouter: gamepad context path longer than %d.", MAX_PATH_DEPTH));
			next[count++] = f.path[i];
		}
	}
	bool same = count == f.path_size;
	for (uint32_t i = 0; same && i < count; i++) {
		same = next[i].context == f.path[i].context && next[i].handle == f.path[i].handle;
	}
	if (same) {
		return;
	}
	memcpy(f.path, next, count * sizeof(PathEntry));
	f.path_size = (uint8_t)count;
	f.base_count = (uint8_t)base;
	s->path_changed();
}

// Off the main thread the handle is reserved now and the push runs deferred.
int InputRouter::push_gamepad_context(const StringName &p_context) {
	const uint32_t handle = s->next_handle.increment();
	if (!Thread::is_main_thread()) {
		_defer(callable_mp(this, &InputRouter::_push_handle).bind(p_context, handle));
		return handle;
	}
	_push_handle(p_context, handle);
	return handle;
}

void InputRouter::_push_handle(const StringName &p_context, uint32_t p_handle) {
	const int ctx = s->ensure_context(p_context);
	ERR_FAIL_COND(ctx < 0);
	Frame &f = *s->write_state;
	ERR_FAIL_COND_MSG(f.path_size == MAX_PATH_DEPTH, vformat("InputRouter: gamepad context path longer than %d.", MAX_PATH_DEPTH));
	f.path[f.path_size].context = (uint8_t)ctx;
	f.path[f.path_size].handle = p_handle;
	f.path_size++;
	s->path_changed();
}

// Removes that entry wherever it is; order of the rest is kept. Unknown handles are ignored.
void InputRouter::pop_gamepad_context(int p_handle) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::pop_gamepad_context).bind(p_handle));
	Frame &f = *s->write_state;
	for (uint32_t i = 0; i < f.path_size; i++) {
		if (f.path[i].handle == (uint32_t)p_handle && p_handle != 0) {
			for (uint32_t j = i; j + 1 < f.path_size; j++) {
				f.path[j] = f.path[j + 1];
			}
			f.path_size--;
			s->path_changed();
			return;
		}
	}
}

// The read frame's path holds indices into the read frame's own NameTable copy, so they resolve
// whatever the write frame has done since.
static StringName context_id(const NameTable &p_names, uint8_t p_context) {
	return p_context < p_names.contexts.size() ? p_names.contexts[p_context].id : StringName();
}

StringName InputRouter::get_gamepad_context() const {
	MAIN_OR_RETURN(StringName());
	const Frame &f = *s->read_state;
	return f.path_size ? context_id(s->names_of(f), f.path[f.path_size - 1].context) : StringName();
}

PackedStringArray InputRouter::get_gamepad_context_path() const {
	MAIN_OR_RETURN(PackedStringArray());
	const Frame &f = *s->read_state;
	const NameTable &nt = s->names_of(f);
	PackedStringArray result;
	for (uint32_t i = 0; i < f.path_size; i++) {
		result.push_back(context_id(nt, f.path[i].context));
	}
	return result;
}

bool InputRouter::is_gamepad_context_active(const StringName &p_context) const {
	MAIN_OR_RETURN(false);
	const Frame &f = *s->read_state;
	const NameTable &nt = s->names_of(f);
	for (uint32_t i = f.routable_from; i < f.path_size; i++) {
		if (context_id(nt, f.path[i].context) == p_context) {
			return true;
		}
	}
	return false;
}

// --- Bindings ---

// One row in p_context, else the binding's own. Off the main thread the handle is reserved now
// and the attach runs deferred.
int InputRouter::attach(const Ref<RouteBindingResource> &p_binding, const StringName &p_context, const Callable &p_target) {
	const uint32_t handle = s->next_handle.increment();
	if (!Thread::is_main_thread()) {
		_defer(callable_mp(this, &InputRouter::_attach_handle).bind(p_binding, p_context, p_target, handle));
		return handle;
	}
	return _attach_handle(p_binding, p_context, p_target, handle) ? (int)handle : 0;
}

// A row from the route map's template now. One the map lacks is compiled into the next frame's
// map and placed then. The editor build also replaces a template that differs from the resource.
bool InputRouter::_attach_handle(const Ref<RouteBindingResource> &p_binding, const StringName &p_context, const Callable &p_target, uint32_t p_handle) {
	ERR_FAIL_COND_V(p_binding.is_null(), false);
	ERR_FAIL_COND_V_MSG(p_binding->get_id() == StringName(), false, "InputRouter: RouteBindingResource has no id.");
	s->ensure_map();
	StringName ctx_id = p_context;
	const Ref<GamepadContext> own = p_binding->get_context();
	if (ctx_id == StringName() && own.is_valid()) {
		ctx_id = own->get_id();
	}
	ERR_FAIL_COND_V_MSG(ctx_id == StringName(), false, vformat("InputRouter: binding '%s' has no context.", p_binding->get_id()));
	if (own.is_valid() && own->get_id() == ctx_id) {
		register_gamepad_context(own);
	}
	Placement p;
	p.id = p_binding->get_id();
	p.scheme = p_binding->get_scheme();
	p.context = ctx_id;
	p.target = p_target;
	p.handle = p_handle;
#ifdef TOOLS_ENABLED
	p.source = p_binding->get_instance_id();
	s->watch_binding(p.source, true);
#endif
	if (!s->place(p)) {
		s->pending_placements.push_back(p);
		s->queue_binding(p_binding, "is missing from");
		return true;
	}
#ifdef TOOLS_ENABLED
	_binding_changed(p.source);
#endif
	return true;
}

void InputRouter::detach(int p_handle) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::detach).bind(p_handle));
	s->release_handle(p_handle);
}

// Each binding takes its own context, else the map's; all share one handle.
int InputRouter::load_map(const Ref<GamepadMap> &p_map) {
	const uint32_t handle = s->next_handle.increment();
	if (!Thread::is_main_thread()) {
		_defer(callable_mp(this, &InputRouter::_load_map_handle).bind(p_map, handle));
		return handle;
	}
	_load_map_handle(p_map, handle);
	return handle;
}

void InputRouter::_load_map_handle(const Ref<GamepadMap> &p_map, uint32_t p_handle) {
	ERR_FAIL_COND(p_map.is_null());
	const Ref<GamepadContext> ctx = p_map->get_context();
	if (ctx.is_valid()) {
		register_gamepad_context(ctx);
	}
	const TypedArray<RouteBindingResource> bindings = p_map->get_bindings();
	for (int i = 0; i < bindings.size(); i++) {
		const Ref<RouteBindingResource> b = bindings[i];
		if (b.is_null()) {
			continue;
		}
		const StringName b_ctx = b->get_context().is_valid() ? b->get_context()->get_id() : (ctx.is_valid() ? ctx->get_id() : StringName());
		_attach_handle(b, b_ctx, Callable(), p_handle);
	}
}

void InputRouter::unload_map(int p_handle) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::unload_map).bind(p_handle));
	s->release_handle(p_handle);
}

// For rows attached without a target, by (context, binding id). Resolved at the next rebuild.
int InputRouter::set_callback(const StringName &p_context, const StringName &p_binding_id, const Callable &p_target) {
	const uint32_t handle = s->next_handle.increment();
	if (!Thread::is_main_thread()) {
		_defer(callable_mp(this, &InputRouter::_set_callback_handle).bind(p_context, p_binding_id, p_target, handle));
		return handle;
	}
	_set_callback_handle(p_context, p_binding_id, p_target, handle);
	return handle;
}

void InputRouter::_set_callback_handle(const StringName &p_context, const StringName &p_binding_id, const Callable &p_target, uint32_t p_handle) {
	ERR_FAIL_COND_MSG(s->named_callbacks.size() >= MAX_NAMED_CALLBACKS, vformat("InputRouter: more than %d named callbacks.", MAX_NAMED_CALLBACKS));
	NamedCallback nc;
	nc.handle = p_handle;
	nc.context = p_context;
	nc.binding_id = p_binding_id;
	nc.target = p_target;
	s->named_callbacks.push_back(nc);
	s->callbacks_dirty = true;
	s->routes_dirty = true;
}

void InputRouter::clear_callback(int p_handle) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::clear_callback).bind(p_handle));
	for (uint32_t i = 0; i < s->named_callbacks.size(); i++) {
		if (s->named_callbacks[i].handle == (uint32_t)p_handle) {
			s->named_callbacks.remove_at_unordered(i);
			s->callbacks_dirty = true;
			s->routes_dirty = true;
			return;
		}
	}
}

// Every row, named callback and pushed context. Base contexts stay.
void InputRouter::clear() {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::clear));
	for (uint32_t r = 0; r < s->rows().rows.size(); r++) {
		if (s->rows().rows[r].input != NONE) {
			s->kill_row((uint16_t)r);
		}
	}
#ifdef TOOLS_ENABLED
	for (const Placement &p : s->pending_placements) {
		s->watch_binding(p.source, false);
	}
#endif
	s->pending_placements.clear();
	s->named_callbacks.clear();
	s->callbacks_dirty = true;
	s->write_state->path_size = s->write_state->base_count;
	s->path_changed();
}

static Dictionary route_dict(const RouteBinding &p_row, const RouteBindingInfo &p_info, const StringName &p_context, int p_family) {
	Dictionary d;
	d["input"] = p_row.input;
	d["label"] = input_label(p_row.input, p_family);
	d["gate"] = p_row.gate == NONE ? String() : input_label(p_row.gate, p_family);
	d["context"] = p_context;
	d["id"] = p_info.id;
	d["action"] = p_info.action;
	d["trigger"] = trigger_name(p_row.trigger);
	d["handle"] = p_info.handle;
	return d;
}

// Physical to virtual on the read frame's path: per input, inner context first, then sequences.
// {input, label, gate, context, id, action, trigger, handle}, labels in the active pad's family.
// flip() rebuilds dirty routes before it seals, so the read frame's are current for its rows.
TypedArray<Dictionary> InputRouter::_get_routes() const {
	MAIN_OR_RETURN(TypedArray<Dictionary>());
	const Frame &f = *s->read_state;
	const RouteTable &table = s->routes_of(f);
	const RowTable &rt = s->rows_of(f);
	const NameTable &nt = s->names_of(f);
	const int family = f.active_pad >= 0 && (f.live_pads & (1u << f.active_pad)) ? f.pads[f.active_pad].family : PAD_FAMILY_GENERIC;
	TypedArray<Dictionary> result;
	for (uint8_t slot = 0; slot < INPUT_COUNT; slot++) {
		for (uint32_t i = table.slot_first[slot]; i < table.slot_first[slot + 1]; i++) {
			const uint16_t r = table.active[i];
			result.push_back(route_dict(rt.rows[r], rt.info[r], nt.contexts[rt.rows[r].context].id, family));
		}
	}
	for (uint32_t i = 0; i < f.sequence_count; i++) {
		const uint16_t r = f.sequences[i].row;
		result.push_back(route_dict(rt.rows[r], rt.info[r], nt.contexts[rt.rows[r].context].id, family));
	}
	return result;
}

// --- Schemes, rebinds, timing ---

// Cancels every claim, like a path change.
void InputRouter::set_scheme(const StringName &p_scheme) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_scheme).bind(p_scheme));
	int index = -1;
	if (p_scheme != StringName()) {
		const int mask = s->scheme_mask_of(p_scheme);
		ERR_FAIL_COND(mask < 0);
		index = lowest_bit((uint64_t)mask);
	}
	if (index == s->write_state->scheme) {
		return;
	}
	s->cancel_claims();
	s->write_state->scheme = (int8_t)index;
	s->routes_dirty = true;
	s->queue_signal(SIGNAL_SCHEME_CHANGED);
	s->queue_signal(SIGNAL_ROUTES_CHANGED);
}

StringName InputRouter::get_scheme() const {
	MAIN_OR_RETURN(StringName());
	const Frame &f = *s->read_state;
	const NameTable &nt = s->names_of(f);
	return f.scheme >= 0 && f.scheme < (int)nt.schemes.size() ? nt.schemes[f.scheme] : StringName();
}

PackedStringArray InputRouter::_get_schemes() const {
	MAIN_OR_RETURN(PackedStringArray());
	PackedStringArray result;
	for (const StringName &scheme : s->names_of(*s->read_state).schemes) {
		result.push_back(scheme);
	}
	return result;
}

// Player rebind by binding id, for rows now and later. Stick for stick; p_gate a button or -1.
bool InputRouter::set_override(const StringName &p_binding_id, int p_input, int p_gate) {
	if (!Thread::is_main_thread()) {
		_defer(callable_mp(this, &InputRouter::set_override).bind(p_binding_id, p_input, p_gate));
		return true;
	}
	ERR_FAIL_INDEX_V(p_input, INPUT_COUNT, false);
	ERR_FAIL_COND_V(p_gate != -1 && (p_gate < 0 || p_gate >= BUTTON_COUNT || p_gate == p_input), false);
	const RowTable &rt = s->rows();
	for (uint32_t r = 0; r < rt.rows.size(); r++) {
		if (rt.rows[r].input != NONE && rt.info[r].id == p_binding_id && is_stick_slot(rt.rows[r].authored_input) != is_stick_slot((uint8_t)p_input)) {
			return false;
		}
	}
	Override *o = nullptr;
	for (Override &existing : s->overrides) {
		if (existing.binding_id == p_binding_id) {
			o = &existing;
		}
	}
	if (!o) {
		ERR_FAIL_COND_V_MSG(s->overrides.size() >= MAX_OVERRIDES, false, vformat("InputRouter: more than %d overrides.", MAX_OVERRIDES));
		s->overrides.push_back(Override());
		o = &s->overrides[s->overrides.size() - 1];
		o->binding_id = p_binding_id;
	}
	o->input = (uint8_t)p_input;
	o->gate = p_gate < 0 ? NONE : (uint8_t)p_gate;
	s->reapply_override(p_binding_id);
	return true;
}

void InputRouter::clear_override(const StringName &p_binding_id) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::clear_override).bind(p_binding_id));
	for (uint32_t i = 0; i < s->overrides.size(); i++) {
		if (s->overrides[i].binding_id != p_binding_id) {
			continue;
		}
		s->overrides.remove_at_unordered(i);
		s->reapply_override(p_binding_id);
		return;
	}
}

void InputRouter::clear_overrides() {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::clear_overrides));
	while (!s->overrides.is_empty()) {
		const StringName id = s->overrides[s->overrides.size() - 1].binding_id;
		s->overrides.remove_at(s->overrides.size() - 1);
		s->reapply_override(id);
	}
}

// {binding_id: [input, gate]}, gate -1 for none. The game persists this and replays set_override().
Dictionary InputRouter::_get_overrides() const {
	MAIN_OR_RETURN(Dictionary());
	Dictionary result;
	for (const Override &o : s->overrides) {
		Array pair;
		pair.push_back(o.input);
		pair.push_back(o.gate == NONE ? -1 : (int)o.gate);
		result[o.binding_id] = pair;
	}
	return result;
}

void InputRouter::set_sequence_timeout(float p_seconds) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_sequence_timeout).bind(p_seconds));
	s->sequence_timeout_usec = (uint64_t)(CLAMP(p_seconds, SEQUENCE_TIMEOUT_MIN, SEQUENCE_TIMEOUT_MAX) * 1000000.0f);
}

float InputRouter::get_sequence_timeout() const {
	MAIN_OR_RETURN(SEQUENCE_TIMEOUT_DEFAULT);
	return s->sequence_timeout_usec / 1000000.0f;
}

void InputRouter::set_chord_window(float p_seconds) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_chord_window).bind(p_seconds));
	s->chord_window_usec = (uint64_t)(CLAMP(p_seconds, 0.0f, CHORD_WINDOW_MAX) * 1000000.0f);
}

float InputRouter::get_chord_window() const {
	MAIN_OR_RETURN(CHORD_WINDOW_DEFAULT);
	return s->chord_window_usec / 1000000.0f;
}

// --- Route map ---

// Validated now; the next frame destroys and recreates the routing state on it. With nothing
// placed yet (boot) it applies at once, so the attaches that follow resolve against it. Getters
// see it after the next flip.
void InputRouter::set_route_map(const Ref<GamepadBakedMaps> &p_map) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_route_map).bind(p_map));
	ERR_FAIL_COND(p_map.is_null());
	const String invalid = Baked::validate(p_map->get_data());
	ERR_FAIL_COND_MSG(!invalid.is_empty(), vformat("InputRouter: route map refused: %s.", invalid));
	s->map_tried = true;
	s->pending_map = p_map->get_data();
	const RowTable &rt = s->rows();
	if (rt.rows.size() == rt.free_rows.size() && s->pending_placements.is_empty() && !s->in_frame) {
		s->rebuild_from_map();
	}
}

// --- Touch, pointer, trace ---

// Live touches of the old mode end first: passed ones release, routed ones cancel.
void InputRouter::set_touch_mode(TouchMode p_mode) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_touch_mode).bind(p_mode));
	ERR_FAIL_INDEX(p_mode, 3);
	if (p_mode == s->touch_mode) {
		return;
	}
	Frame &f = *s->write_state;
	for (uint32_t i = 0; i < f.touch_count; i++) {
		const TouchState &t = f.touches[i];
		if (t.passed) {
			s->out.push_back(make_touch_cancel(t));
		} else if (!t.suppressed) {
			s->queue_touch(t.index, PHASE_CANCELED, t.position, 0.0f);
		}
	}
	f.touch_count = 0;
	s->touch_mode = p_mode;
}

InputRouter::TouchMode InputRouter::get_touch_mode() const {
	MAIN_OR_RETURN(TOUCH_AS_POINTER);
	return s->touch_mode;
}

// Pixels since the last call, from pointer bindings and (MOUSE_IN_PAD_POINTER) mouse motion. The
// write frame carries the sum forward, so taking it there takes all of it.
Vector2 InputRouter::take_pointer_delta() {
	MAIN_OR_RETURN(Vector2());
	Frame &f = *s->write_state;
	const Vector2 delta = f.pointer_delta;
	f.pointer_delta = Vector2();
	return delta;
}

Vector2 InputRouter::get_pointer_velocity() const {
	return s->read_frame()->pointer_velocity;
}

// The ring is allocated here, once; records become Dictionaries only in get_trace().
void InputRouter::set_trace_enabled(bool p_enabled, int p_capacity) {
	MAIN_OR_DEFER(callable_mp(this, &InputRouter::set_trace_enabled).bind(p_enabled, p_capacity));
	s->trace_enabled = p_enabled;
	s->trace_head = 0;
	s->trace_count = 0;
	s->path_history_head = 0;
	if (p_enabled) {
		s->trace.resize(CLAMP(p_capacity, 64, 65536));
		s->path_history.resize(PATH_HISTORY);
		s->record_path();
	} else {
		s->trace.reset();
		s->path_history.reset();
	}
}

bool InputRouter::is_trace_enabled() const {
	MAIN_OR_RETURN(false);
	return s->trace_enabled;
}

// Records since the last call, oldest first, as {usec, frame, path, context, id, pad, input,
// label, phase, outcome, value}. path is the context path at the time, when still in history.
TypedArray<Dictionary> InputRouter::_get_trace() {
	MAIN_OR_RETURN(TypedArray<Dictionary>());
	TypedArray<Dictionary> result;
	const uint32_t size = s->trace.size();
	if (size == 0) {
		return result;
	}
	const uint32_t start = (s->trace_head + size - s->trace_count) % size;
	const Frame &f = *s->write_state; // Records span frames: names resolve against the newest.
	const NameTable &nt = s->names();
	const RowTable &rt = s->rows();
	for (uint32_t n = 0; n < s->trace_count; n++) {
		const TraceRecord &r = s->trace[(start + n) % size];
		Dictionary d;
		d["usec"] = r.usec;
		d["frame"] = r.frame;
		PackedStringArray path; // Newest first: unused slots, version 0, sit behind every recorded one.
		const uint32_t history = s->path_history.size();
		for (uint32_t k = 1; k <= history; k++) {
			const Pair<uint32_t, PackedStringArray> &h = s->path_history[(s->path_history_head + history - k) % history];
			if (h.first == r.path_version) {
				path = h.second;
				break;
			}
		}
		d["path"] = path;
		d["context"] = r.context < nt.contexts.size() ? nt.contexts[r.context].id : StringName();
		d["id"] = r.row >= 0 && r.row < (int)rt.info.size() ? rt.info[r.row].id : StringName();
		d["pad"] = r.pad;
		d["input"] = r.input == NONE ? -1 : (int)r.input;
		d["label"] = r.input == NONE ? String() : input_label(r.input, r.pad >= 0 && (f.live_pads & (1u << r.pad)) ? f.pads[r.pad].family : PAD_FAMILY_GENERIC);
		d["phase"] = r.phase;
		d["outcome"] = OUTCOME_NAMES[MIN(r.outcome, (uint8_t)OUTCOME_CANCELED)];
		d["value"] = r.value;
		result.push_back(d);
	}
	s->trace_count = 0;
	return result;
}

// --- Input::FrameFilter ---

// Once per frame, before physics, on the main thread, straight through: the frame update
// S(t + 1) = F(S(t), input) into the write frame, then the flip that seals it, before any script
// code sees the frame: Input's dispatch, then callbacks, then signals. What script changes from
// then on lands in the next write frame and goes out with the next flip. A requested disable runs
// instead of the frame. Every flip happens here, one per frame, which is what the ring's grace
// counts on.
void InputRouter::input_poll_process_flip() {
	if (!s->write_state->enabled) {
		return;
	}
	if (s->disable_requested) {
		s->disable();
		return;
	}
	s->in_frame = true;
	_poll();
	_process();
	_flip();
	_callback();
	_emit_signals();
	s->in_frame = false;
}

// Clock, a pending route map, the frame's events from Input, connection changes from any thread,
// then the action releases owed since the last frame. begin_frame() already cleared the edges.
void InputRouter::_poll() {
	Frame &f = *s->write_state;
	f.now = OS::get_singleton()->get_ticks_usec();
	f.frame++;
	if (!s->pending_map.is_empty() || !s->pending_bindings.is_empty()) {
		s->rebuild_from_map();
	}
	Input::get_singleton()->take_buffered_events(s->batch);
	s->poll_connections();
	for (const StringName &action : s->pulses) {
		s->emit_action(action, false, 0.0f, f.active_pad);
	}
	s->pulses.clear();
}

void InputRouter::_process() {
	s->route_frame();
}

// The write frame becomes the read frame, its queued work with it, and the next write frame forks
// from it. Then Input parses the routed batch with no router lock to take.
void InputRouter::_flip() {
	s->flip();
	SWAP(s->batch, s->out);
	s->out.clear();
	Input::get_singleton()->parse_filtered_events(s->batch);
	s->batch.clear();
}

// The read frame's callbacks, FIFO. Nothing flips inside a frame, so it holds throughout. A row
// that died or was reused since it fired is skipped.
void InputRouter::_callback() {
	const Frame &f = *s->read_state;
	for (uint32_t i = 0; i < f.queued_count; i++) {
		if (!f.queued[i].signal) {
			const Queued q = f.queued[i];
			s->run_callback(q);
		}
	}
}

// The read frame's signals, FIFO, after every callback. No signal fires every frame, and the
// router has no signal mutex.
void InputRouter::_emit_signals() {
	const Frame &f = *s->read_state;
	for (uint32_t i = 0; i < f.queued_count; i++) {
		if (f.queued[i].signal) {
			const Queued q = f.queued[i];
			s->run_signal(q);
		}
	}
}

// Focus loss: Input drops its pressed state next, so only router state clears. Events still
// buffered route next frame.
void InputRouter::pressed_events_released() {
	if (s->write_state->enabled) {
		s->release_all(false);
	}
}

// Any thread. Input holds its lock around this call, which makes it the ring's only producer.
void InputRouter::joy_connection_changed(int p_pad, bool p_connected) {
	const uint32_t tail = s->connection_tail.load(std::memory_order_relaxed);
	if (tail - s->connection_head.load(std::memory_order_acquire) >= MAX_CONNECTION_CHANGES) {
		s->connection_overflow.store(true, std::memory_order_release);
		return;
	}
	s->connection_ring[tail % MAX_CONNECTION_CHANGES] = Pair<int8_t, bool>((int8_t)p_pad, p_connected);
	s->connection_tail.store(tail + 1, std::memory_order_release);
}

#ifdef TOOLS_ENABLED
// Live tuning, and the attach-time check: a resource that no longer matches its template merges
// into the next frame's map, which rebuilds every row. A compile error waits for a fix.
void InputRouter::_binding_changed(ObjectID p_binding) {
	Ref<RouteBindingResource> binding = Object::cast_to<RouteBindingResource>(ObjectDB::get_instance(p_binding));
	if (binding.is_null()) {
		return;
	}
	Dictionary probe = s->map_data.duplicate();
	String error;
	switch (Baked::add_binding(probe, **binding, error)) {
		case Baked::ADD_FAILED:
			WARN_PRINT(vformat("InputRouter: binding '%s': %s; keeping the last good tuning.", binding->get_id(), error));
			break;
		case Baked::ADD_SAME:
			break;
		default:
			s->queue_binding(binding, "differs from");
			break;
	}
}
#endif
