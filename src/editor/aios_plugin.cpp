/**************************************************************************/
/*  aios_plugin.cpp                                                       */
/**************************************************************************/

#include "aios_plugin.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/random_number_generator.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#define AIOS_SESSION_DIR "res://.godot/ai_agent_os"
#define AIOS_SESSION_FILE AIOS_SESSION_DIR "/session.json"

#define SETTING_ENABLED "ai_agent_os/transport/enabled"
#define SETTING_MODE "ai_agent_os/transport/mode"
#define SETTING_PORT "ai_agent_os/transport/port"
#define SETTING_BIND "ai_agent_os/transport/bind_address"
#define SETTING_TOKEN "ai_agent_os/transport/require_token"
#define SETTING_CHECKPOINT "ai_agent_os/vcs/auto_checkpoint"

#define SETTING_PROVIDER "ai_agent_os/model/provider"
#define SETTING_ANTHROPIC_MODEL "ai_agent_os/model/anthropic_model"
#define SETTING_OPENROUTER_MODEL "ai_agent_os/model/openrouter_model"
#define SETTING_THINKING "ai_agent_os/model/thinking_enabled"
#define SETTING_SHOW_THINKING "ai_agent_os/model/show_thinking"
#define SETTING_EFFORT "ai_agent_os/model/reasoning_effort"
#define SETTING_MAX_TOKENS "ai_agent_os/model/max_output_tokens"

#define SETTING_MAX_TURNS "ai_agent_os/agent/max_turns"
#define SETTING_MAX_REPAIRS "ai_agent_os/agent/max_repair_attempts"
#define SETTING_AUTO_PLAYTEST "ai_agent_os/agent/auto_playtest"
#define SETTING_AUTO_ROLLBACK "ai_agent_os/agent/auto_rollback"
#define SETTING_REQUIRE_BRIEF "ai_agent_os/agent/require_brief"
#define SETTING_REQUIRE_PLAN "ai_agent_os/agent/require_plan_approval"

#define SETTING_ASSET_DIR "ai_agent_os/assets/target_directory"
#define SETTING_MAX_PAID "ai_agent_os/assets/max_paid_calls"
#define SETTING_ASSET_PROVIDER "ai_agent_os/assets/default_provider"

// Effort is stored as an index so Project Settings can render it as a dropdown;
// this is the mapping to the strings both APIs actually take.
static const char *AIOS_EFFORT_NAMES[] = { "low", "medium", "high", "xhigh", "max" };
static const int AIOS_EFFORT_COUNT = 5;

void AIOSPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_client_connected", "client_id", "remote"), &AIOSPlugin::_on_client_connected);
	ClassDB::bind_method(D_METHOD("_on_client_disconnected", "client_id", "reason"), &AIOSPlugin::_on_client_disconnected);
	ClassDB::bind_method(D_METHOD("_on_message_received", "client_id", "message"), &AIOSPlugin::_on_message_received);
	ClassDB::bind_method(D_METHOD("_on_transport_log", "level", "message"), &AIOSPlugin::_on_transport_log);
	ClassDB::bind_method(D_METHOD("_on_prompt_submitted", "text", "mode"), &AIOSPlugin::_on_prompt_submitted);
	ClassDB::bind_method(D_METHOD("_on_execute_plan_requested", "mode"), &AIOSPlugin::_on_execute_plan_requested);
	ClassDB::bind_method(D_METHOD("_on_stop_requested"), &AIOSPlugin::_on_stop_requested);
	ClassDB::bind_method(D_METHOD("_on_rollback_requested"), &AIOSPlugin::_on_rollback_requested);
	ClassDB::bind_method(D_METHOD("_on_history_cleared"), &AIOSPlugin::_on_history_cleared);
	ClassDB::bind_method(D_METHOD("_on_scene_changed", "root"), &AIOSPlugin::_on_scene_changed);
	ClassDB::bind_method(D_METHOD("_on_scene_saved", "path"), &AIOSPlugin::_on_scene_saved);

	ClassDB::bind_method(D_METHOD("_on_settings_changed", "settings"), &AIOSPlugin::_on_settings_changed);
	ClassDB::bind_method(D_METHOD("_on_api_key_submitted", "provider", "key"), &AIOSPlugin::_on_api_key_submitted);
	ClassDB::bind_method(D_METHOD("_on_api_key_cleared", "provider"), &AIOSPlugin::_on_api_key_cleared);
	ClassDB::bind_method(D_METHOD("_on_models_refresh_requested", "provider"), &AIOSPlugin::_on_models_refresh_requested);
	ClassDB::bind_method(D_METHOD("_on_models_listed", "models"), &AIOSPlugin::_on_models_listed);
	ClassDB::bind_method(D_METHOD("_on_client_log", "level", "message"), &AIOSPlugin::_on_client_log);

	ClassDB::bind_method(D_METHOD("_on_pipeline_stage_changed", "stage", "detail"), &AIOSPlugin::_on_pipeline_stage_changed);
	ClassDB::bind_method(D_METHOD("_on_pipeline_message", "text"), &AIOSPlugin::_on_pipeline_message);
	ClassDB::bind_method(D_METHOD("_on_pipeline_thinking", "text"), &AIOSPlugin::_on_pipeline_thinking);
	ClassDB::bind_method(D_METHOD("_on_pipeline_log", "level", "message"), &AIOSPlugin::_on_pipeline_log);
	ClassDB::bind_method(D_METHOD("_on_pipeline_tool_invoked", "tool", "params"), &AIOSPlugin::_on_pipeline_tool_invoked);
	ClassDB::bind_method(D_METHOD("_on_pipeline_tool_completed", "tool", "ok", "envelope"), &AIOSPlugin::_on_pipeline_tool_completed);
	ClassDB::bind_method(D_METHOD("_on_pipeline_plan_proposed", "plan", "diff_preview"), &AIOSPlugin::_on_pipeline_plan_proposed);
	ClassDB::bind_method(D_METHOD("_on_pipeline_run_finished", "summary"), &AIOSPlugin::_on_pipeline_run_finished);

	ClassDB::bind_method(D_METHOD("_on_playtest_output", "stream", "line"), &AIOSPlugin::_on_playtest_output);
	ClassDB::bind_method(D_METHOD("_on_playtest_finished", "report"), &AIOSPlugin::_on_playtest_finished);

	ClassDB::bind_method(D_METHOD("_on_asset_stage_changed", "stage", "detail"), &AIOSPlugin::_on_asset_stage_changed);
	ClassDB::bind_method(D_METHOD("_on_asset_ready", "result"), &AIOSPlugin::_on_asset_ready);
	ClassDB::bind_method(D_METHOD("_on_asset_failed", "error"), &AIOSPlugin::_on_asset_failed);
	ClassDB::bind_method(D_METHOD("_on_asset_log", "level", "message"), &AIOSPlugin::_on_asset_log);
	ClassDB::bind_method(D_METHOD("_on_billable_call", "tool", "calls", "budget"), &AIOSPlugin::_on_billable_call);
}

String AIOSPlugin::_get_plugin_name() const {
	return "AI Agent OS";
}

bool AIOSPlugin::_has_main_screen() const {
	return false;
}

/* -------------------------------------------------------------------------- */
/*  Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

Variant AIOSPlugin::_setting(const String &p_name, const Variant &p_default, int p_type, const String &p_hint_string) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps->has_setting(p_name)) {
		ps->set_setting(p_name, p_default);
	}
	Dictionary info;
	info["name"] = p_name;
	info["type"] = p_type;
	if (!p_hint_string.is_empty()) {
		info["hint"] = PROPERTY_HINT_ENUM;
		info["hint_string"] = p_hint_string;
	}
	ps->add_property_info(info);
	return ps->get_setting(p_name, p_default);
}

void AIOSPlugin::_register_project_settings() {
	_setting(SETTING_ENABLED, true, Variant::BOOL);
	_setting(SETTING_MODE, 0, Variant::INT, "WebSocket,TCP JSON Lines");
	_setting(SETTING_PORT, 45857, Variant::INT);
	_setting(SETTING_BIND, "127.0.0.1", Variant::STRING);
	_setting(SETTING_TOKEN, true, Variant::BOOL);
	_setting(SETTING_CHECKPOINT, true, Variant::BOOL);

	// The two providers keep separate model settings on purpose: their model ids
	// are not interchangeable ("claude-opus-5" vs "anthropic/claude-opus-4.1"),
	// so sharing one field would break the configuration on every switch.
	_setting(SETTING_PROVIDER, 0, Variant::INT, "Anthropic,OpenRouter");
	_setting(SETTING_ANTHROPIC_MODEL, "claude-opus-5", Variant::STRING);
	_setting(SETTING_OPENROUTER_MODEL, "anthropic/claude-opus-4.1", Variant::STRING);
	_setting(SETTING_THINKING, true, Variant::BOOL);
	_setting(SETTING_SHOW_THINKING, true, Variant::BOOL);
	_setting(SETTING_EFFORT, 2, Variant::INT, "low,medium,high,xhigh,max");
	_setting(SETTING_MAX_TOKENS, 16384, Variant::INT);

	_setting(SETTING_MAX_TURNS, 24, Variant::INT);
	_setting(SETTING_MAX_REPAIRS, 3, Variant::INT);
	_setting(SETTING_AUTO_PLAYTEST, true, Variant::BOOL);
	_setting(SETTING_AUTO_ROLLBACK, true, Variant::BOOL);
	_setting(SETTING_REQUIRE_BRIEF, true, Variant::BOOL);
	_setting(SETTING_REQUIRE_PLAN, false, Variant::BOOL);

	// Generation APIs bill per call, so the ceiling is a setting rather than a
	// constant. -1 disables the guard for anyone who would rather not be asked.
	_setting(SETTING_ASSET_DIR, "res://assets/generated", Variant::STRING);
	_setting(SETTING_MAX_PAID, 10, Variant::INT);
	_setting(SETTING_ASSET_PROVIDER, 0, Variant::INT, "Meshy,Tripo3D");
}

/* -------------------------------------------------------------------------- */
/*  Model configuration                                                        */
/* -------------------------------------------------------------------------- */

Dictionary AIOSPlugin::_read_model_settings() const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const bool openrouter = (int)(int64_t)ps->get_setting(SETTING_PROVIDER, 0) == 1;

	int effort_index = (int)(int64_t)ps->get_setting(SETTING_EFFORT, 2);
	if (effort_index < 0 || effort_index >= AIOS_EFFORT_COUNT) {
		effort_index = 2;
	}

	Dictionary settings;
	settings["provider"] = openrouter ? "openrouter" : "anthropic";
	settings["model"] = ps->get_setting(openrouter ? SETTING_OPENROUTER_MODEL : SETTING_ANTHROPIC_MODEL, "");
	settings["thinking_enabled"] = (bool)ps->get_setting(SETTING_THINKING, true);
	settings["show_thinking"] = (bool)ps->get_setting(SETTING_SHOW_THINKING, true);
	settings["effort"] = AIOS_EFFORT_NAMES[effort_index];
	settings["max_tokens"] = (int)(int64_t)ps->get_setting(SETTING_MAX_TOKENS, 16384);
	return settings;
}

void AIOSPlugin::_write_model_settings(const Dictionary &p_settings) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const bool openrouter = String(p_settings.get("provider", "anthropic")) == "openrouter";

	ps->set_setting(SETTING_PROVIDER, openrouter ? 1 : 0);

	const String model = p_settings.get("model", "");
	if (!model.is_empty()) {
		ps->set_setting(openrouter ? SETTING_OPENROUTER_MODEL : SETTING_ANTHROPIC_MODEL, model);
	}
	ps->set_setting(SETTING_THINKING, (bool)p_settings.get("thinking_enabled", true));
	ps->set_setting(SETTING_SHOW_THINKING, (bool)p_settings.get("show_thinking", true));

	const String effort = p_settings.get("effort", "high");
	for (int i = 0; i < AIOS_EFFORT_COUNT; i++) {
		if (effort == AIOS_EFFORT_NAMES[i]) {
			ps->set_setting(SETTING_EFFORT, i);
			break;
		}
	}
	ps->set_setting(SETTING_MAX_TOKENS, (int)(int64_t)p_settings.get("max_tokens", 16384));

	// Persist immediately. Losing an API-key-adjacent configuration because the
	// editor was closed the wrong way is a bad first impression.
	ps->save();
}

void AIOSPlugin::_apply_model_settings() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const Dictionary settings = _read_model_settings();

	if (llm != nullptr) {
		llm->set_config(settings);
		llm->set_max_turns((int)(int64_t)ps->get_setting(SETTING_MAX_TURNS, 24));
	}
	if (pipeline.is_valid()) {
		pipeline->set_max_repair_attempts((int)(int64_t)ps->get_setting(SETTING_MAX_REPAIRS, 3));
		pipeline->set_auto_playtest((bool)ps->get_setting(SETTING_AUTO_PLAYTEST, true));
		pipeline->set_auto_rollback((bool)ps->get_setting(SETTING_AUTO_ROLLBACK, true));
		pipeline->set_require_brief((bool)ps->get_setting(SETTING_REQUIRE_BRIEF, true));
		pipeline->set_require_plan_approval((bool)ps->get_setting(SETTING_REQUIRE_PLAN, false));
	}
	if (dock != nullptr) {
		dock->set_settings(settings);
	}
	_refresh_key_status();
}

void AIOSPlugin::_refresh_key_status() {
	if (dock == nullptr || credentials.is_null()) {
		return;
	}
	const String provider = _read_model_settings().get("provider", "anthropic");
	const String key = credentials->get_key(provider);
	dock->set_key_status(provider, !key.is_empty(), AIOSCredentials::redact(key),
			credentials->is_from_environment(provider));
}

String AIOSPlugin::_generate_token() const {
	// A loopback handshake secret, not a credential: it exists so a random
	// process (or a web page probing localhost) cannot drive the editor by
	// accident. Anything that can read the project folder can read this.
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->randomize();
	String token;
	for (int i = 0; i < 4; i++) {
		token += String::num_uint64((uint64_t)rng->randi(), 16).lpad(8, "0");
	}
	return token;
}

void AIOSPlugin::_write_session_file() {
	if (!DirAccess::dir_exists_absolute(AIOS_SESSION_DIR)) {
		DirAccess::make_dir_recursive_absolute(AIOS_SESSION_DIR);
	}

	Dictionary session;
	session["protocol"] = "godot-ai-os/1";
	session["host"] = ipc->get_bind_address();
	session["port"] = ipc->get_port();
	session["transport"] = ipc->get_mode() == AIOSIpcServer::MODE_WEBSOCKET ? "websocket" : "tcp-jsonl";
	session["token"] = session_token;
	session["project"] = ProjectSettings::get_singleton()->get_setting("application/config/name", "");
	session["project_path"] = ProjectSettings::get_singleton()->globalize_path("res://");
	session["started_at"] = Time::get_singleton()->get_unix_time_from_system();

	Ref<FileAccess> file = FileAccess::open(AIOS_SESSION_FILE, FileAccess::WRITE);
	if (file.is_null()) {
		UtilityFunctions::push_warning("AI Agent OS: could not write " AIOS_SESSION_FILE);
		return;
	}
	file->store_string(JSON::stringify(session, "  "));
	file->close();
}

void AIOSPlugin::_remove_session_file() {
	if (FileAccess::file_exists(AIOS_SESSION_FILE)) {
		DirAccess::remove_absolute(AIOS_SESSION_FILE);
	}
}

void AIOSPlugin::_start_transport() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!(bool)ps->get_setting(SETTING_ENABLED, true)) {
		dock->append_log("warn", "Agent bridge disabled in Project Settings (ai_agent_os/transport/enabled).");
		dock->set_transport_info(false, "", 0, "", 0);
		return;
	}

	const bool require_token = (bool)ps->get_setting(SETTING_TOKEN, true);
	session_token = require_token ? _generate_token() : String();

	const int port = (int)(int64_t)ps->get_setting(SETTING_PORT, 45857);
	const String bind = ps->get_setting(SETTING_BIND, "127.0.0.1");
	const int mode = (int)(int64_t)ps->get_setting(SETTING_MODE, 0);

	if (ipc->start(port, bind, mode, session_token) != OK) {
		dock->append_log("error", vformat("Could not open the agent bridge on port %d.", port));
		dock->set_transport_info(false, bind, port, "", 0);
		return;
	}

	_write_session_file();
	dock->set_session_token(session_token);
	dock->set_transport_info(true, ipc->get_bind_address(), ipc->get_port(),
			mode == AIOSIpcServer::MODE_WEBSOCKET ? "websocket" : "tcp-jsonl", 0);

	UtilityFunctions::print(vformat("[AI Agent OS] Bridge ready on %s:%d - connection details in %s",
			ipc->get_bind_address(), ipc->get_port(), AIOS_SESSION_FILE));
}

void AIOSPlugin::_enter_tree() {
	_register_project_settings();

	world_model.instantiate();
	git.instantiate();
	git->set_enabled((bool)ProjectSettings::get_singleton()->get_setting(SETTING_CHECKPOINT, true));
	playtest.instantiate();
	registry.instantiate();
	registry->set_auto_checkpoint((bool)ProjectSettings::get_singleton()->get_setting(SETTING_CHECKPOINT, true));
	credentials.instantiate();
	memory.instantiate();
	ipc.instantiate();

	// The LLM client is a Node because HTTPRequest is: it needs a tree to
	// process its own polling. Parenting it to the plugin means it lives and
	// dies with the plugin without any extra bookkeeping.
	llm = memnew(AIOSLlmClient);
	llm->set_name("AIOSLlmClient");
	add_child(llm);
	llm->setup(credentials);

	assets = memnew(AIOSAssetPipeline);
	assets->set_name("AIOSAssetPipeline");
	add_child(assets);
	assets->setup(credentials);

	registry->setup(world_model, git, playtest, assets);
	registry->set_billable_budget((int)(int64_t)ProjectSettings::get_singleton()->get_setting(SETTING_MAX_PAID, 10));

	pipeline.instantiate();
	pipeline->setup(registry, git, playtest, llm);
	pipeline->set_memory(memory);

	dock = memnew(AIOSChatDock);
	add_control_to_dock(EditorPlugin::DOCK_SLOT_RIGHT_UL, dock);

	dock->connect("prompt_submitted", Callable(this, "_on_prompt_submitted"));
	dock->connect("execute_plan_requested", Callable(this, "_on_execute_plan_requested"));
	dock->connect("stop_requested", Callable(this, "_on_stop_requested"));
	dock->connect("rollback_requested", Callable(this, "_on_rollback_requested"));
	dock->connect("history_cleared", Callable(this, "_on_history_cleared"));
	dock->connect("settings_changed", Callable(this, "_on_settings_changed"));
	dock->connect("api_key_submitted", Callable(this, "_on_api_key_submitted"));
	dock->connect("api_key_cleared", Callable(this, "_on_api_key_cleared"));
	dock->connect("models_refresh_requested", Callable(this, "_on_models_refresh_requested"));

	pipeline->connect("stage_changed", Callable(this, "_on_pipeline_stage_changed"));
	pipeline->connect("agent_message", Callable(this, "_on_pipeline_message"));
	pipeline->connect("agent_thinking", Callable(this, "_on_pipeline_thinking"));
	pipeline->connect("pipeline_log", Callable(this, "_on_pipeline_log"));
	pipeline->connect("tool_invoked", Callable(this, "_on_pipeline_tool_invoked"));
	pipeline->connect("tool_completed", Callable(this, "_on_pipeline_tool_completed"));
	pipeline->connect("plan_proposed", Callable(this, "_on_pipeline_plan_proposed"));
	pipeline->connect("run_finished", Callable(this, "_on_pipeline_run_finished"));

	assets->connect("asset_stage_changed", Callable(this, "_on_asset_stage_changed"));
	assets->connect("asset_ready", Callable(this, "_on_asset_ready"));
	assets->connect("asset_failed", Callable(this, "_on_asset_failed"));
	assets->connect("asset_log", Callable(this, "_on_asset_log"));
	registry->connect("billable_call", Callable(this, "_on_billable_call"));

	playtest->connect("playtest_output", Callable(this, "_on_playtest_output"));
	playtest->connect("playtest_finished", Callable(this, "_on_playtest_finished"));

	llm->connect("models_listed", Callable(this, "_on_models_listed"));
	llm->connect("client_log", Callable(this, "_on_client_log"));

	ipc->connect("client_connected", Callable(this, "_on_client_connected"));
	ipc->connect("client_disconnected", Callable(this, "_on_client_disconnected"));
	ipc->connect("message_received", Callable(this, "_on_message_received"));
	ipc->connect("transport_log", Callable(this, "_on_transport_log"));

	connect("scene_changed", Callable(this, "_on_scene_changed"));
	connect("scene_saved", Callable(this, "_on_scene_saved"));

	_start_transport();
	_apply_model_settings();

	if (llm->is_configured()) {
		dock->append_log("info",
				"Built-in agent ready: " + llm->describe_target() +
						". Type a goal — it will ask clarifying questions before building.");
	} else {
		dock->append_log("info", "No API key set yet. Open Settings in this dock to add one, or connect an external agent over the bridge.");
	}

	set_process(true);
}

void AIOSPlugin::_exit_tree() {
	set_process(false);

	// Stop the run before tearing anything down: the pipeline holds references
	// to the registry and the playtest, and a playtest left running would
	// outlive the editor as an orphaned process.
	if (pipeline.is_valid()) {
		pipeline->stop();
	}
	if (playtest.is_valid() && playtest->is_running()) {
		playtest->stop();
	}
	if (llm != nullptr) {
		llm->cancel();
		remove_child(llm);
		memdelete(llm);
		llm = nullptr;
	}
	if (assets != nullptr) {
		assets->cancel();
		remove_child(assets);
		memdelete(assets);
		assets = nullptr;
	}

	if (ipc.is_valid()) {
		ipc->stop();
		ipc.unref();
	}
	_remove_session_file();

	if (dock != nullptr) {
		remove_control_from_docks(dock);
		memdelete(dock);
		dock = nullptr;
	}

	pipeline.unref();
	registry.unref();
	world_model.unref();
	playtest.unref();
	credentials.unref();
	git.unref();
}

void AIOSPlugin::_process(double p_delta) {
	// The pipeline drives the playtest tail from here, so it has to run before
	// the early-out below — a playtest must keep being read even if the
	// transport was never started.
	if (pipeline.is_valid()) {
		pipeline->poll(p_delta);
	}
	if (assets != nullptr) {
		assets->poll(p_delta);
	}

	if (ipc.is_null()) {
		return;
	}
	ipc->poll();

	// The client count only changes on connect/disconnect, but the header also
	// shows liveness, so refresh it on a slow timer rather than every frame.
	status_accumulator += p_delta;
	if (status_accumulator >= 0.5) {
		status_accumulator = 0.0;
		const int clients = ipc->get_client_count();
		if (clients != last_reported_clients && dock != nullptr) {
			last_reported_clients = clients;
			dock->set_transport_info(ipc->is_running(), ipc->get_bind_address(), ipc->get_port(),
					ipc->get_mode() == AIOSIpcServer::MODE_WEBSOCKET ? "websocket" : "tcp-jsonl", clients);
		}
	}
}

/* -------------------------------------------------------------------------- */
/*  Transport events                                                           */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_client_connected(int p_client_id, const String &p_remote) {
	dock->append_log("success", vformat("Agent #%d connected from %s.", p_client_id, p_remote));

	// Hand the newcomer everything it needs to start working without a round
	// trip: the tool manifest and the current world model revision.
	Dictionary hello;
	hello["type"] = "event";
	hello["event"] = "session_ready";
	Dictionary data;
	data["protocol"] = "godot-ai-os/1";
	data["tools"] = registry->list_tools();
	data["world_revision"] = world_model->get_revision();
	hello["data"] = data;
	ipc->send_to(p_client_id, hello);
}

void AIOSPlugin::_on_client_disconnected(int p_client_id, const String &p_reason) {
	dock->append_log("info", vformat("Agent #%d disconnected (%s).", p_client_id, p_reason));
}

void AIOSPlugin::_on_transport_log(const String &p_level, const String &p_message) {
	if (dock != nullptr) {
		dock->append_log(p_level, p_message);
	}
}

void AIOSPlugin::_on_message_received(int p_client_id, const Dictionary &p_message) {
	const String type = p_message.has("type") ? String(p_message["type"]) : String("request");
	if (type == "request" || p_message.has("tool")) {
		_handle_request(p_client_id, p_message);
	} else if (type == "event") {
		_handle_event(p_client_id, p_message);
	} else {
		dock->append_log("warn", "Ignored a message with unknown type '" + type + "'.");
	}
}

void AIOSPlugin::_handle_request(int p_client_id, const Dictionary &p_message) {
	const String tool = p_message.has("tool") ? String(p_message["tool"]) : String();
	const Dictionary params = AIOSJson::get_dict(p_message, "params");

	if (pipeline.is_valid() && pipeline->is_running() && AIOSToolRegistry::is_mutating(tool)) {
		Dictionary response;
		response["type"] = "response";
		if (p_message.has("id")) {
			response["id"] = p_message["id"];
		}
		response["tool"] = tool;
		response["ok"] = false;
		Dictionary details;
		details["stage"] = pipeline->get_stage_name();
		response["error"] = AIOSJson::error("driver_busy",
				"The built-in agent pipeline is active (" + pipeline->get_stage_name() +
						"). Stop it before external agents mutate the project.",
				details)["error"];
		ipc->send_to(p_client_id, response);
		dock->append_log("warn",
				vformat("Rejected mutating IPC call '%s' while the built-in agent is running.", tool));
		return;
	}

	dock->append_tool_call(tool, params);

	Dictionary envelope = registry->call_tool(tool, params);
	const bool ok = envelope.has("ok") && (bool)envelope["ok"];
	dock->append_tool_result(tool, ok, envelope);

	Dictionary response;
	response["type"] = "response";
	if (p_message.has("id")) {
		response["id"] = p_message["id"];
	}
	response["tool"] = tool;
	response["ok"] = ok;
	if (ok) {
		response["result"] = envelope["result"];
	} else {
		response["error"] = envelope["error"];
	}
	ipc->send_to(p_client_id, response);

	// Tell every other agent the world moved under them.
	if (ok && AIOSToolRegistry::is_mutating(tool)) {
		Dictionary data;
		data["revision"] = world_model->get_revision();
		data["cause"] = tool;
		ipc->broadcast_event("world_changed", data);
	}
}

void AIOSPlugin::_handle_event(int p_client_id, const Dictionary &p_message) {
	const String event = p_message.has("event") ? String(p_message["event"]) : String();
	const Dictionary data = AIOSJson::get_dict(p_message, "data");

	if (event == "chat" || event == "message") {
		dock->append_agent(AIOSJson::get_string(data, "text", ""));
	} else if (event == "thinking") {
		dock->append_thinking(AIOSJson::get_string(data, "text", ""));
	} else if (event == "status" || event == "state") {
		dock->set_state_name(AIOSJson::get_string(data, "state", "IDLE"), AIOSJson::get_string(data, "detail", ""));
	} else if (event == "log") {
		dock->append_log(AIOSJson::get_string(data, "level", "info"), AIOSJson::get_string(data, "text", ""));
	} else if (event == "runtime_log") {
		// Forwarded from a running playtest by the runtime log bridge.
		dock->append_log(AIOSJson::get_string(data, "stream", "stdout"), AIOSJson::get_string(data, "text", ""));
	} else {
		dock->append_log("info", vformat("Agent #%d sent an unhandled event '%s'.", p_client_id, event));
	}
}

/* -------------------------------------------------------------------------- */
/*  Dock events                                                                */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_prompt_submitted(const String &p_text, const String &p_mode) {
	// The built-in agent takes precedence when it has a key: configuring one is
	// a deliberate act, and silently routing the prompt to an external client
	// instead would make that configuration look broken.
	if (llm != nullptr && llm->is_configured()) {
		// During the clarification interview, Send continues the conversation
		// instead of starting a brand-new run that would discard the brief.
		if (pipeline.is_valid() && pipeline->is_awaiting_user()) {
			Dictionary continued = pipeline->continue_with_user_answer(p_text);
			if (!(bool)continued["ok"]) {
				Dictionary error = continued["error"];
				dock->append_log("error", String(error["message"]));
			}
			return;
		}
		if (pipeline.is_valid() && pipeline->is_clarifying() && !pipeline->is_awaiting_user()) {
			dock->append_log("warn",
					"The agent is still thinking. Wait for its questions, or press Skip & Build.");
			return;
		}

		Dictionary started;
		if (pipeline.is_valid() && pipeline->has_session_context()) {
			started = pipeline->continue_session(p_text, p_mode);
		} else {
			started = pipeline->start(p_text, p_mode);
		}
		if (!(bool)started["ok"]) {
			Dictionary error = started["error"];
			dock->append_log("error", String(error["message"]));
		}
		return;
	}

	if (ipc->get_client_count() == 0) {
		dock->append_log("warn",
				"Nowhere to send that: no API key is configured for the built-in agent and no external "
				"agent is connected. Open Settings in this dock to add a key.");
		return;
	}
	Dictionary data;
	data["text"] = p_text;
	data["mode"] = p_mode;
	data["world_revision"] = world_model->get_revision();
	ipc->broadcast_event("user_prompt", data);
	dock->set_state_name("PLANNING", "prompt sent to agent");
}

void AIOSPlugin::_on_execute_plan_requested(const String &p_mode) {
	// During clarification the button is "Skip & Build": jump straight to a
	// minimal brief so the human is never trapped in an interview they do not want.
	if (pipeline.is_valid() && (pipeline->is_clarifying() || pipeline->is_awaiting_user())) {
		Dictionary skipped = pipeline->skip_clarification_and_build();
		if (!(bool)skipped["ok"]) {
			Dictionary error = skipped["error"];
			dock->append_log("error", String(error["message"]));
		}
		return;
	}

	if (pipeline.is_valid() && pipeline->is_awaiting_plan_approval()) {
		Dictionary approved = pipeline->approve_plan();
		if (!(bool)approved["ok"]) {
			Dictionary error = approved["error"];
			dock->append_log("error", String(error["message"]));
		}
		return;
	}

	Dictionary data;
	data["mode"] = p_mode;
	ipc->broadcast_event("execute_plan", data);
	dock->append_log("info", "Asked the agent to execute its current plan.");
}

void AIOSPlugin::_on_stop_requested() {
	if (pipeline.is_valid()) {
		pipeline->stop();
	}
	ipc->broadcast_event("stop", Dictionary());
	dock->set_state_name("IDLE", "stopped by user");
	dock->append_log("warn", "Stop requested. The agent should abandon its current step.");

	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei != nullptr && ei->is_playing_scene()) {
		ei->stop_playing_scene();
		dock->append_log("info", "Stopped the running playtest.");
	}
}

void AIOSPlugin::_on_history_cleared() {
	if (pipeline.is_valid()) {
		pipeline->reset_session();
	}
	dock->set_state_name("IDLE", "");
}

void AIOSPlugin::_on_rollback_requested() {
	dock->set_state_name("REPAIRING", "rolling back last checkpoint");
	Dictionary envelope = registry->call_tool("rollback_last", Dictionary());
	const bool ok = envelope.has("ok") && (bool)envelope["ok"];
	dock->append_tool_result("rollback_last", ok, envelope);

	if (ok) {
		EditorInterface *ei = EditorInterface::get_singleton();
		if (ei != nullptr && ei->get_resource_filesystem() != nullptr) {
			ei->get_resource_filesystem()->scan();
		}
		world_model->invalidate();
		Dictionary data;
		data["revision"] = world_model->get_revision();
		data["cause"] = "rollback_last";
		ipc->broadcast_event("world_changed", data);
		dock->append_log("warn", "Scene files changed on disk. Reopen the scene to load the reverted version.");
	}
	dock->set_state_name("IDLE", "");
}

/* -------------------------------------------------------------------------- */
/*  Editor events                                                              */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_scene_changed(Node *p_root) {
	(void)p_root;
	if (world_model.is_valid()) {
		world_model->invalidate();
	}
}

void AIOSPlugin::_on_scene_saved(const String &p_path) {
	if (world_model.is_valid()) {
		world_model->invalidate();
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["path"] = p_path;
		ipc->broadcast_event("scene_saved", data);
	}
}

/* -------------------------------------------------------------------------- */
/*  Settings events                                                            */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_settings_changed(const Dictionary &p_settings) {
	_write_model_settings(p_settings);
	_apply_model_settings();

	if (llm != nullptr) {
		dock->append_log("info", "Agent target is now " + llm->describe_target() + ".");
	}
}

void AIOSPlugin::_on_api_key_submitted(const String &p_provider, const String &p_key) {
	if (credentials.is_null()) {
		return;
	}
	if (credentials->set_key(p_provider, p_key) != OK) {
		dock->append_log("error", "Could not write the credential store. The key was not saved.");
		return;
	}
	// Never log the key. The redacted form is enough to confirm the right one
	// landed, and this pane ends up in screenshots.
	dock->append_log("success", "Saved " + p_provider + " key " + AIOSCredentials::redact(p_key) + ".");
	_refresh_key_status();

	if (llm != nullptr && llm->is_configured()) {
		dock->append_log("info", "Built-in agent ready: " + llm->describe_target() + ".");
	}
}

void AIOSPlugin::_on_api_key_cleared(const String &p_provider) {
	if (credentials.is_null()) {
		return;
	}
	credentials->clear_key(p_provider);
	dock->append_log("info", "Cleared the stored " + p_provider + " key.");
	_refresh_key_status();
}

void AIOSPlugin::_on_models_refresh_requested(const String &p_provider) {
	if (llm == nullptr) {
		return;
	}
	if (!credentials->has_key(p_provider)) {
		dock->append_log("warn", "Cannot list models for " + p_provider + " without a key.");
		return;
	}
	dock->append_log("info", "Fetching the model list from " + p_provider + "...");
	if (llm->fetch_models() != OK) {
		dock->append_log("error", "Could not start the model-list request.");
	}
}

void AIOSPlugin::_on_models_listed(const Array &p_models) {
	if (dock == nullptr) {
		return;
	}
	const String current = _read_model_settings().get("model", "");
	dock->set_model_list(p_models, current);
	dock->append_log("success", vformat("%d models available.", p_models.size()));
}

void AIOSPlugin::_on_client_log(const String &p_level, const String &p_message) {
	if (dock != nullptr) {
		dock->append_log(p_level, p_message);
	}
}

/* -------------------------------------------------------------------------- */
/*  Pipeline events                                                            */
/* -------------------------------------------------------------------------- */

// Everything the pipeline reports goes two places: the dock, so the human can
// watch, and the IPC bridge, so an external tool can watch too. Neither is
// authoritative — the pipeline is — which is why these handlers only forward.

void AIOSPlugin::_on_pipeline_stage_changed(const String &p_stage, const String &p_detail) {
	if (dock != nullptr) {
		dock->set_state_name(p_stage, p_detail);
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["state"] = p_stage;
		data["detail"] = p_detail;
		ipc->broadcast_event("status", data);
	}
}

void AIOSPlugin::_on_pipeline_message(const String &p_text) {
	if (dock != nullptr) {
		dock->append_agent(p_text);
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["text"] = p_text;
		ipc->broadcast_event("agent_message", data);
	}
}

void AIOSPlugin::_on_pipeline_thinking(const String &p_text) {
	if (dock != nullptr) {
		dock->append_thinking(p_text);
	}
}

void AIOSPlugin::_on_pipeline_log(const String &p_level, const String &p_message) {
	if (dock != nullptr) {
		dock->append_log(p_level, p_message);
	}
}

void AIOSPlugin::_on_pipeline_tool_invoked(const String &p_tool, const Dictionary &p_params) {
	if (dock != nullptr) {
		dock->append_tool_call(p_tool, p_params);
	}
}

void AIOSPlugin::_on_pipeline_tool_completed(const String &p_tool, bool p_ok, const Dictionary &p_envelope) {
	if (dock != nullptr) {
		dock->append_tool_result(p_tool, p_ok, p_envelope);
	}
	if (p_ok && AIOSToolRegistry::is_mutating(p_tool) && ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["revision"] = world_model->get_revision();
		data["cause"] = p_tool;
		ipc->broadcast_event("world_changed", data);
	}
}

void AIOSPlugin::_on_pipeline_plan_proposed(const Dictionary &p_plan, const String &p_diff_preview) {
	if (dock != nullptr) {
		dock->append_diff_preview(p_diff_preview);
		dock->append_log("info", "Plan waiting for approval — press Approve Plan to continue.");
	}
}

void AIOSPlugin::_on_pipeline_run_finished(const Dictionary &p_summary) {
	const bool ok = (bool)p_summary.get("ok", false);
	if (dock != nullptr) {
		dock->append_log(ok ? "success" : "warn", String(p_summary.get("message", "Run finished.")));
	}

	// A run that touched the filesystem — a rollback, or scripts written to
	// disk — leaves the editor's cached view stale until it rescans.
	if ((bool)p_summary.get("filesystem_changed", false)) {
		EditorInterface *ei = EditorInterface::get_singleton();
		if (ei != nullptr && ei->get_resource_filesystem() != nullptr) {
			ei->get_resource_filesystem()->scan();
		}
	}
	if (world_model.is_valid()) {
		world_model->invalidate();
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		ipc->broadcast_event("run_finished", p_summary);
	}
}

/* -------------------------------------------------------------------------- */
/*  Playtest events                                                            */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_playtest_output(const String &p_stream, const String &p_line) {
	if (dock != nullptr) {
		dock->append_log(p_stream == "stderr" ? "error" : "info", p_line);
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["stream"] = p_stream;
		data["text"] = p_line;
		ipc->broadcast_event("runtime_log", data);
	}
}

void AIOSPlugin::_on_playtest_finished(const Dictionary &p_report) {
	const String outcome = p_report.get("outcome", "none");
	if (dock != nullptr) {
		dock->append_log(outcome == "clean" ? "success" : "warn", String(p_report.get("summary", "")));
	}
	// The pipeline listens to this signal directly; the broadcast is for
	// external agents, which have no other way to learn the result of a
	// run_playtest they started over the bridge.
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		ipc->broadcast_event("playtest_finished", p_report);
	}
}

/* -------------------------------------------------------------------------- */
/*  Asset pipeline events                                                      */
/* -------------------------------------------------------------------------- */

void AIOSPlugin::_on_asset_stage_changed(const String &p_stage, const String &p_detail) {
	if (dock != nullptr) {
		// Asset generation runs alongside the agent pipeline rather than inside
		// it, so it reports through the log rather than hijacking the stage
		// header — otherwise a download would overwrite "EXECUTING".
		dock->append_log("info", "Asset: " + p_stage.to_lower() + (p_detail.is_empty() ? String() : String(" - ") + p_detail));
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		Dictionary data;
		data["stage"] = p_stage;
		data["detail"] = p_detail;
		ipc->broadcast_event("asset_stage", data);
	}
}

void AIOSPlugin::_on_asset_ready(const Dictionary &p_result) {
	const String path = String(p_result.get("path", ""));
	if (dock != nullptr) {
		dock->append_log("success", "Asset imported: " + path);
		if ((bool)p_result.get("cleanup_requested", false)) {
			dock->append_log("info",
					"Run cleanup_mesh on it to reduce triangles and generate collision before using it in a scene.");
		}
	}
	if (world_model.is_valid()) {
		world_model->invalidate();
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		ipc->broadcast_event("asset_ready", p_result);
	}
}

void AIOSPlugin::_on_asset_failed(const Dictionary &p_error) {
	if (dock != nullptr) {
		dock->append_log("error", "Asset generation failed: " + String(p_error.get("message", "")));
	}
	if (ipc.is_valid() && ipc->get_client_count() > 0) {
		ipc->broadcast_event("asset_failed", p_error);
	}
}

void AIOSPlugin::_on_asset_log(const String &p_level, const String &p_message) {
	if (dock != nullptr) {
		dock->append_log(p_level, p_message);
	}
}

void AIOSPlugin::_on_billable_call(const String &p_tool, int p_calls, int p_budget) {
	if (dock == nullptr) {
		return;
	}
	// Spending is worth saying out loud every single time. The failure mode this
	// guards against is a user discovering the cost after the fact.
	String message = "Paid API call: " + p_tool + " (" + String::num_int64(p_calls);
	if (p_budget >= 0) {
		message += " of " + String::num_int64(p_budget) + " allowed this session";
	} else {
		message += ", no limit set";
	}
	message += ").";
	dock->append_log("warn", message);
}
