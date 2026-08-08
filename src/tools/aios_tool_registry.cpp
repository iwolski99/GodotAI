/**************************************************************************/
/*  aios_tool_registry.cpp                                                */
/**************************************************************************/

#include "aios_tool_registry.h"

#include "../util/aios_json.h"
#include "../validate/aios_validator.h"
#include "aios_scene_tools.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#define AIOS_SCHEMA_DIR "res://addons/godot_ai_os/schemas/"

struct ToolInfo {
	const char *name;
	const char *summary;
	bool mutating;
};

// Order matters only for readability: this is what an agent sees from
// list_tools, and it doubles as the plugin's public surface.
static const ToolInfo TOOL_TABLE[] = {
	{ "get_world_model", "Snapshot of the project: scene tree, node types, scripts, groups and the res:// inventory.", false },
	{ "create_node_safe", "Instantiate a node under a parent with type-checked properties.", true },
	{ "attach_script_safe", "Write a GDScript file and attach it to a node, verifying the base class first.", true },
	{ "safe_delete_node", "Delete a node after auditing children, signals, NodePath properties and script references.", true },
	{ "set_node_properties", "Change properties on an existing node: move it, resize it, retint it, set exported script variables.", true },
	{ "reparent_node", "Move a node to a new parent and/or child index, preserving its world transform.", true },
	{ "connect_signal_safe", "Wire a signal on one node to a method on another, verifying both exist.", true },
	{ "disconnect_signal_safe", "Remove a signal connection.", true },
	{ "create_scene", "Create a new .tscn file with a root node of the given type — the way to build a reusable prefab.", true },
	{ "read_script", "Read a GDScript file with its function index, optionally windowed to a line range.", false },
	{ "patch_script", "Edit part of a script (one function, an append, or a text replacement) without rewriting the whole file.", true },
	{ "save_scene", "Persist the open scene to disk (required before checkpoints can capture scene edits).", true },
	{ "open_scene", "Switch the editor to another scene so the other tools act on it.", false },
	{ "validate_change", "Dry-run a planned tool call through the static checker without executing it.", false },
	{ "validate_scene", "Sweep the open scene for dangling NodePaths, missing resources and broken scripts.", false },
	{ "run_playtest", "Launch the game in a child process and return its runtime errors and stack traces.", false },
	{ "create_checkpoint", "Commit the current project state as a rollback point.", true },
	{ "rollback_last", "Revert the most recent checkpoint.", true },
	{ "list_tools", "This manifest, including the JSON schema of every tool.", false },
	{ "ping", "Liveness check; returns the editor version and world-model revision.", false },
};

static const int TOOL_COUNT = sizeof(TOOL_TABLE) / sizeof(ToolInfo);

void AIOSToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("call_tool", "tool", "params"), &AIOSToolRegistry::call_tool);
	ClassDB::bind_method(D_METHOD("list_tool_names"), &AIOSToolRegistry::list_tool_names);
	ClassDB::bind_method(D_METHOD("list_tools"), &AIOSToolRegistry::list_tools);
	ClassDB::bind_method(D_METHOD("get_tool_schema", "tool"), &AIOSToolRegistry::get_tool_schema);
	ClassDB::bind_method(D_METHOD("set_auto_checkpoint", "enabled"), &AIOSToolRegistry::set_auto_checkpoint);
	ClassDB::bind_method(D_METHOD("is_auto_checkpoint"), &AIOSToolRegistry::is_auto_checkpoint);

	ADD_SIGNAL(MethodInfo("tool_executed",
			PropertyInfo(Variant::STRING, "tool"),
			PropertyInfo(Variant::BOOL, "ok"),
			PropertyInfo(Variant::DICTIONARY, "envelope")));
}

void AIOSToolRegistry::setup(const Ref<AIOSWorldModel> &p_world_model, const Ref<AIOSGitCheckpoint> &p_git,
		const Ref<AIOSPlaytest> &p_playtest) {
	world_model = p_world_model;
	git = p_git;
	playtest = p_playtest;
}

bool AIOSToolRegistry::is_mutating(const String &p_tool) {
	for (int i = 0; i < TOOL_COUNT; i++) {
		if (p_tool == TOOL_TABLE[i].name) {
			return TOOL_TABLE[i].mutating;
		}
	}
	return false;
}

Dictionary AIOSToolRegistry::_load_schema(const String &p_tool) {
	if (schema_cache.has(p_tool)) {
		return schema_cache[p_tool];
	}
	const String path = String(AIOS_SCHEMA_DIR) + p_tool + ".json";
	if (!FileAccess::file_exists(path)) {
		return Dictionary();
	}
	Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	schema_cache[p_tool] = parsed;
	return parsed;
}

Dictionary AIOSToolRegistry::get_tool_schema(const String &p_tool) {
	Dictionary schema = _load_schema(p_tool);
	if (schema.is_empty()) {
		return AIOSJson::error("schema_not_found",
				"No schema shipped for '" + p_tool + "'. Expected " + String(AIOS_SCHEMA_DIR) + p_tool + ".json.");
	}
	return AIOSJson::ok(schema);
}

PackedStringArray AIOSToolRegistry::list_tool_names() const {
	PackedStringArray names;
	for (int i = 0; i < TOOL_COUNT; i++) {
		names.push_back(TOOL_TABLE[i].name);
	}
	return names;
}

Array AIOSToolRegistry::list_tools() {
	Array out;
	for (int i = 0; i < TOOL_COUNT; i++) {
		Dictionary d;
		d["name"] = TOOL_TABLE[i].name;
		d["summary"] = TOOL_TABLE[i].summary;
		d["mutating"] = TOOL_TABLE[i].mutating;
		Dictionary schema = _load_schema(TOOL_TABLE[i].name);
		if (!schema.is_empty()) {
			d["input_schema"] = schema;
		}
		out.push_back(d);
	}
	return out;
}

Dictionary AIOSToolRegistry::call_tool(const String &p_tool, const Dictionary &p_params) {
	const uint64_t started = Time::get_singleton()->get_ticks_usec();
	Dictionary envelope;

	bool known = false;
	for (int i = 0; i < TOOL_COUNT; i++) {
		if (p_tool == TOOL_TABLE[i].name) {
			known = true;
			break;
		}
	}
	if (!known) {
		Dictionary details;
		details["available"] = list_tool_names();
		envelope = AIOSJson::error("unknown_tool", "No tool named '" + p_tool + "'.", details);
		emit_signal("tool_executed", p_tool, false, envelope);
		return envelope;
	}

	const bool dry_run = AIOSJson::get_bool(p_params, "dry_run", false);

	if (p_tool == "get_world_model") {
		envelope = world_model.is_valid() ? world_model->get_world_model(p_params)
										  : AIOSJson::error("not_initialised", "World model is unavailable.");
	} else if (p_tool == "create_node_safe") {
		envelope = AIOSSceneTools::create_node_safe(p_params);
	} else if (p_tool == "attach_script_safe") {
		envelope = AIOSSceneTools::attach_script_safe(p_params);
	} else if (p_tool == "safe_delete_node") {
		envelope = AIOSSceneTools::safe_delete_node(p_params);
	} else if (p_tool == "set_node_properties") {
		envelope = AIOSSceneTools::set_node_properties(p_params);
	} else if (p_tool == "reparent_node") {
		envelope = AIOSSceneTools::reparent_node(p_params);
	} else if (p_tool == "connect_signal_safe") {
		envelope = AIOSSceneTools::connect_signal_safe(p_params);
	} else if (p_tool == "disconnect_signal_safe") {
		envelope = AIOSSceneTools::disconnect_signal_safe(p_params);
	} else if (p_tool == "create_scene") {
		envelope = AIOSSceneTools::create_scene(p_params);
	} else if (p_tool == "read_script") {
		envelope = AIOSSceneTools::read_script(p_params);
	} else if (p_tool == "patch_script") {
		envelope = AIOSSceneTools::patch_script(p_params);
	} else if (p_tool == "save_scene") {
		envelope = AIOSSceneTools::save_scene(p_params);
	} else if (p_tool == "open_scene") {
		envelope = AIOSSceneTools::open_scene(p_params);
	} else if (p_tool == "validate_change") {
		envelope = AIOSValidator::validate_planned_call(
				AIOSJson::get_string(p_params, "tool", ""),
				AIOSJson::get_dict(p_params, "params"));
	} else if (p_tool == "validate_scene") {
		envelope = AIOSValidator::validate_scene();
	} else if (p_tool == "run_playtest") {
		// Asynchronous by nature: this returns as soon as the child process is
		// up, and the report is delivered later as a `playtest_finished` event.
		// The in-editor pipeline intercepts this tool before dispatch so it can
		// await the report inline; an external agent over IPC gets the event.
		envelope = playtest.is_valid() ? playtest->start(p_params)
									   : AIOSJson::error("not_initialised", "Playtesting is unavailable.");
	} else if (p_tool == "create_checkpoint") {
		envelope = git.is_valid() ? git->create_checkpoint(AIOSJson::get_string(p_params, "label", "manual checkpoint"))
								  : AIOSJson::error("not_initialised", "Checkpointing is unavailable.");
	} else if (p_tool == "rollback_last") {
		envelope = git.is_valid() ? git->rollback_last()
								  : AIOSJson::error("not_initialised", "Checkpointing is unavailable.");
	} else if (p_tool == "list_tools") {
		Dictionary result;
		result["tools"] = list_tools();
		result["protocol"] = "godot-ai-os/1";
		envelope = AIOSJson::ok(result);
	} else if (p_tool == "ping") {
		Dictionary result;
		result["pong"] = true;
		result["revision"] = world_model.is_valid() ? world_model->get_revision() : 0;
		result["server_time"] = Time::get_singleton()->get_unix_time_from_system();
		if (git.is_valid()) {
			result["vcs"] = git->get_status();
		}
		envelope = AIOSJson::ok(result);
	}

	const bool ok = envelope.has("ok") && (bool)envelope["ok"];

	// Anything that touched the tree makes the cached model a lie.
	if (ok && !dry_run && is_mutating(p_tool) && world_model.is_valid()) {
		world_model->invalidate();
	}

	// Checkpoint after the fact so the commit contains the finished edit. The
	// checkpoint tools are excluded to avoid recursing into themselves.
	if (ok && !dry_run && auto_checkpoint && git.is_valid() && is_mutating(p_tool) &&
			p_tool != "create_checkpoint" && p_tool != "rollback_last") {
		Dictionary checkpoint = git->create_checkpoint(p_tool);
		if ((bool)checkpoint["ok"]) {
			Dictionary result = envelope["result"];
			Dictionary cp = checkpoint["result"];
			if ((bool)AIOSJson::get_bool(cp, "created", false)) {
				result["checkpoint"] = cp["short_sha"];
				envelope["result"] = result;
			}
		}
	}

	Dictionary result_or_error = envelope.has("result") ? Dictionary(envelope["result"]) : Dictionary();
	result_or_error["elapsed_msec"] = (double)(Time::get_singleton()->get_ticks_usec() - started) / 1000.0;
	if (envelope.has("result")) {
		envelope["result"] = result_or_error;
	}

	emit_signal("tool_executed", p_tool, ok, envelope);
	return envelope;
}
