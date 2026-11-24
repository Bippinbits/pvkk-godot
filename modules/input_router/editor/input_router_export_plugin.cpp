#ifdef TOOLS_ENABLED

#include "input_router_export_plugin.h"

#include "../input_router_internal.hpp"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "editor/export/editor_export_platform.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "scene/resources/packed_scene.h"

// Contexts, maps and standalone bindings by file type. Scenes only when they embed a binding or
// map: text scenes are searched for the type name before anything is loaded.
void InputRouterExportPlugin::_scan(EditorFileSystemDirectory *p_dir, TypedArray<GamepadContext> &r_contexts, TypedArray<GamepadMap> &r_maps, TypedArray<RouteBindingResource> &r_bindings) {
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_scan(p_dir->get_subdir(i), r_contexts, r_maps, r_bindings);
	}
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		const StringName type = p_dir->get_file_type(i);
		const String path = p_dir->get_file_path(i);
		if (type == SNAME("GamepadContext")) {
			r_contexts.push_back(ResourceLoader::load(path));
		} else if (type == SNAME("GamepadMap")) {
			r_maps.push_back(ResourceLoader::load(path));
		} else if (type == SNAME("RouteBindingResource")) {
			r_bindings.push_back(ResourceLoader::load(path));
		} else if (type == SNAME("PackedScene")) {
			if (path.ends_with(".tscn")) {
				const String text = FileAccess::get_file_as_string(path);
				if (!text.contains("\"RouteBindingResource\"") && !text.contains("\"GamepadMap\"")) {
					continue;
				}
			}
			const Ref<PackedScene> scene = ResourceLoader::load(path);
			if (scene.is_null()) {
				continue;
			}
			const Ref<SceneState> state = scene->get_state();
			for (int n = 0; n < state->get_node_count(); n++) {
				for (int p = 0; p < state->get_node_property_count(n); p++) {
					_collect(state->get_node_property_value(n, p), r_maps, r_bindings);
				}
			}
		}
	}
}

void InputRouterExportPlugin::_collect(const Variant &p_value, TypedArray<GamepadMap> &r_maps, TypedArray<RouteBindingResource> &r_bindings) {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *object = p_value;
		if (RouteBindingResource *binding = Object::cast_to<RouteBindingResource>(object)) {
			r_bindings.push_back(binding);
		} else if (GamepadMap *map = Object::cast_to<GamepadMap>(object)) {
			r_maps.push_back(map);
		}
	} else if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		for (int i = 0; i < array.size(); i++) {
			_collect(array[i], r_maps, r_bindings);
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		const Array values = Dictionary(p_value).values();
		for (int i = 0; i < values.size(); i++) {
			_collect(values[i], r_maps, r_bindings);
		}
	}
}

// Bakes the project's gamepad data into input_devices/router/baked_map. On a bake error the map
// is left out and the game compiles bindings at runtime; _export_file() keeps a copy saved in the
// project out either way.
void InputRouterExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	target = String();
	const String path = GLOBAL_GET("input_devices/router/baked_map");
	if (path.is_empty() || !GLOBAL_GET("input_devices/router/enabled")) {
		return;
	}
	target = path.simplify_path();
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	ERR_FAIL_NULL(efs);
	TypedArray<GamepadContext> contexts;
	TypedArray<GamepadMap> maps;
	TypedArray<RouteBindingResource> bindings;
	_scan(efs->get_filesystem(), contexts, maps, bindings);

	PackedStringArray errors;
	const Ref<GamepadBakedMaps> baked = InputRouterInternal::bake(contexts, maps, bindings, errors);
	if (!errors.is_empty()) {
		const Ref<EditorExportPlatform> platform = get_export_platform();
		for (const String &e : errors) {
			if (platform.is_valid()) {
				platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Gamepad map", e);
			}
			ERR_PRINT("InputRouter bake: " + e);
		}
		return;
	}

	const String temp = EditorPaths::get_singleton()->get_temp_dir().path_join("gamepad_baked_maps.res");
	const Error err = ResourceSaver::save(baked, temp);
	ERR_FAIL_COND_MSG(err != OK, "InputRouter: could not write the baked gamepad map.");
	add_file(target, FileAccess::get_file_as_bytes(temp), false);
}

// A baked map saved in the project (a local rebake, say) predates this export's bake.
void InputRouterExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	if (!target.is_empty() && p_path.simplify_path() == target) {
		skip();
	}
}

void InputRouterExportPlugin::_export_end() {
	target = String();
}

#endif // TOOLS_ENABLED
