#pragma once

// Inspector panels for RouteBindingResource and GamepadMap. The live preview reads stock Input
// and shapes with the router's own functions.

#ifdef TOOLS_ENABLED

#include "../input_router_internal.hpp"

#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"

class Button;
class GridContainer;
class Label;
class OptionButton;
class Tree;

class InputRouterPadPreview : public Control {
	GDCLASS(InputRouterPadPreview, Control);

	Ref<RouteBindingResource> binding;
	InputRouterInternal::RouteBinding row;
	float lut[InputRouterInternal::LUT_STRIDE] = {};
	bool has_lut = false;
	bool valid = false;
	String error;
	InputRouterInternal::Claim claim;
	int pad = -1;
	float raw = 0.0f;
	float shaped = 0.0f;
	Vector2 raw_stick;
	Vector2 shaped_stick;
	String fired;
	uint64_t fired_usec = 0;

	void _compile();
	void _poll();
	void _draw_response(const Rect2 &p_rect);
	void _draw_stick(const Rect2 &p_rect);

protected:
	void _notification(int p_what);

public:
	void set_binding(const Ref<RouteBindingResource> &p_binding);
	void set_pad(int p_pad);
	int get_live_pad() const;
	uint8_t get_input() const { return valid ? row.input : InputRouterInternal::NONE; }

	InputRouterPadPreview();
};

class InputRouterBindingPanel : public VBoxContainer {
	GDCLASS(InputRouterBindingPanel, VBoxContainer);

	Ref<RouteBindingResource> binding;
	Label *summary = nullptr;
	GridContainer *labels = nullptr;
	Button *listen = nullptr;
	OptionButton *pads = nullptr;
	InputRouterPadPreview *preview = nullptr;
	uint32_t listen_start = 0;
	uint32_t listen_prev = 0;
	float listen_axes[InputRouterInternal::AXIS_COUNT] = {};
	int listen_first = -1;

	void _update();
	void _listen_toggled(bool p_on);
	void _poll_listen();
	void _commit_input(RouteBindingResource::Source p_source, int p_index, int p_direction, int p_gate);
	void _preset(int p_preset);
	void _pad_selected(int p_index);
	void _refresh_pads();

protected:
	void _notification(int p_what);

public:
	void set_binding(const Ref<RouteBindingResource> &p_binding);

	InputRouterBindingPanel();
};

class InputRouterMapPanel : public VBoxContainer {
	GDCLASS(InputRouterMapPanel, VBoxContainer);

	Ref<GamepadMap> map;
	Tree *tree = nullptr;

	void _update();

protected:
	void _notification(int p_what);

public:
	void set_map(const Ref<GamepadMap> &p_map);

	InputRouterMapPanel();
};

class InputRouterInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(InputRouterInspectorPlugin, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
};

class InputRouterEditorPlugin : public EditorPlugin {
	GDCLASS(InputRouterEditorPlugin, EditorPlugin);

	static inline InputRouterEditorPlugin *singleton = nullptr;
	Ref<FontVariation> label_font;
	Ref<Font> label_glyphs;

public:
	static bool resource_label(const Ref<Resource> &p_resource, String &r_text, Ref<Font> &r_font); // EditorResourcePicker::LabelFunc
	Ref<Font> get_label_font(const Ref<Font> &p_glyphs);

	virtual String get_plugin_name() const override { return "InputRouter"; }

	InputRouterEditorPlugin();
	~InputRouterEditorPlugin();
};

#endif // TOOLS_ENABLED
