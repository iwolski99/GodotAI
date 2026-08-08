/**************************************************************************/
/*  aios_provider.cpp                                                     */
/**************************************************************************/

#include "aios_provider.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#define ANTHROPIC_VERSION "2023-06-01"
// The array form of `fallbacks` uses a different beta date than the "default"
// scalar form; pairing the wrong header with the wrong shape is a 400, so these
// two constants must stay together.
#define ANTHROPIC_FALLBACK_BETA "server-side-fallback-2026-07-01"

/* -------------------------------------------------------------------------- */
/*  Config                                                                     */
/* -------------------------------------------------------------------------- */

Dictionary AIOSProviderConfig::to_dict() const {
	Dictionary d;
	d["provider"] = provider;
	d["model"] = model;
	d["max_tokens"] = max_tokens;
	d["thinking_enabled"] = thinking_enabled;
	d["show_thinking"] = show_thinking;
	d["effort"] = effort;
	d["use_fallbacks"] = use_fallbacks;
	return d;
}

AIOSProviderConfig AIOSProviderConfig::from_dict(const Dictionary &p_dict) {
	AIOSProviderConfig c;
	c.provider = AIOSJson::get_string(p_dict, "provider", c.provider);
	c.model = AIOSJson::get_string(p_dict, "model", c.model);
	c.max_tokens = (int)AIOSJson::get_int(p_dict, "max_tokens", c.max_tokens);
	c.thinking_enabled = AIOSJson::get_bool(p_dict, "thinking_enabled", c.thinking_enabled);
	c.show_thinking = AIOSJson::get_bool(p_dict, "show_thinking", c.show_thinking);
	c.effort = AIOSJson::get_string(p_dict, "effort", c.effort);
	c.use_fallbacks = AIOSJson::get_bool(p_dict, "use_fallbacks", c.use_fallbacks);
	return c;
}

/* -------------------------------------------------------------------------- */
/*  Endpoints and headers                                                      */
/* -------------------------------------------------------------------------- */

String AIOSProvider::messages_url(const String &p_provider) {
	if (p_provider == "openrouter") {
		return "https://openrouter.ai/api/v1/chat/completions";
	}
	return "https://api.anthropic.com/v1/messages";
}

String AIOSProvider::models_url(const String &p_provider) {
	if (p_provider == "openrouter") {
		return "https://openrouter.ai/api/v1/models";
	}
	return "https://api.anthropic.com/v1/models?limit=100";
}

PackedStringArray AIOSProvider::auth_headers(const String &p_provider, const String &p_api_key, bool p_want_fallbacks) {
	PackedStringArray headers;
	headers.push_back("content-type: application/json");

	if (p_provider == "openrouter") {
		headers.push_back("Authorization: Bearer " + p_api_key);
		// OpenRouter uses these for its public leaderboard. Identifying the
		// plugin is polite and helps them (and users) attribute traffic.
		headers.push_back("HTTP-Referer: https://github.com/iwolski99/GodotAI");
		headers.push_back("X-Title: Godot AI Agent OS");
		return headers;
	}

	headers.push_back("x-api-key: " + p_api_key);
	headers.push_back("anthropic-version: " ANTHROPIC_VERSION);
	if (p_want_fallbacks) {
		headers.push_back("anthropic-beta: " ANTHROPIC_FALLBACK_BETA);
	}
	return headers;
}

PackedStringArray AIOSProvider::models_headers(const String &p_provider, const String &p_api_key) {
	PackedStringArray headers;
	if (p_provider == "openrouter") {
		// OpenRouter's model list is public; the key is optional and only
		// affects which models are shown as available to the account.
		if (!p_api_key.is_empty()) {
			headers.push_back("Authorization: Bearer " + p_api_key);
		}
		return headers;
	}
	headers.push_back("x-api-key: " + p_api_key);
	headers.push_back("anthropic-version: " ANTHROPIC_VERSION);
	return headers;
}

/* -------------------------------------------------------------------------- */
/*  Reasoning controls                                                         */
/* -------------------------------------------------------------------------- */

static bool is_valid_effort(const String &p_effort) {
	return p_effort == "low" || p_effort == "medium" || p_effort == "high" ||
			p_effort == "xhigh" || p_effort == "max";
}

String AIOSProvider::clamp_effort(const AIOSProviderConfig &p_config, String &r_note) {
	String effort = p_config.effort;
	if (!is_valid_effort(effort)) {
		r_note = "Unknown effort '" + effort + "'; using 'high'.";
		effort = "high";
	}

	// Anthropic rejects disabled thinking above `high`. Letting that 400 reach
	// the dock would read as an inscrutable API failure for what is really a
	// settings combination, so clamp and say so.
	if (p_config.provider == "anthropic" && !p_config.thinking_enabled &&
			(effort == "xhigh" || effort == "max")) {
		r_note = "Thinking is off, which Anthropic only allows up to 'high' effort - using 'high' instead of '" +
				effort + "'.";
		effort = "high";
	}
	return effort;
}

bool AIOSProvider::supports_reasoning(const AIOSProviderConfig &p_config) {
	if (p_config.provider == "anthropic") {
		return true;
	}
	// OpenRouter accepts `reasoning` for every model and silently ignores it on
	// models without reasoning support, so there is nothing to gate on here.
	return true;
}

/* -------------------------------------------------------------------------- */
/*  Canonical -> OpenAI chat translation                                       */
/* -------------------------------------------------------------------------- */

static Array to_openai_messages(const String &p_system, const Array &p_messages) {
	Array out;

	if (!p_system.strip_edges().is_empty()) {
		Dictionary sys;
		sys["role"] = "system";
		sys["content"] = p_system;
		out.push_back(sys);
	}

	for (int i = 0; i < p_messages.size(); i++) {
		Dictionary msg = p_messages[i];
		const String role = String(msg.get("role", "user"));
		Variant content = msg.get("content", "");

		if (content.get_type() == Variant::STRING) {
			Dictionary m;
			m["role"] = role;
			m["content"] = content;
			out.push_back(m);
			continue;
		}

		Array blocks = content;

		// A canonical user turn carrying tool results becomes one OpenAI
		// message per result, each with role "tool" — the shapes do not line up
		// one-to-one, which is why this is a loop and not a field rename.
		Array tool_results;
		Array tool_uses;
		String text;
		for (int b = 0; b < blocks.size(); b++) {
			if (Variant(blocks[b]).get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary block = blocks[b];
			const String type = String(block.get("type", ""));
			if (type == "text") {
				if (!text.is_empty()) {
					text += "\n";
				}
				text += String(block.get("text", ""));
			} else if (type == "tool_result") {
				tool_results.push_back(block);
			} else if (type == "tool_use") {
				tool_uses.push_back(block);
			}
			// "thinking" blocks are deliberately dropped: OpenAI-shaped APIs
			// have nowhere to put them, and OpenRouter regenerates reasoning
			// per request rather than replaying it.
		}

		// Images that could not travel inside a tool result, to be sent as a
		// follow-up user message. See the comment below.
		Array orphaned_images;

		for (int t = 0; t < tool_results.size(); t++) {
			Dictionary block = tool_results[t];
			Dictionary m;
			m["role"] = "tool";
			m["tool_call_id"] = block.get("tool_use_id", "");

			Variant result_content = block.get("content", "");
			if (result_content.get_type() == Variant::STRING) {
				m["content"] = String(result_content);
			} else if (result_content.get_type() == Variant::ARRAY) {
				// The OpenAI chat-completions schema requires a `tool` message's
				// content to be a plain string — an image block is rejected
				// outright. Anthropic allows images inside a tool_result, so the
				// canonical history legitimately contains them.
				//
				// The workaround the OpenAI ecosystem settled on: keep the text
				// in the tool message and re-send the image as a user message
				// immediately after. The model sees both, in order.
				Array inner = result_content;
				String flat;
				for (int k = 0; k < inner.size(); k++) {
					if (Variant(inner[k]).get_type() != Variant::DICTIONARY) {
						continue;
					}
					Dictionary ib = inner[k];
					const String itype = String(ib.get("type", ""));
					if (itype == "image") {
						orphaned_images.push_back(ib);
					} else if (itype == "text") {
						if (!flat.is_empty()) {
							flat += "\n";
						}
						flat += String(ib.get("text", ""));
					}
				}
				if (orphaned_images.size() > 0 && flat.is_empty()) {
					flat = "(screenshot returned; it follows as the next message)";
				}
				m["content"] = flat;
			} else {
				m["content"] = JSON::stringify(result_content);
			}
			out.push_back(m);
		}

		for (int im = 0; im < orphaned_images.size(); im++) {
			Dictionary block = orphaned_images[im];
			Dictionary source = block.get("source", Dictionary());

			Dictionary url;
			url["url"] = "data:" + String(source.get("media_type", "image/png")) +
					";base64," + String(source.get("data", ""));

			Dictionary part;
			part["type"] = "image_url";
			part["image_url"] = url;

			Array parts;
			parts.push_back(part);

			Dictionary m;
			m["role"] = "user";
			m["content"] = parts;
			out.push_back(m);
		}

		if (!text.is_empty() || tool_uses.size() > 0) {
			Dictionary m;
			m["role"] = role;
			m["content"] = text;
			if (tool_uses.size() > 0) {
				Array calls;
				for (int t = 0; t < tool_uses.size(); t++) {
					Dictionary block = tool_uses[t];
					Dictionary fn;
					fn["name"] = block.get("name", "");
					// OpenAI wants arguments as a JSON *string*, not an object.
					fn["arguments"] = JSON::stringify(block.get("input", Dictionary()));

					Dictionary call;
					call["id"] = block.get("id", "");
					call["type"] = "function";
					call["function"] = fn;
					calls.push_back(call);
				}
				m["tool_calls"] = calls;
			}
			out.push_back(m);
		}
	}

	return out;
}

static Array to_openai_tools(const Array &p_tools) {
	Array out;
	for (int i = 0; i < p_tools.size(); i++) {
		Dictionary tool = p_tools[i];
		Dictionary fn;
		fn["name"] = tool.get("name", "");
		fn["description"] = tool.get("description", "");
		fn["parameters"] = tool.get("input_schema", Dictionary());

		Dictionary wrapper;
		wrapper["type"] = "function";
		wrapper["function"] = fn;
		out.push_back(wrapper);
	}
	return out;
}

/* -------------------------------------------------------------------------- */
/*  Request building                                                           */
/* -------------------------------------------------------------------------- */

Dictionary AIOSProvider::build_request(const AIOSProviderConfig &p_config,
		const String &p_system,
		const Array &p_messages,
		const Array &p_tools) {
	String note;
	const String effort = clamp_effort(p_config, note);

	Dictionary body;
	body["model"] = p_config.model;

	if (p_config.provider == "openrouter") {
		body["messages"] = to_openai_messages(p_system, p_messages);
		body["max_tokens"] = p_config.max_tokens;
		if (p_tools.size() > 0) {
			body["tools"] = to_openai_tools(p_tools);
		}

		Dictionary reasoning;
		if (p_config.thinking_enabled) {
			// OpenRouter normalises `effort` across providers, mapping it onto
			// whatever each upstream model actually exposes.
			reasoning["effort"] = effort;
			// `exclude` keeps reasoning on but withholds the trace, which is
			// what "thinking on, don't show it to me" means.
			reasoning["exclude"] = !p_config.show_thinking;
		} else {
			reasoning["enabled"] = false;
		}
		body["reasoning"] = reasoning;

		Dictionary out;
		out["body"] = body;
		out["note"] = note;
		return out;
	}

	// --- Anthropic ---------------------------------------------------------
	body["max_tokens"] = p_config.max_tokens;
	if (!p_system.strip_edges().is_empty()) {
		body["system"] = p_system;
	}
	body["messages"] = p_messages;
	if (p_tools.size() > 0) {
		body["tools"] = p_tools;
	}

	Dictionary thinking;
	if (p_config.thinking_enabled) {
		// Adaptive is the only supported on-mode on current models; the old
		// budget_tokens form is rejected with a 400.
		thinking["type"] = "adaptive";
		thinking["display"] = p_config.show_thinking ? "summarized" : "omitted";
	} else {
		thinking["type"] = "disabled";
	}
	body["thinking"] = thinking;

	Dictionary output_config;
	output_config["effort"] = effort;
	body["output_config"] = output_config;

	if (p_config.use_fallbacks) {
		// "default" lets Anthropic route by refusal category rather than us
		// pinning a substitute model that will eventually be deprecated.
		body["fallbacks"] = "default";
	}

	// Deliberately absent: temperature, top_p, top_k. Current Claude models
	// reject them with a 400 — steering goes in the system prompt instead.

	Dictionary out;
	out["body"] = body;
	out["note"] = note;
	return out;
}

Dictionary AIOSProvider::tool_result_message(const Array &p_results) {
	Dictionary msg;
	msg["role"] = "user";
	msg["content"] = p_results;
	return msg;
}

static String ensure_unique_tool_id(const String &p_id, int p_index, Dictionary &r_used) {
	String id = p_id.strip_edges();
	if (id.is_empty() || r_used.has(id)) {
		id = "call_" + String::num_int64(p_index) + "_" +
				String::num_uint64((uint64_t)Time::get_singleton()->get_ticks_usec() & 0xfffff);
	}
	r_used[id] = true;
	return id;
}

static void normalize_tool_call_ids(Array &r_calls) {
	Dictionary used;
	for (int i = 0; i < r_calls.size(); i++) {
		Dictionary call = r_calls[i];
		call["id"] = ensure_unique_tool_id(String(call.get("id", "")), i, used);
		r_calls[i] = call;
	}
}

/* -------------------------------------------------------------------------- */
/*  Response parsing                                                           */
/* -------------------------------------------------------------------------- */

static Dictionary parse_anthropic(const Dictionary &p_body) {
	Dictionary out;

	const String stop_reason = String(p_body.get("stop_reason", ""));
	out["stop_reason"] = stop_reason;
	out["model"] = p_body.get("model", "");
	out["usage"] = p_body.get("usage", Dictionary());

	// Check the refusal before touching content: on a pre-output refusal the
	// content array is empty, so anything that indexes content[0] crashes here.
	if (stop_reason == "refusal") {
		Dictionary details = p_body.get("stop_details", Dictionary());
		const String category = String(details.get("category", "unspecified"));
		return AIOSJson::error("refusal",
				"The model's safety classifiers declined this request (category: " + category + "). "
				"This can happen on benign security or life-sciences work. Rephrase, or switch model in the dock.",
				details);
	}

	Array content = p_body.get("content", Array());
	String text;
	String thinking;
	Array tool_calls;

	for (int i = 0; i < content.size(); i++) {
		if (Variant(content[i]).get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary block = content[i];
		const String type = String(block.get("type", ""));
		if (type == "text") {
			if (!text.is_empty()) {
				text += "\n";
			}
			text += String(block.get("text", ""));
		} else if (type == "thinking") {
			const String t = String(block.get("thinking", ""));
			if (!t.is_empty()) {
				if (!thinking.is_empty()) {
					thinking += "\n";
				}
				thinking += t;
			}
		} else if (type == "tool_use") {
			Dictionary call;
			call["id"] = block.get("id", "");
			call["name"] = block.get("name", "");
			call["input"] = block.get("input", Dictionary());
			tool_calls.push_back(call);
		}
	}

	normalize_tool_call_ids(tool_calls);
	int call_idx = 0;
	for (int i = 0; i < content.size(); i++) {
		if (Variant(content[i]).get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary block = content[i];
		if (String(block.get("type", "")) == "tool_use" && call_idx < tool_calls.size()) {
			block["id"] = Dictionary(tool_calls[call_idx])["id"];
			content[i] = block;
			call_idx++;
		}
	}

	out["text"] = text;
	out["thinking"] = thinking;
	out["tool_calls"] = tool_calls;
	// The raw content array goes back into history verbatim. Thinking blocks
	// carry signatures the API validates on the next turn, so reconstructing
	// this from the parsed fields above would break multi-turn requests.
	out["assistant_content"] = content;

	return AIOSJson::ok(out);
}

static Dictionary parse_openrouter(const Dictionary &p_body) {
	Array choices = p_body.get("choices", Array());
	if (choices.is_empty()) {
		return AIOSJson::error("empty_response", "The provider returned no choices.", p_body);
	}

	Dictionary choice = choices[0];
	Dictionary message = choice.get("message", Dictionary());

	Dictionary out;
	out["stop_reason"] = choice.get("finish_reason", "");
	out["model"] = p_body.get("model", "");
	out["usage"] = p_body.get("usage", Dictionary());

	const String text = String(message.get("content", ""));
	// OpenRouter surfaces reasoning traces on `reasoning`; some upstreams use
	// `reasoning_content` instead, so accept either.
	String thinking = String(message.get("reasoning", ""));
	if (thinking.is_empty()) {
		thinking = String(message.get("reasoning_content", ""));
	}

	Array tool_calls;
	Array raw_calls = message.get("tool_calls", Array());
	for (int i = 0; i < raw_calls.size(); i++) {
		Dictionary raw = raw_calls[i];
		Dictionary fn = raw.get("function", Dictionary());

		Dictionary call;
		call["id"] = raw.get("id", "");
		call["name"] = fn.get("name", "");
		// Arguments arrive as a JSON string. A model can emit malformed JSON
		// here, so a parse failure becomes an empty input the tool layer will
		// reject with a useful message rather than a crash.
		Variant parsed = JSON::parse_string(String(fn.get("arguments", "{}")));
		call["input"] = parsed.get_type() == Variant::DICTIONARY ? parsed : Variant(Dictionary());
		if (parsed.get_type() != Variant::DICTIONARY) {
			call["arguments_unparsed"] = fn.get("arguments", "");
		}
		tool_calls.push_back(call);
	}

	normalize_tool_call_ids(tool_calls);

	out["text"] = text;
	out["thinking"] = thinking;
	out["tool_calls"] = tool_calls;

	// Rebuild canonical content so history stays provider-neutral: a user can
	// switch providers mid-conversation and the transcript still converts.
	Array assistant_content;
	if (!text.is_empty()) {
		Dictionary block;
		block["type"] = "text";
		block["text"] = text;
		assistant_content.push_back(block);
	}
	for (int i = 0; i < tool_calls.size(); i++) {
		Dictionary call = tool_calls[i];
		Dictionary block;
		block["type"] = "tool_use";
		block["id"] = call["id"];
		block["name"] = call["name"];
		block["input"] = call["input"];
		assistant_content.push_back(block);
	}
	out["assistant_content"] = assistant_content;

	return AIOSJson::ok(out);
}

Dictionary AIOSProvider::parse_response(const String &p_provider, const Dictionary &p_body) {
	if (p_provider == "openrouter") {
		return parse_openrouter(p_body);
	}
	return parse_anthropic(p_body);
}

Dictionary AIOSProvider::parse_error(const String &p_provider, int p_status, const String &p_raw_body) {
	String code = "http_" + String::num_int64(p_status);
	String message;

	Variant parsed = JSON::parse_string(p_raw_body);
	if (parsed.get_type() == Variant::DICTIONARY) {
		Dictionary body = parsed;
		Variant error = body.get("error", Variant());
		if (error.get_type() == Variant::DICTIONARY) {
			Dictionary err = error;
			message = String(err.get("message", ""));
			const String api_type = String(err.get("type", ""));
			if (!api_type.is_empty()) {
				code = api_type;
			}
		} else if (error.get_type() == Variant::STRING) {
			message = String(error);
		}
	}

	if (message.is_empty()) {
		// Cap the raw body: a provider that returns an HTML error page would
		// otherwise dump kilobytes of markup into the dock.
		message = p_raw_body.substr(0, 400);
	}

	// Turn the failures users actually hit into instructions rather than
	// status codes.
	String hint;
	if (p_status == 401 || p_status == 403) {
		hint = " Check the API key in the AI Agent dock's Settings panel.";
	} else if (p_status == 404) {
		hint = " The model ID may be wrong for this provider - pick one from the model dropdown.";
	} else if (p_status == 429) {
		hint = " Rate limited. Wait a moment, or lower the reasoning effort to spend fewer tokens.";
	} else if (p_status >= 500) {
		hint = " The provider is having trouble; this is usually worth retrying.";
	}

	Dictionary details;
	details["provider"] = p_provider;
	details["status"] = p_status;
	return AIOSJson::error(code, message + hint, details);
}

Array AIOSProvider::parse_models(const String &p_provider, const Dictionary &p_body) {
	Array out;
	Array data = p_body.get("data", Array());

	for (int i = 0; i < data.size(); i++) {
		if (Variant(data[i]).get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary model = data[i];

		Dictionary entry;
		entry["id"] = model.get("id", "");
		if (p_provider == "openrouter") {
			entry["name"] = model.get("name", model.get("id", ""));
			entry["context"] = model.get("context_length", 0);
			Dictionary pricing = model.get("pricing", Dictionary());
			if (!pricing.is_empty()) {
				entry["prompt_price"] = pricing.get("prompt", "");
				entry["completion_price"] = pricing.get("completion", "");
			}
		} else {
			entry["name"] = model.get("display_name", model.get("id", ""));
			entry["context"] = model.get("max_input_tokens", 0);
			entry["max_output"] = model.get("max_tokens", 0);
		}
		out.push_back(entry);
	}
	return out;
}

Dictionary AIOSProvider::image_block(const String &p_base64, const String &p_media_type) {
	Dictionary source;
	source["type"] = "base64";
	source["media_type"] = p_media_type.is_empty() ? String("image/png") : p_media_type;
	source["data"] = p_base64;

	Dictionary block;
	block["type"] = "image";
	block["source"] = source;
	return block;
}
