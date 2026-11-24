#pragma once

// Bakes the canonical gamepad map into the export. Editor builds only.

#ifdef TOOLS_ENABLED

#include "../gamepad_resources.h"

#include "editor/export/editor_export_plugin.h"

class EditorFileSystemDirectory;

class InputRouterExportPlugin : public EditorExportPlugin {
	GDCLASS(InputRouterExportPlugin, EditorExportPlugin);

	String target; // The export's baked map path; empty when the router is off or has none.

	void _scan(EditorFileSystemDirectory *p_dir, TypedArray<GamepadContext> &r_contexts, TypedArray<GamepadMap> &r_maps, TypedArray<RouteBindingResource> &r_bindings);
	static void _collect(const Variant &p_value, TypedArray<GamepadMap> &r_maps, TypedArray<RouteBindingResource> &r_bindings);

protected:
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	virtual void _export_end() override;

public:
	virtual String get_name() const override { return "InputRouter"; }
};

#endif // TOOLS_ENABLED
