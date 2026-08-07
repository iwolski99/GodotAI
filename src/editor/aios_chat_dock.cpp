/**************************************************************************/
/*  aios_chat_dock.cpp                                                    */
/**************************************************************************/

#include "aios_chat_dock.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// Palette chosen to sit on Godot's dark editor theme without fighting it.
static const Color COLOR_USER = Color(0.55f, 0.78f, 1.0f);
static const Color COLOR_AGENT = Color(0.85f, 0.87f, 0.92f);
static const Color COLOR_THINKING = Color(0.62f, 0.60f, 0.72f);
static const Color COLOR_INFO = Color(0.60f, 0.68f, 0.78f);
static const Color COLOR_SUCCESS = Color(0.45f, 0.85f, 0.55f);
static const Color COLOR_WARN = Color(0.98f, 0.75f, 0.35f);
static const Color COLOR_ERROR = Color(1.0f, 0.45f, 0.45f);
static const Color COLOR_MUTED = Color(0.55f, 0.57f, 0.62f);

AIOSChatDock::AIOSChatDock() {
	set_name("AI Agent");
	set_custom_minimum_size(Vector2(320, 260));
	_build_ui();
}

void AIOSChatDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_input_gui_input", "event"), &AIOSChatDock::_on_input_gui_input);
	ClassDB::bind_method(D_METHOD("_on_send_pressed"), &AIOSChatDock::_on_send_pressed);
	ClassDB::bind_method(D_METHOD("_on_execute_pressed"), &AIOSChatDock::_on_execute_pressed);
	ClassDB::bind_method(D_METHOD("_on_stop_pressed"), &AIOSChatDock::_on_stop_pressed);
	ClassDB::bind_method(D_METHOD("_on_rollback_pressed"), &AIOSChatDock::_on_rollback_pressed);
	ClassDB::bind_method(D_METHOD("_on_clear_pressed"), &AIOSChatDock::_on_clear_pressed);
	ClassDB::bind_method(D_METHOD("_on_token_pressed"), &AIOSChatDock::_on_token_pressed);

	ClassDB::bind_method(D_METHOD("append_user", "text"), &AIOSChatDock::append_user);
	ClassDB::bind_method(D_METHOD("append_agent", "text"), &AIOSChatDock::append_agent);
	ClassDB::bind_method(D_METHOD("append_thinking", "text"), &AIOSChatDock::append_thinking);
	ClassDB::bind_method(D_METHOD("append_log", "level", "text"), &AIOSChatDock::append_log);
	ClassDB::bind_method(D_METHOD("append_tool_call", "tool", "params"), &AIOSChatDock::append_tool_call);
	ClassDB::bind_method(D_METHOD("append_tool_result", "tool", "ok", "envelope"), &AIOSChatDock::append_tool_result);
	ClassDB::bind_method(D_METHOD("clear_history"), &AIOSChatDock::clear_history);
	ClassDB::bind_method(D_METHOD("set_state_name", "state", "detail"), &AIOSChatDock::set_state_name);
	ClassDB::bind_method(D_METHOD("get_state_name"), &AIOSChatDock::get_state_name);
	ClassDB::bind_method(D_METHOD("set_transport_info", "running", "bind", "port", "mode", "clients"), &AIOSChatDock::set_transport_info);
	ClassDB::bind_method(D_METHOD("set_session_token", "token"), &AIOSChatDock::set_session_token);
	ClassDB::bind_method(D_METHOD("get_selected_mode"), &AIOSChatDock::get_selected_mode);

	ADD_SIGNAL(MethodInfo("prompt_submitted", PropertyInfo(Variant::STRING, "text"), PropertyInfo(Variant::STRING, "mode")));
	ADD_SIGNAL(MethodInfo("execute_plan_requested", PropertyInfo(Variant::STRING, "mode")));
	ADD_SIGNAL(MethodInfo("stop_requested"));
	ADD_SIGNAL(MethodInfo("rollback_requested"));
}

/* -------------------------------------------------------------------------- */
/*  UI construction                                                            */
/* -------------------------------------------------------------------------- */

void AIOSChatDock::_build_ui() {
	VBoxContainer *root = memnew(VBoxContainer);
	root->set_anchors_preset(Control::PRESET_FULL_RECT);
	root->add_theme_constant_override("separation", 4);
	add_child(root);

	// --- status header -----------------------------------------------------
	header_panel = memnew(PanelContainer);
	root->add_child(header_panel);

	HBoxContainer *header = memnew(HBoxContainer);
	header->add_theme_constant_override("separation", 8);
	header_panel->add_child(header);

	state_label = memnew(Label);
	state_label->set_text("[IDLE]");
	state_label->add_theme_color_override("font_color", _state_color(STATE_IDLE));
	header->add_child(state_label);

	detail_label = memnew(Label);
	detail_label->set_text("waiting for an agent");
	detail_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	detail_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	detail_label->add_theme_color_override("font_color", COLOR_MUTED);
	header->add_child(detail_label);

	token_button = memnew(Button);
	token_button->set_text("token");
	token_button->set_flat(true);
	token_button->set_tooltip_text("Copy this session's agent token to the clipboard.");
	token_button->connect("pressed", Callable(this, "_on_token_pressed"));
	header->add_child(token_button);

	connection_label = memnew(Label);
	connection_label->set_text("offline");
	connection_label->add_theme_color_override("font_color", COLOR_MUTED);
	header->add_child(connection_label);

	// --- history -----------------------------------------------------------
	history = memnew(RichTextLabel);
	history->set_use_bbcode(true);
	history->set_scroll_follow(true);
	history->set_selection_enabled(true);
	history->set_focus_mode(Control::FOCUS_CLICK);
	history->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	history->set_custom_minimum_size(Vector2(0, 120));
	root->add_child(history);

	// --- input -------------------------------------------------------------
	input = memnew(TextEdit);
	input->set_placeholder("Describe what to build. Enter sends, Shift+Enter adds a line.");
	input->set_custom_minimum_size(Vector2(0, 76));
	input->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	input->connect("gui_input", Callable(this, "_on_input_gui_input"));
	root->add_child(input);

	// --- action bar --------------------------------------------------------
	HBoxContainer *actions = memnew(HBoxContainer);
	actions->add_theme_constant_override("separation", 4);
	root->add_child(actions);

	mode_selector = memnew(OptionButton);
	mode_selector->add_item("Architect", 0);
	mode_selector->add_item("Coder", 1);
	mode_selector->add_item("Debugger", 2);
	mode_selector->add_item("Playtester", 3);
	mode_selector->select(0);
	mode_selector->set_tooltip_text("The role the agent should adopt. Sent with every prompt.");
	actions->add_child(mode_selector);

	send_button = memnew(Button);
	send_button->set_text("Send");
	send_button->set_tooltip_text("Send the prompt to the connected agent (Enter).");
	send_button->connect("pressed", Callable(this, "_on_send_pressed"));
	actions->add_child(send_button);

	execute_button = memnew(Button);
	execute_button->set_text("Execute Plan");
	execute_button->set_tooltip_text("Tell the agent to execute the plan it just proposed.");
	execute_button->connect("pressed", Callable(this, "_on_execute_pressed"));
	actions->add_child(execute_button);

	stop_button = memnew(Button);
	stop_button->set_text("Stop");
	stop_button->set_tooltip_text("Cancel whatever the agent is doing right now.");
	stop_button->connect("pressed", Callable(this, "_on_stop_pressed"));
	actions->add_child(stop_button);

	rollback_button = memnew(Button);
	rollback_button->set_text("Rollback");
	rollback_button->set_tooltip_text("Revert the most recent agent checkpoint (git revert).");
	rollback_button->connect("pressed", Callable(this, "_on_rollback_pressed"));
	actions->add_child(rollback_button);

	clear_button = memnew(Button);
	clear_button->set_text("Clear");
	clear_button->set_flat(true);
	clear_button->connect("pressed", Callable(this, "_on_clear_pressed"));
	actions->add_child(clear_button);
}

void AIOSChatDock::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		append_log("info", "AI Agent OS dock ready. Waiting for an agent to connect.");
	}
}

/* -------------------------------------------------------------------------- */
/*  Input handling                                                             */
/* -------------------------------------------------------------------------- */

void AIOSChatDock::_on_input_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo()) {
		return;
	}
	const Key code = key->get_keycode();
	if (code != KEY_ENTER && code != KEY_KP_ENTER) {
		return;
	}
	if (key->is_shift_pressed()) {
		return; // Shift+Enter falls through to TextEdit and inserts a newline.
	}
	_submit_prompt();
	input->accept_event();
}

void AIOSChatDock::_submit_prompt() {
	const String text = input->get_text().strip_edges();
	if (text.is_empty()) {
		return;
	}
	input->set_text("");
	append_user(text);
	emit_signal("prompt_submitted", text, get_selected_mode());
}

void AIOSChatDock::_on_send_pressed() {
	_submit_prompt();
}

void AIOSChatDock::_on_execute_pressed() {
	emit_signal("execute_plan_requested", get_selected_mode());
}

void AIOSChatDock::_on_stop_pressed() {
	emit_signal("stop_requested");
}

void AIOSChatDock::_on_rollback_pressed() {
	emit_signal("rollback_requested");
}

void AIOSChatDock::_on_clear_pressed() {
	clear_history();
}

void AIOSChatDock::_on_token_pressed() {
	if (session_token.is_empty()) {
		append_log("warn", "No session token - the bridge is not running.");
		return;
	}
	DisplayServer::get_singleton()->clipboard_set(session_token);
	append_log("info", "Session token copied to the clipboard.");
}

String AIOSChatDock::get_selected_mode() const {
	if (mode_selector == nullptr) {
		return "architect";
	}
	return mode_selector->get_item_text(mode_selector->get_selected()).to_lower();
}

/* -------------------------------------------------------------------------- */
/*  Rendering                                                                  */
/* -------------------------------------------------------------------------- */

// NOTE: String("...") decodes a C literal as Latin-1, not UTF-8 — any non-ASCII
// glyph in this file must go through String::utf8() or it renders as mojibake.
String AIOSChatDock::_escape(const String &p_text) {
	// Agent output is untrusted as far as BBCode is concerned: a stray "[img]"
	// in a log line should render as text, not fetch something.
	return p_text.replace("[", "[lb]");
}

String AIOSChatDock::_markdown_to_bbcode(const String &p_text) {
	// A deliberately small subset. Agents write fenced code and `inline code`
	// constantly; everything else reads fine as plain text.
	PackedStringArray chunks = _escape(p_text).split("```");
	String out;
	for (int i = 0; i < chunks.size(); i++) {
		String chunk = chunks[i];
		if (i % 2 == 1) {
			// Inside a fence: drop an optional language tag on the first line.
			const int newline = chunk.find("\n");
			if (newline >= 0 && newline < 20 && !chunk.substr(0, newline).contains(" ")) {
				chunk = chunk.substr(newline + 1);
			}
			out += "\n[code][color=#c8d4e0]" + chunk + "[/color][/code]\n";
		} else {
			// `inline` -> [code]inline[/code]
			PackedStringArray parts = chunk.split("`");
			for (int j = 0; j < parts.size(); j++) {
				out += (j % 2 == 1) ? "[code]" + parts[j] + "[/code]" : parts[j];
			}
		}
	}
	return out;
}

void AIOSChatDock::_append_line(const String &p_bbcode) {
	if (history == nullptr) {
		return;
	}
	history->append_text(p_bbcode + String("\n"));
	message_count++;
}

void AIOSChatDock::append_user(const String &p_text) {
	_append_line("[color=#8cc7ff][b]you[/b][/color]  " + _markdown_to_bbcode(p_text));
}

void AIOSChatDock::append_agent(const String &p_text) {
	_append_line("[color=#a0e4b0][b]agent[/b][/color]  " + _markdown_to_bbcode(p_text));
}

void AIOSChatDock::append_thinking(const String &p_text) {
	_append_line("[color=#9e9bb8][i]" + _escape(p_text) + "[/i][/color]");
}

void AIOSChatDock::append_log(const String &p_level, const String &p_text) {
	Color color = COLOR_INFO;
	String tag = p_level.to_upper();
	if (p_level == "error") {
		color = COLOR_ERROR;
	} else if (p_level == "warn" || p_level == "warning") {
		color = COLOR_WARN;
		tag = "WARN";
	} else if (p_level == "success") {
		color = COLOR_SUCCESS;
		tag = "OK";
	} else if (p_level == "stdout" || p_level == "stderr") {
		color = p_level == "stderr" ? COLOR_ERROR : COLOR_MUTED;
		tag = p_level.to_upper();
	}

	const String time = Time::get_singleton()->get_time_string_from_system();
	_append_line("[color=#8a8c93]" + time + "[/color] [color=#" + color.to_html(false) + "][" + tag + "][/color] " +
			"[color=#c9ccd4]" + _escape(p_text) + "[/color]");
}

void AIOSChatDock::append_tool_call(const String &p_tool, const Dictionary &p_params) {
	String args = JSON::stringify(p_params);
	if (args.length() > 240) {
		args = args.substr(0, 240) + String("...");
	}
	_append_line("[color=#7fb4d8]" + String::utf8("→ ") + _escape(p_tool) + "[/color] [color=#7a7d85]" + _escape(args) + "[/color]");
}

void AIOSChatDock::append_tool_result(const String &p_tool, bool p_ok, const Dictionary &p_envelope) {
	if (p_ok) {
		Dictionary result = p_envelope.has("result") ? Dictionary(p_envelope["result"]) : Dictionary();
		String summary;
		if (result.has("path")) {
			summary = String(result["path"]);
		} else if (result.has("script")) {
			summary = String(result["script"]);
		} else if (result.has("node")) {
			summary = String(result["node"]);
		}
		if (result.has("checkpoint")) {
			summary += "  @" + String(result["checkpoint"]);
		}
		_append_line("[color=#73d98a]" + String::utf8("✓ ") + _escape(p_tool) + "[/color] [color=#8a8c93]" + _escape(summary) + "[/color]");

		// Warnings are the part a human most needs to see; never bury them.
		Array warnings = AIOSJson::get_array(result, "warnings");
		for (int i = 0; i < warnings.size(); i++) {
			Variant w = warnings[i];
			const String text = w.get_type() == Variant::DICTIONARY ? String(Dictionary(w)["message"]) : String(w);
			_append_line("   [color=#fac059]! " + _escape(text) + "[/color]");
		}
		return;
	}

	Dictionary err = p_envelope.has("error") ? Dictionary(p_envelope["error"]) : Dictionary();
	const String code = err.has("code") ? String(err["code"]) : String("error");
	const String message = err.has("message") ? String(err["message"]) : String("(no message)");
	_append_line("[color=#ff7373]" + String::utf8("✗ ") + _escape(p_tool) + " - " + _escape(code) + "[/color]");
	_append_line("   [color=#e2a0a0]" + _escape(message) + "[/color]");

	if (err.has("details")) {
		Dictionary details = err["details"];
		Array blockers = AIOSJson::get_array(details, "blockers");
		for (int i = 0; i < blockers.size(); i++) {
			Dictionary b = blockers[i];
			_append_line("   [color=#ff9c9c]" + String::utf8("· ") + _escape(String(b["message"])) + "[/color]");
		}
	}
}

void AIOSChatDock::clear_history() {
	if (history != nullptr) {
		history->clear();
	}
	message_count = 0;
	append_log("info", "History cleared.");
}

/* -------------------------------------------------------------------------- */
/*  State                                                                      */
/* -------------------------------------------------------------------------- */

Color AIOSChatDock::_state_color(State p_state) {
	switch (p_state) {
		case STATE_PLANNING:
			return Color(0.62f, 0.72f, 1.0f);
		case STATE_VALIDATING:
			return Color(0.85f, 0.78f, 0.45f);
		case STATE_EXECUTING:
			return Color(0.45f, 0.85f, 0.55f);
		case STATE_PLAYTESTING:
			return Color(0.55f, 0.85f, 0.95f);
		case STATE_REPAIRING:
			return Color(0.98f, 0.65f, 0.35f);
		case STATE_ERROR:
			return Color(1.0f, 0.45f, 0.45f);
		default:
			return Color(0.65f, 0.67f, 0.72f);
	}
}

String AIOSChatDock::_state_name(State p_state) {
	switch (p_state) {
		case STATE_PLANNING:
			return "PLANNING";
		case STATE_VALIDATING:
			return "VALIDATING";
		case STATE_EXECUTING:
			return "EXECUTING";
		case STATE_PLAYTESTING:
			return "PLAYTESTING";
		case STATE_REPAIRING:
			return "REPAIRING";
		case STATE_ERROR:
			return "ERROR";
		default:
			return "IDLE";
	}
}

void AIOSChatDock::set_state_name(const String &p_state, const String &p_detail) {
	const String upper = p_state.to_upper();
	State next = STATE_IDLE;
	if (upper == "PLANNING") {
		next = STATE_PLANNING;
	} else if (upper == "VALIDATING") {
		next = STATE_VALIDATING;
	} else if (upper == "EXECUTING") {
		next = STATE_EXECUTING;
	} else if (upper == "PLAYTESTING") {
		next = STATE_PLAYTESTING;
	} else if (upper == "REPAIRING") {
		next = STATE_REPAIRING;
	} else if (upper == "ERROR") {
		next = STATE_ERROR;
	}

	state = next;
	if (state_label != nullptr) {
		state_label->set_text("[" + _state_name(state) + "]");
		state_label->add_theme_color_override("font_color", _state_color(state));
	}
	if (detail_label != nullptr) {
		detail_label->set_text(p_detail);
	}
}

void AIOSChatDock::set_transport_info(bool p_running, const String &p_bind, int p_port, const String &p_mode, int p_clients) {
	if (connection_label == nullptr) {
		return;
	}
	if (!p_running) {
		connection_label->set_text("offline");
		connection_label->add_theme_color_override("font_color", COLOR_ERROR);
		connection_label->set_tooltip_text("The agent bridge is not listening. Check Project Settings > AI Agent OS.");
		return;
	}
	connection_label->set_text(vformat(String::utf8("%d agent%s · :%d"), p_clients, p_clients == 1 ? "" : "s", p_port));
	connection_label->add_theme_color_override("font_color", p_clients > 0 ? COLOR_SUCCESS : COLOR_MUTED);
	connection_label->set_tooltip_text(vformat("Listening on %s:%d (%s)", p_bind, p_port, p_mode));
}

void AIOSChatDock::set_session_token(const String &p_token) {
	session_token = p_token;
	if (token_button != nullptr) {
		token_button->set_disabled(p_token.is_empty());
	}
}
