/**************************************************************************/
/*  aios_plugin.h                                                         */
/*  EditorPlugin entry point: owns the transport, tools and the dock.     */
/**************************************************************************/

#pragma once

#include "../agent/aios_agent_memory.h"
#include "../agent/aios_credentials.h"
#include "../agent/aios_llm_client.h"
#include "../assets/aios_asset_pipeline.h"
#include "../ipc/aios_ipc_server.h"
#include "../pipeline/aios_pipeline.h"
#include "../playtest/aios_playtest.h"
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
	Ref<AIOSPlaytest> playtest;
	Ref<AIOSCredentials> credentials;
	Ref<AIOSAgentMemory> memory;
	Ref<AIOSPipeline> pipeline;
	AIOSLlmClient *llm = nullptr; // A child Node: HTTPRequest needs a tree.
	AIOSAssetPipeline *assets = nullptr; // Ditto.
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

	// Model configuration lives in ProjectSettings so it survives a restart and
	// can be reviewed in one place; the dock is a view onto it, not a second
	// source of truth.
	Dictionary _read_model_settings() const;
	void _write_model_settings(const Dictionary &p_settings);
	void _apply_model_settings();
	void _refresh_key_status();

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
	void _on_settings_changed(const Dictionary &p_settings);
	void _on_api_key_submitted(const String &p_provider, const String &p_key);
	void _on_api_key_cleared(const String &p_provider);
	void _on_models_refresh_requested(const String &p_provider);

	// Pipeline -> here (and on to the dock and every connected IPC client)
	void _on_pipeline_stage_changed(const String &p_stage, const String &p_detail);
	void _on_pipeline_message(const String &p_text);
	void _on_pipeline_thinking(const String &p_text);
	void _on_pipeline_log(const String &p_level, const String &p_message);
	void _on_pipeline_tool_invoked(const String &p_tool, const Dictionary &p_params);
	void _on_pipeline_tool_completed(const String &p_tool, bool p_ok, const Dictionary &p_envelope);
	void _on_pipeline_plan_proposed(const Dictionary &p_plan, const String &p_diff_preview);
	void _on_pipeline_run_finished(const Dictionary &p_summary);

	// Playtest -> here
	void _on_playtest_output(const String &p_stream, const String &p_line);
	void _on_playtest_finished(const Dictionary &p_report);

	// Asset pipeline -> here
	void _on_asset_stage_changed(const String &p_stage, const String &p_detail);
	void _on_asset_ready(const Dictionary &p_result);
	void _on_asset_failed(const Dictionary &p_error);
	void _on_asset_log(const String &p_level, const String &p_message);
	void _on_billable_call(const String &p_tool, int p_calls, int p_budget);

	// LLM client -> here
	void _on_models_listed(const Array &p_models);
	void _on_client_log(const String &p_level, const String &p_message);

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
