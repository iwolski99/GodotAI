/**************************************************************************/
/*  aios_json.h                                                           */
/*  JSON <-> Variant marshalling helpers for the AI Agent OS.             */
/**************************************************************************/

#pragma once

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

// Conversion helpers between engine Variants and the JSON-safe subset that is
// transported over the IPC link.
//
// Godot's own JSON.stringify() silently degrades non-JSON types (a Vector2
// becomes the string "(1, 2)") which is lossy and impossible for an agent to
// round-trip. We instead encode them as tagged objects:
//
//     Vector2(1, 2)  ->  { "__type": "Vector2", "x": 1, "y": 2 }
//
// and accept both the tagged form and plain shorthands ([1, 2] / {"x":1,"y":2})
// when decoding, so agents can write whichever is convenient.
class AIOSJson {
public:
	// Variant -> JSON-safe Variant (only bool/int/float/String/Array/Dictionary
	// and nested combinations thereof come out).
	static Variant to_json(const Variant &p_value, int p_depth = 0);

	// JSON-safe Variant -> Variant of the requested Variant::Type.
	// Returns true on success. On failure, r_error holds a human-readable
	// explanation that is safe to hand back to the agent.
	static bool coerce(const Variant &p_input, int p_target_type, Variant &r_output, String &r_error);

	// Human-readable name of a Variant::Type ("Vector2", "int", ...).
	static String type_name(int p_type);

	// Convenience accessors with defaults, used all over the tool layer.
	static bool get_bool(const Dictionary &p_dict, const String &p_key, bool p_default);
	static int64_t get_int(const Dictionary &p_dict, const String &p_key, int64_t p_default);
	static String get_string(const Dictionary &p_dict, const String &p_key, const String &p_default);
	static Dictionary get_dict(const Dictionary &p_dict, const String &p_key);
	static Array get_array(const Dictionary &p_dict, const String &p_key);

	// Standard tool envelopes.
	static Dictionary ok(const Dictionary &p_result);
	static Dictionary error(const String &p_code, const String &p_message, const Dictionary &p_details = Dictionary());
};
