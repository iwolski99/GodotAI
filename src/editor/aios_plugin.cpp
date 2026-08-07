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

void AIOSPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_client_connected", "client_id", "remote"), &AIOSPlugin::_on_client_connected);
	ClassDB::bind_method(D_METHOD("_on_client_disconnected", "client_id", "reason"), &AIOSPlugin::_on_client_disconnected);
	ClassDB::bind_method(D_METHOD("_on_message_received", "client_id", "message"), &AIOSPlugin::_on_message_received);
	ClassDB::bind_method(D_METHOD("_on_transport_log", "level", "message"), &AIOSPlugin::_on_transport_log);
	ClassDB::bind_method(D_METHOD("_on_prompt_submitted", "text", "mode"), &AIOSPlugin::_on_prompt_submitted);
	ClassDB::bind_method(D_METHOD("_on_execute_plan_requested", "mode"), &AIOSPlugin::_on_execute_plan_requested);
	ClassDB::bind_method(D_METHOD("_on_stop_requested"), &AIOSPlugin::_on_stop_requested);
	ClassDB::bind_method(D_METHOD("_on_rollback_requested"), &AIOSPlugin::_on_rollback_requested);
	ClassDB::bind_method(D_METHOD("_on_scene_changed", "root"), &AIOSPlugin::_on_scene_changed);
	ClassDB::bind_method(D_METHOD("_on_scene_saved", "path"), &AIOSPlugin::_on_scene_saved);
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
	registry.instantiate();
	registry->setup(world_model, git);
	registry->set_auto_checkpoint((bool)ProjectSettings::get_singleton()->get_setting(SETTING_CHECKPOINT, true));
	ipc.instantiate();

	dock = memnew(AIOSChatDock);
	add_control_to_dock(EditorPlugin::DOCK_SLOT_RIGHT_UL, dock);

	dock->connect("prompt_submitted", Callable(this, "_on_prompt_submitted"));
	dock->connect("execute_plan_requested", Callable(this, "_on_execute_plan_requested"));
	dock->connect("stop_requested", Callable(this, "_on_stop_requested"));
	dock->connect("rollback_requested", Callable(this, "_on_rollback_requested"));

	ipc->connect("client_connected", Callable(this, "_on_client_connected"));
	ipc->connect("client_disconnected", Callable(this, "_on_client_disconnected"));
	ipc->connect("message_received", Callable(this, "_on_message_received"));
	ipc->connect("transport_log", Callable(this, "_on_transport_log"));

	connect("scene_changed", Callable(this, "_on_scene_changed"));
	connect("scene_saved", Callable(this, "_on_scene_saved"));

	_start_transport();
	set_process(true);
}

void AIOSPlugin::_exit_tree() {
	set_process(false);

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

	registry.unref();
	world_model.unref();
	git.unref();
}

void AIOSPlugin::_process(double p_delta) {
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
	if (ipc->get_client_count() == 0) {
		dock->append_log("warn", "No agent is connected, so that prompt went nowhere. Start your agent client and try again.");
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
	Dictionary data;
	data["mode"] = p_mode;
	ipc->broadcast_event("execute_plan", data);
	dock->append_log("info", "Asked the agent to execute its current plan.");
}

void AIOSPlugin::_on_stop_requested() {
	ipc->broadcast_event("stop", Dictionary());
	dock->set_state_name("IDLE", "stopped by user");
	dock->append_log("warn", "Stop requested. The agent should abandon its current step.");

	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei != nullptr && ei->is_playing_scene()) {
		ei->stop_playing_scene();
		dock->append_log("info", "Stopped the running playtest.");
	}
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
