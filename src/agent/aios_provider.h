/**************************************************************************/
/*  aios_provider.h                                                       */
/*  Request building and response normalisation per LLM provider.         */
/**************************************************************************/

#pragma once

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

// Two providers, one internal conversation format.
//
// The canonical format is Anthropic's: messages are {role, content[]} where
// content blocks are text / thinking / tool_use / tool_result. It wins as the
// internal representation because it is the lossier conversion in one
// direction only — thinking blocks and their signatures have no OpenAI-shaped
// equivalent, so storing OpenAI-native and converting up would discard data the
// Anthropic API requires to be echoed back verbatim.
//
// So: Anthropic requests are near-passthrough; OpenRouter requests are
// translated into OpenAI chat-completions shape on the way out and translated
// back on the way in.
struct AIOSProviderConfig {
	String provider = "anthropic"; // "anthropic" | "openrouter"
	String model = "claude-opus-5";
	int max_tokens = 16384;

	// Reasoning. `thinking_enabled` is the on/off switch; `effort` is the depth.
	// They interact: see clamp_effort() for the one combination the Anthropic
	// API rejects outright.
	bool thinking_enabled = true;
	bool show_thinking = true; // Anthropic display:"summarized" / OpenRouter !exclude
	String effort = "high"; // low | medium | high | xhigh | max

	// Anthropic's server-side refusal fallback. Costs nothing when unused and
	// rescues a request the safety classifiers decline, so it defaults on; the
	// client retries without it if the account lacks the beta.
	bool use_fallbacks = true;

	Dictionary to_dict() const;
	static AIOSProviderConfig from_dict(const Dictionary &p_dict);
};

class AIOSProvider {
public:
	// --- endpoints ---------------------------------------------------------
	static String messages_url(const String &p_provider);
	static String models_url(const String &p_provider);
	static PackedStringArray auth_headers(const String &p_provider, const String &p_api_key, bool p_want_fallbacks);
	static PackedStringArray models_headers(const String &p_provider, const String &p_api_key);

	// --- requests ----------------------------------------------------------
	// p_messages is the canonical (Anthropic-shaped) history; p_tools is the
	// tool manifest in Anthropic shape ({name, description, input_schema}).
	static Dictionary build_request(const AIOSProviderConfig &p_config,
			const String &p_system,
			const Array &p_messages,
			const Array &p_tools);

	// --- responses ---------------------------------------------------------
	// Normalises a provider response into:
	//   { ok, text, thinking, tool_calls[{id,name,input}], stop_reason,
	//     assistant_content (canonical, to append to history), usage, model }
	// or an {ok:false, error:{code,message}} envelope.
	static Dictionary parse_response(const String &p_provider, const Dictionary &p_body);

	// Turns an HTTP error body into a readable {code, message} without leaking
	// the API key that may appear in echoed request data.
	static Dictionary parse_error(const String &p_provider, int p_status, const String &p_raw_body);

	// Normalises a provider's model list into [{id, name, context, note}].
	static Array parse_models(const String &p_provider, const Dictionary &p_body);

	// --- helpers -----------------------------------------------------------
	// Anthropic rejects thinking:disabled above `high` effort with a 400.
	// Rather than let that reach the user as an opaque API error, the request
	// builder clamps and reports what it did.
	static String clamp_effort(const AIOSProviderConfig &p_config, String &r_note);

	// Builds a canonical user message carrying tool results.
	static Dictionary tool_result_message(const Array &p_results);

	// True when the provider/model pair supports a reasoning control at all.
	static bool supports_reasoning(const AIOSProviderConfig &p_config);
};
