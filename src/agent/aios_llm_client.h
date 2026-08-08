/**************************************************************************/
/*  aios_llm_client.h                                                     */
/*  Async LLM transport for the in-editor agent.                          */
/**************************************************************************/

#pragma once

#include "aios_credentials.h"
#include "aios_provider.h"

#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Everything here is asynchronous, and that is not a stylistic choice: a model
// call takes seconds to minutes, and Godot's editor is single-threaded. A
// blocking HTTP call would freeze the entire editor — including the Stop button
// whose whole job is to interrupt a run that has gone wrong.
//
// So this is a Node hosting an HTTPRequest child, driven by signals. It owns the
// conversation history (canonical Anthropic-shaped blocks) so that switching
// provider mid-conversation keeps the transcript intact.
class AIOSLlmClient : public Node {
	GDCLASS(AIOSLlmClient, Node)

public:
	enum State {
		STATE_IDLE,
		STATE_WAITING,
		STATE_CANCELLED,
	};

private:
	HTTPRequest *http = nullptr;
	HTTPRequest *models_http = nullptr;
	Ref<AIOSCredentials> credentials;

	AIOSProviderConfig config;
	String system_prompt;
	Array tools;
	Array history;

	State state = STATE_IDLE;
	int turn_count = 0;
	int max_turns = 24;

	// Set when a request went out with the fallbacks beta. If the account does
	// not have it, the API 400s; we retry once without rather than making the
	// user discover a beta flag they never opted into.
	bool pending_used_fallbacks = false;
	Dictionary pending_body;

	void _on_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_models_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);

	Error _dispatch(const Dictionary &p_body, bool p_with_fallbacks);
	void _fail(const Dictionary &p_error);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	void setup(const Ref<AIOSCredentials> &p_credentials);

	void set_config(const Dictionary &p_config);
	Dictionary get_config() const { return config.to_dict(); }

	void set_system_prompt(const String &p_prompt) { system_prompt = p_prompt; }
	void set_tools(const Array &p_tools) { tools = p_tools; }
	void set_max_turns(int p_turns) { max_turns = p_turns > 0 ? p_turns : 1; }

	// --- conversation ------------------------------------------------------
	void reset_conversation();
	Array get_history() const { return history; }
	int get_turn_count() const { return turn_count; }

	// Starts a fresh exchange from a user prompt.
	Error send_user_message(const String &p_text);

	// Continues an exchange by answering the tool calls the model just made.
	// p_results are canonical tool_result blocks.
	Error send_tool_results(const Array &p_results);

	// Aborts an in-flight request. Safe to call when idle.
	void cancel();

	bool is_busy() const { return state == STATE_WAITING; }

	// --- model discovery ---------------------------------------------------
	Error fetch_models();

	// True when a key is available for the configured provider.
	bool is_configured() const;
	String describe_target() const;
};
