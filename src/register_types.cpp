/**************************************************************************/
/*  register_types.cpp                                                    */
/*  GDExtension entry point.                                              */
/**************************************************************************/

#include "register_types.h"

#include "agent/aios_agent_memory.h"
#include "agent/aios_credentials.h"
#include "assets/aios_asset_pipeline.h"
#include "assets/aios_blender_bridge.h"
#include "agent/aios_llm_client.h"
#include "editor/aios_chat_dock.h"
#include "editor/aios_plugin.h"
#include "ipc/aios_ipc_server.h"
#include "pipeline/aios_pipeline.h"
#include "playtest/aios_playtest.h"
#include "tools/aios_tool_registry.h"
#include "validate/aios_validator.h"
#include "vision/aios_vision.h"
#include "vcs/aios_git_checkpoint.h"
#include "world/aios_world_model.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_ai_agent_os_module(ModuleInitializationLevel p_level) {
	// Everything here touches EditorInterface, so there is nothing to register
	// at the scene level — an exported game must not carry this code path.
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}

	GDREGISTER_CLASS(AIOSIpcServer);
	GDREGISTER_CLASS(AIOSWorldModel);
	GDREGISTER_CLASS(AIOSGitCheckpoint);
	GDREGISTER_CLASS(AIOSValidator);
	GDREGISTER_CLASS(AIOSPlaytest);
	GDREGISTER_CLASS(AIOSVision);
	GDREGISTER_CLASS(AIOSAssetPipeline);
	GDREGISTER_CLASS(AIOSBlenderBridge);
	GDREGISTER_CLASS(AIOSToolRegistry);
	GDREGISTER_CLASS(AIOSCredentials);
	GDREGISTER_CLASS(AIOSAgentMemory);
	GDREGISTER_CLASS(AIOSLlmClient);
	GDREGISTER_CLASS(AIOSPipeline);
	GDREGISTER_CLASS(AIOSChatDock);
	GDREGISTER_CLASS(AIOSPlugin);

	EditorPlugins::add_by_type<AIOSPlugin>();
}

void uninitialize_ai_agent_os_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
	// godot-cpp tears the registered EditorPlugin down for us; see
	// EditorPlugins::deinitialize() in src/godot.cpp.
}

extern "C" {

GDExtensionBool GDE_EXPORT godot_ai_os_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_ai_agent_os_module);
	init_obj.register_terminator(uninitialize_ai_agent_os_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_EDITOR);

	return init_obj.init();
}
}
