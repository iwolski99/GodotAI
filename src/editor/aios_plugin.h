/**************************************************************************/
/*  aios_plugin.h                                                         */
/*  EditorPlugin entry point: owns the transport, tools and the dock.     */
/**************************************************************************/

#pragma once

#include "../ipc/aios_ipc_server.h"
#include "../tools/aios_tool_registry.h"
#include "../vcs/aios_git_checkpoint.h"
#include "../world/aios_world_model.h"
#include "aios_chat_dock.h"

#include <godot_cpp/classes/editor_plugin.hpp>

using namespace godot;

class AIOSPlugin : public EditorPlugin {
	GDCLASS(AIOSPlugin, EditorPlugin)

private:
	Ref<AIOSIpcServer> ipc;
	Ref<AIOSWorldModel> world_model;
	Ref<AIOSToolRegistry> registry;
	Ref<AIOSGitCheckpoint> git;
	AIOSChatDock *dock = nullptr;

	String session_token;
	double status_accumulator = 0.0;
	int last_reported_clients = -1;

	void _register_project_settings();
	Variant _setting(const String &p_name, const Variant &p_default, int p_type, const String &p_hint_string = String());
	String _generate_token() const;
	void _write_session_file();
	void _remove_session_file();
	void _start_transport();

	// IPC -> here
	void _on_client_connected(int p_client_id, const String &p_remote);
	void _on_client_disconnected(int p_client_id, const String &p_reason);
	void _on_message_received(int p_client_id, const Dictionary &p_message);
	void _on_transport_log(const String &p_level, const String &p_message);

	// Dock -> here
	void _on_prompt_submitted(const String &p_text, const String &p_mode);
	void _on_execute_plan_requested(const String &p_mode);
	void _on_stop_requested();
	void _on_rollback_requested();

	// Editor -> here
	void _on_scene_changed(Node *p_root);
	void _on_scene_saved(const String &p_path);

	void _handle_request(int p_client_id, const Dictionary &p_message);
	void _handle_event(int p_client_id, const Dictionary &p_message);

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;
	String _get_plugin_name() const override;
	bool _has_main_screen() const override;
};
