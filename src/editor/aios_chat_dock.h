/**************************************************************************/
/*  aios_chat_dock.h                                                      */
/*  The AI Agent dock: chat, live pipeline state and manual overrides.    */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/text_edit.hpp>

using namespace godot;

// A dock is not a nice-to-have here. An agent that edits your project without a
// visible, interruptible trace of what it is doing is not something anyone
// should run twice, so the dock is where the pipeline state, the tool log and
// the two override buttons (stop, rollback) live.
//
// The dock is deliberately dumb: it renders state and emits intent. Every
// decision belongs to AIOSPlugin, which owns the transport and the tools.
class AIOSChatDock : public Control {
	GDCLASS(AIOSChatDock, Control)

public:
	enum State {
		STATE_IDLE,
		STATE_PLANNING,
		STATE_VALIDATING,
		STATE_EXECUTING,
		STATE_PLAYTESTING,
		STATE_REPAIRING,
		STATE_ERROR,
	};

private:
	PanelContainer *header_panel = nullptr;
	Label *state_label = nullptr;
	Label *detail_label = nullptr;
	Label *connection_label = nullptr;
	RichTextLabel *history = nullptr;
	TextEdit *input = nullptr;
	OptionButton *mode_selector = nullptr;
	Button *send_button = nullptr;
	Button *execute_button = nullptr;
	Button *stop_button = nullptr;
	Button *rollback_button = nullptr;
	Button *clear_button = nullptr;
	Button *token_button = nullptr;

	State state = STATE_IDLE;
	String session_token;
	int message_count = 0;

	void _build_ui();
	void _submit_prompt();
	void _on_input_gui_input(const Ref<InputEvent> &p_event);
	void _on_send_pressed();
	void _on_execute_pressed();
	void _on_stop_pressed();
	void _on_rollback_pressed();
	void _on_clear_pressed();
	void _on_token_pressed();

	void _append_line(const String &p_bbcode);
	static String _escape(const String &p_text);
	static String _markdown_to_bbcode(const String &p_text);
	static Color _state_color(State p_state);
	static String _state_name(State p_state);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	AIOSChatDock();

	// --- rendering ---------------------------------------------------------
	void append_user(const String &p_text);
	void append_agent(const String &p_text);
	void append_thinking(const String &p_text);
	void append_log(const String &p_level, const String &p_text);
	void append_tool_call(const String &p_tool, const Dictionary &p_params);
	void append_tool_result(const String &p_tool, bool p_ok, const Dictionary &p_envelope);
	void clear_history();

	// --- state -------------------------------------------------------------
	void set_state_name(const String &p_state, const String &p_detail);
	String get_state_name() const { return _state_name(state); }
	void set_transport_info(bool p_running, const String &p_bind, int p_port, const String &p_mode, int p_clients);
	void set_session_token(const String &p_token);

	String get_selected_mode() const;
};
