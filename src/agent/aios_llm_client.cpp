/**************************************************************************/
/*  aios_llm_client.cpp                                                   */
/**************************************************************************/

#include "aios_llm_client.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

void AIOSLlmClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_request_completed", "result", "code", "headers", "body"), &AIOSLlmClient::_on_request_completed);
	ClassDB::bind_method(D_METHOD("_on_models_completed", "result", "code", "headers", "body"), &AIOSLlmClient::_on_models_completed);

	ClassDB::bind_method(D_METHOD("set_config", "config"), &AIOSLlmClient::set_config);
	ClassDB::bind_method(D_METHOD("get_config"), &AIOSLlmClient::get_config);
	ClassDB::bind_method(D_METHOD("set_system_prompt", "prompt"), &AIOSLlmClient::set_system_prompt);
	ClassDB::bind_method(D_METHOD("set_tools", "tools"), &AIOSLlmClient::set_tools);
	ClassDB::bind_method(D_METHOD("set_max_turns", "turns"), &AIOSLlmClient::set_max_turns);
	ClassDB::bind_method(D_METHOD("reset_conversation"), &AIOSLlmClient::reset_conversation);
	ClassDB::bind_method(D_METHOD("restore_conversation", "history", "turn_count", "system_prompt"),
			&AIOSLlmClient::restore_conversation);
	ClassDB::bind_method(D_METHOD("get_history"), &AIOSLlmClient::get_history);
	ClassDB::bind_method(D_METHOD("get_turn_count"), &AIOSLlmClient::get_turn_count);
	ClassDB::bind_method(D_METHOD("send_user_message", "text"), &AIOSLlmClient::send_user_message);
	ClassDB::bind_method(D_METHOD("send_tool_results", "results"), &AIOSLlmClient::send_tool_results);
	ClassDB::bind_method(D_METHOD("cancel"), &AIOSLlmClient::cancel);
	ClassDB::bind_method(D_METHOD("is_busy"), &AIOSLlmClient::is_busy);
	ClassDB::bind_method(D_METHOD("fetch_models"), &AIOSLlmClient::fetch_models);
	ClassDB::bind_method(D_METHOD("is_configured"), &AIOSLlmClient::is_configured);
	ClassDB::bind_method(D_METHOD("describe_target"), &AIOSLlmClient::describe_target);

	ADD_SIGNAL(MethodInfo("response_received", PropertyInfo(Variant::DICTIONARY, "response")));
	ADD_SIGNAL(MethodInfo("request_failed", PropertyInfo(Variant::DICTIONARY, "error")));
	ADD_SIGNAL(MethodInfo("models_listed", PropertyInfo(Variant::ARRAY, "models")));
	ADD_SIGNAL(MethodInfo("client_log", PropertyInfo(Variant::STRING, "level"), PropertyInfo(Variant::STRING, "message")));
}

void AIOSLlmClient::_ready() {
	http = memnew(HTTPRequest);
	// Reasoning models on hard tasks routinely run for minutes; the default
	// timeout would cut off exactly the requests worth waiting for.
	http->set_timeout(600.0);
	// Threaded requests keep the editor's frame loop responsive while a large
	// response body is being received and decompressed.
	http->set_use_threads(true);
	http->set_accept_gzip(true);
	add_child(http);
	http->connect("request_completed", Callable(this, "_on_request_completed"));

	models_http = memnew(HTTPRequest);
	models_http->set_timeout(30.0);
	models_http->set_use_threads(true);
	add_child(models_http);
	models_http->connect("request_completed", Callable(this, "_on_models_completed"));
}

void AIOSLlmClient::setup(const Ref<AIOSCredentials> &p_credentials) {
	credentials = p_credentials;
}

void AIOSLlmClient::set_config(const Dictionary &p_config) {
	config = AIOSProviderConfig::from_dict(p_config);
}

bool AIOSLlmClient::is_configured() const {
	if (credentials.is_null()) {
		return false;
	}
	return const_cast<AIOSCredentials *>(credentials.ptr())->has_key(config.provider);
}

String AIOSLlmClient::describe_target() const {
	return config.provider + String(" / ") + config.model;
}

void AIOSLlmClient::reset_conversation() {
	history.clear();
	turn_count = 0;
}

void AIOSLlmClient::restore_conversation(const Array &p_history, int p_turn_count, const String &p_system_prompt) {
	cancel();
	history = p_history;
	AIOSProvider::sanitize_conversation_history(history);
	turn_count = p_turn_count > 0 ? p_turn_count : 0;
	system_prompt = p_system_prompt;
}

/* -------------------------------------------------------------------------- */
/*  Sending                                                                    */
/* -------------------------------------------------------------------------- */

Error AIOSLlmClient::send_user_message(const String &p_text) {
	if (state == STATE_WAITING) {
		emit_signal("client_log", "warn", "A request is already in flight; ignoring the new prompt.");
		return ERR_BUSY;
	}

	Dictionary block;
	block["type"] = "text";
	block["text"] = p_text;
	Array content;
	content.push_back(block);

	Dictionary message;
	message["role"] = "user";
	message["content"] = content;
	history.push_back(message);

	turn_count = 0; // A user prompt starts a new exchange.

	Dictionary built = AIOSProvider::build_request(config, system_prompt, history, tools);
	const String note = String(built["note"]);
	if (!note.is_empty()) {
		emit_signal("client_log", "warn", note);
	}
	return _dispatch(built["body"], config.use_fallbacks && config.provider == "anthropic");
}

Error AIOSLlmClient::send_tool_results(const Array &p_results) {
	if (state == STATE_WAITING) {
		return ERR_BUSY;
	}

	// A tool round trip is one agent turn. Capping them is what stops a model
	// that has misread the situation from looping on the same failing call
	// until the user's credits are gone.
	turn_count++;
	if (turn_count > max_turns) {
		Dictionary details;
		details["turns"] = turn_count;
		details["max_turns"] = max_turns;
		_fail(AIOSJson::error("turn_limit_reached",
				"The agent used " + String::num_int64(turn_count) + " tool turns without finishing, which is the "
				"configured ceiling. Stopping so it cannot loop indefinitely. Raise Max tool turns in the "
				"dock's Settings if the task genuinely needs more steps.",
				details));
		return ERR_BUSY;
	}

	const Array assistant_ids = AIOSProvider::last_assistant_tool_use_ids(history);
	const Array aligned = AIOSProvider::align_tool_results(p_results, assistant_ids);
	history.push_back(AIOSProvider::tool_result_message(aligned));

	Dictionary built = AIOSProvider::build_request(config, system_prompt, history, tools);
	return _dispatch(built["body"], config.use_fallbacks && config.provider == "anthropic");
}

Error AIOSLlmClient::_dispatch(const Dictionary &p_body, bool p_with_fallbacks) {
	if (credentials.is_null()) {
		_fail(AIOSJson::error("not_initialised", "The credential store is unavailable."));
		return ERR_UNCONFIGURED;
	}

	const String key = credentials->get_key(config.provider);
	if (key.is_empty()) {
		const String env_name = AIOSCredentials::env_var_for(config.provider);
		_fail(AIOSJson::error("no_api_key",
				"No API key for " + config.provider + ". Add one in the AI Agent dock's Settings panel, or set the " +
						env_name + " environment variable before starting Godot."));
		return ERR_UNCONFIGURED;
	}

	pending_body = p_body;
	pending_used_fallbacks = p_with_fallbacks;

	const PackedStringArray headers = AIOSProvider::auth_headers(config.provider, key, p_with_fallbacks);
	const String url = AIOSProvider::messages_url(config.provider);
	const String payload = JSON::stringify(p_body);

	state = STATE_WAITING;
	const Error err = http->request(url, headers, HTTPClient::METHOD_POST, payload);
	if (err != OK) {
		state = STATE_IDLE;
		_fail(AIOSJson::error("request_failed",
				"Could not start the HTTP request (error " + String::num_int64(err) + "). Check that Godot has "
				"network access and that no proxy is blocking " + url + "."));
		return err;
	}
	return OK;
}

void AIOSLlmClient::cancel() {
	if (state != STATE_WAITING) {
		return;
	}
	http->cancel_request();
	state = STATE_CANCELLED;
	emit_signal("client_log", "warn", "Model request cancelled.");
	state = STATE_IDLE;
}

void AIOSLlmClient::_fail(const Dictionary &p_error) {
	state = STATE_IDLE;
	emit_signal("request_failed", p_error.has("error") ? Variant(p_error["error"]) : Variant(p_error));
}

/* -------------------------------------------------------------------------- */
/*  Receiving                                                                  */
/* -------------------------------------------------------------------------- */

void AIOSLlmClient::_on_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	(void)p_headers;

	if (state == STATE_CANCELLED) {
		state = STATE_IDLE;
		return;
	}
	state = STATE_IDLE;

	if (p_result != HTTPRequest::RESULT_SUCCESS) {
		String reason;
		switch (p_result) {
			case HTTPRequest::RESULT_CANT_CONNECT:
			case HTTPRequest::RESULT_CANT_RESOLVE:
				reason = "could not reach the provider - check your internet connection and any proxy settings";
				break;
			case HTTPRequest::RESULT_TLS_HANDSHAKE_ERROR:
				reason = "the TLS handshake failed, which usually means a proxy is intercepting HTTPS";
				break;
			case HTTPRequest::RESULT_TIMEOUT:
				reason = "the request timed out. Reasoning models can take minutes; if this keeps happening, lower "
						 "the effort setting or the max tokens";
				break;
			default:
				reason = "the transport failed (result " + String::num_int64(p_result) + ")";
				break;
		}
		_fail(AIOSJson::error("transport_error", "The request to " + config.provider + " failed: " + reason + "."));
		return;
	}

	const String raw = p_body.get_string_from_utf8();

	if (p_code < 200 || p_code >= 300) {
		// A 400 that mentions fallbacks means the account lacks that beta.
		// Retry once without it rather than making the user find a setting for
		// a feature they never asked for.
		if (p_code == 400 && pending_used_fallbacks && raw.to_lower().contains("fallback")) {
			emit_signal("client_log", "info",
					"This account does not have Anthropic's refusal-fallback beta; retrying without it.");
			Dictionary retry_body = pending_body;
			retry_body.erase("fallbacks");
			config.use_fallbacks = false;
			_dispatch(retry_body, false);
			return;
		}
		_fail(AIOSProvider::parse_error(config.provider, p_code, raw));
		return;
	}

	Variant parsed = JSON::parse_string(raw);
	if (parsed.get_type() != Variant::DICTIONARY) {
		_fail(AIOSJson::error("malformed_response",
				"The provider returned a " + String::num_int64(p_code) + " with a body that is not JSON."));
		return;
	}

	Dictionary envelope = AIOSProvider::parse_response(config.provider, parsed);
	if (!(bool)envelope["ok"]) {
		_fail(envelope);
		return;
	}

	Dictionary result = envelope["result"];

	// Append the assistant turn verbatim. On Anthropic this preserves thinking
	// blocks with their signatures, which the API validates on the next turn —
	// rebuilding them from parsed text would break the conversation.
	Array assistant_content = result.get("assistant_content", Array());
	if (assistant_content.size() > 0) {
		Dictionary assistant;
		assistant["role"] = "assistant";
		assistant["content"] = assistant_content;
		history.push_back(assistant);
	}

	emit_signal("response_received", result);
}

/* -------------------------------------------------------------------------- */
/*  Model discovery                                                            */
/* -------------------------------------------------------------------------- */

Error AIOSLlmClient::fetch_models() {
	if (credentials.is_null()) {
		return ERR_UNCONFIGURED;
	}
	const String key = credentials->get_key(config.provider);
	if (key.is_empty() && config.provider != "openrouter") {
		emit_signal("client_log", "warn", "Add an API key before listing models.");
		return ERR_UNCONFIGURED;
	}

	const PackedStringArray headers = AIOSProvider::models_headers(config.provider, key);
	return models_http->request(AIOSProvider::models_url(config.provider), headers, HTTPClient::METHOD_GET, "");
}

void AIOSLlmClient::_on_models_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	(void)p_headers;

	if (p_result != HTTPRequest::RESULT_SUCCESS || p_code < 200 || p_code >= 300) {
		emit_signal("client_log", "warn",
				"Could not fetch the model list from " + config.provider + " (HTTP " + String::num_int64(p_code) +
						"). You can still type a model ID by hand.");
		return;
	}

	Variant parsed = JSON::parse_string(p_body.get_string_from_utf8());
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}

	Array models = AIOSProvider::parse_models(config.provider, parsed);
	emit_signal("models_listed", models);
}
