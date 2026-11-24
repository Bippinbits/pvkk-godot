#include "register_types.h"

#include "gamepad_resources.h"
#include "input_router.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/input_router_editor_plugin.h"
#endif

static InputRouter *input_router = nullptr;

// The router exists in every build but filters input only once enabled, never in the editor.
void initialize_input_router_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(GamepadContext);
		GDREGISTER_CLASS(RouteBindingResource);
		GDREGISTER_CLASS(GamepadMap);
		GDREGISTER_CLASS(GamepadBakedMaps);
		GDREGISTER_CLASS(GamepadGlyphProvider);
		GDREGISTER_ABSTRACT_CLASS(InputRouter);

		input_router = memnew(InputRouter);
		Engine::get_singleton()->add_singleton(Engine::Singleton("InputRouter", InputRouter::get_singleton()));
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<InputRouterEditorPlugin>();
	}
#endif
}

// Input may still flush during shutdown; the destructor clears the frame filter first.
void uninitialize_input_router_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE || !input_router) {
		return;
	}
	Engine::get_singleton()->remove_singleton("InputRouter");
	memdelete(input_router);
	input_router = nullptr;
}
