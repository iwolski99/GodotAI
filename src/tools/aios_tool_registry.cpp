/**************************************************************************/
/*  aios_tool_registry.cpp                                                */
/**************************************************************************/

#include "aios_tool_registry.h"

#include "../util/aios_json.h"
#include "../assets/aios_asset_pipeline.h"
#include "../assets/aios_blender_bridge.h"
#include "../validate/aios_validator.h"
#include "../vision/aios_vision.h"
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
	{ "ask_user", "Pause and ask the human clarifying questions before building. Required when the goal is underspecified.", false },
	{ "commit_brief", "Lock in a 2D/3D game design brief after clarification; unlocks build tools.", false },
	{ "propose_plan", "Propose a step-by-step plan for human review before mutating the project.", false },
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
	{ "import_asset", "Copy an image or audio file into res:// and trigger Godot's import pipeline.", true },
	{ "save_scene", "Persist the open scene to disk (required before checkpoints can capture scene edits).", true },
	{ "open_scene", "Switch the editor to another scene so the other tools act on it.", false },
	{ "validate_change", "Dry-run a planned tool call through the static checker without executing it.", false },
	{ "validate_scene", "Sweep the open scene for dangling NodePaths, missing resources and broken scripts.", false },
	{ "run_playtest", "Launch the game in a child process and return its runtime errors and stack traces.", false },
	{ "capture_viewport_screenshot", "Capture the editor viewport as a PNG. Requires a vision-capable model to see the image; otherwise you get the saved file path only.", false },
	{ "generate_3d_asset", "Generate a 3D model from a text prompt via Meshy or Tripo3D, download it, and import it. Costs money per call.", true },
	{ "import_asset_from_url", "Download any asset URL into the project and import it. Works with providers this plugin does not know about.", true },
	{ "cleanup_mesh", "Run a mesh through headless Blender: reduce triangles, normalise scale, and tag collision geometry.", true },
	{ "run_blender_script", "Execute a Blender Python script headless and import what it exports. For procedural geometry.", true },
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
	ClassDB::bind_method(D_METHOD("set_billable_budget", "budget"), &AIOSToolRegistry::set_billable_budget);
	ClassDB::bind_method(D_METHOD("get_billable_calls"), &AIOSToolRegistry::get_billable_calls);
	ClassDB::bind_method(D_METHOD("reset_billable_calls"), &AIOSToolRegistry::reset_billable_calls);

	ADD_SIGNAL(MethodInfo("tool_executed",
			PropertyInfo(Variant::STRING, "tool"),
			PropertyInfo(Variant::BOOL, "ok"),
			PropertyInfo(Variant::DICTIONARY, "envelope")));

	ADD_SIGNAL(MethodInfo("billable_call",
			PropertyInfo(Variant::STRING, "tool"),
			PropertyInfo(Variant::INT, "calls_made"),
			PropertyInfo(Variant::INT, "budget")));
}

void AIOSToolRegistry::setup(const Ref<AIOSWorldModel> &p_world_model, const Ref<AIOSGitCheckpoint> &p_git,
		const Ref<AIOSPlaytest> &p_playtest, AIOSAssetPipeline *p_assets) {
	world_model = p_world_model;
	git = p_git;
	playtest = p_playtest;
	asset_pipeline = p_assets;
}

bool AIOSToolRegistry::is_billable(const String &p_tool) {
	return p_tool == "generate_3d_asset";
}

bool AIOSToolRegistry::is_mutating(const String &p_tool) {
	for (int i = 0; i < TOOL_COUNT; i++) {
		if (p_tool == TOOL_TABLE[i].name) {
			return TOOL_TABLE[i].mutating;
		}
	}
	return false;
}

bool AIOSToolRegistry::is_scene_edit(const String &p_tool) {
	return p_tool == "create_node_safe" || p_tool == "attach_script_safe" || p_tool == "safe_delete_node" ||
			p_tool == "set_node_properties" || p_tool == "reparent_node" || p_tool == "connect_signal_safe" ||
			p_tool == "disconnect_signal_safe" || p_tool == "create_scene" || p_tool == "patch_script" ||
			p_tool == "save_scene";
}

bool AIOSToolRegistry::is_clarify_phase_tool(const String &p_tool) {
	return p_tool == "ask_user" || p_tool == "commit_brief" || p_tool == "propose_plan" ||
			p_tool == "get_world_model" || p_tool == "list_tools" || p_tool == "ping" ||
			p_tool == "read_script" || p_tool == "open_scene";
}

bool AIOSToolRegistry::is_allowed_for_role(const String &p_tool, const String &p_role) {
	const String role = p_role.to_lower();
	if (role.is_empty() || role == "coder") {
		return true;
	}

	if (role == "architect") {
		// Planning and inspection only — no file or scene mutations.
		return p_tool == "ask_user" || p_tool == "commit_brief" || p_tool == "propose_plan" ||
				p_tool == "get_world_model" || p_tool == "read_script" || p_tool == "open_scene" ||
				p_tool == "validate_change" || p_tool == "validate_scene" || p_tool == "run_playtest" ||
				p_tool == "list_tools" || p_tool == "ping";
	}

	if (role == "debugger") {
		// Fix bugs without destructive or structural changes.
		if (p_tool == "safe_delete_node" || p_tool == "create_node_safe" || p_tool == "create_scene" ||
				p_tool == "rollback_last" || p_tool == "create_checkpoint" || p_tool == "import_asset") {
			return false;
		}
		return true;
	}

	if (role == "playtester") {
		return p_tool == "get_world_model" || p_tool == "read_script" || p_tool == "open_scene" ||
				p_tool == "validate_scene" || p_tool == "run_playtest" || p_tool == "propose_plan" ||
				p_tool == "list_tools" || p_tool == "ping";
	}

	return true;
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

	if (!active_role.is_empty() && !is_allowed_for_role(p_tool, active_role)) {
		Dictionary details;
		details["role"] = active_role;
		details["tool"] = p_tool;
		envelope = AIOSJson::error("role_forbidden",
				"The '" + active_role + "' role cannot call '" + p_tool + "'. Switch mode or use a different tool.",
				details);
		emit_signal("tool_executed", p_tool, false, envelope);
		return envelope;
	}

	const bool dry_run = AIOSJson::get_bool(p_params, "dry_run", false);

	// --- cost guard --------------------------------------------------------
	// Generation APIs bill per call. An agent in a repair loop that calls
	// generate_3d_asset five times because the first result "looked wrong" is
	// spending the user's money, and nothing else in the pipeline would stop
	// it: the call succeeds every time, so there is no error to back off from.
	if (is_billable(p_tool) && !dry_run) {
		if (billable_budget >= 0 && billable_calls >= billable_budget) {
			Dictionary details;
			details["calls_made"] = billable_calls;
			details["budget"] = billable_budget;
			details["tool"] = p_tool;
			envelope = AIOSJson::error("budget_exhausted",
					"This session has already made " + String::num_int64(billable_calls) +
							" paid generation call(s), which is the configured limit. Raise "
							"ai_agent_os/assets/max_paid_calls in Project Settings if you want more, or "
							"reuse an asset you already generated.",
					details);
			emit_signal("tool_executed", p_tool, false, envelope);
			return envelope;
		}
		billable_calls++;
		emit_signal("billable_call", p_tool, billable_calls, billable_budget);
	}

	if (p_tool == "ask_user") {
		// The in-editor pipeline intercepts this and pauses for a dock answer.
		// External harnesses present the questions themselves and treat the
		// returned payload as the interview turn.
		Array questions = AIOSJson::get_array(p_params, "questions");
		if (questions.is_empty()) {
			envelope = AIOSJson::error("missing_parameter",
					"'questions' must contain at least one question object with id and prompt.");
		} else {
			Dictionary result;
			result["queued"] = true;
			result["intro"] = AIOSJson::get_string(p_params, "intro", "");
			result["questions"] = questions;
			result["message"] =
					"Present these questions to the human and continue with their answers. "
					"Do not build until commit_brief has succeeded.";
			envelope = AIOSJson::ok(result);
		}
	} else if (p_tool == "commit_brief") {
		const String dimensions = AIOSJson::get_string(p_params, "dimensions", "").to_lower();
		const String title = AIOSJson::get_string(p_params, "title", "");
		const String summary = AIOSJson::get_string(p_params, "summary", "");
		const String core_loop = AIOSJson::get_string(p_params, "core_loop", "");
		const String scope = AIOSJson::get_string(p_params, "scope", "");
		if (title.is_empty() || summary.is_empty() || core_loop.is_empty() || scope.is_empty()) {
			envelope = AIOSJson::error("missing_parameter",
					"commit_brief requires title, summary, core_loop and scope.");
		} else if (dimensions != "2d" && dimensions != "3d") {
			envelope = AIOSJson::error("invalid_dimensions",
					"'dimensions' must be \"2d\" or \"3d\". Ask the human with ask_user if you do not know.");
		} else {
			Dictionary brief;
			brief["title"] = title;
			brief["dimensions"] = dimensions;
			brief["genre"] = AIOSJson::get_string(p_params, "genre", "");
			brief["summary"] = summary;
			brief["core_loop"] = core_loop;
			brief["controls"] = AIOSJson::get_string(p_params, "controls", "");
			brief["win_lose"] = AIOSJson::get_string(p_params, "win_lose", "");
			brief["scope"] = scope;
			brief["art_direction"] = AIOSJson::get_string(p_params, "art_direction", "");
			brief["technical_notes"] = AIOSJson::get_string(p_params, "technical_notes", "");
			Dictionary result;
			result["committed"] = true;
			result["brief"] = brief;
			result["message"] =
					"Brief locked. Build tools are now available. Implement this brief; prefer the declared "
					"2D or 3D node types (Node2D/CharacterBody2D vs Node3D/CharacterBody3D) and call "
					"get_world_model before the first edit.";
			envelope = AIOSJson::ok(result);
		}
	} else if (p_tool == "propose_plan") {
		const String summary = AIOSJson::get_string(p_params, "summary", "");
		Array steps = AIOSJson::get_array(p_params, "steps");
		if (summary.is_empty() || steps.is_empty()) {
			envelope = AIOSJson::error("missing_parameter",
					"propose_plan requires a non-empty 'summary' and at least one entry in 'steps'.");
		} else {
			Dictionary result;
			result["queued"] = true;
			result["summary"] = summary;
			result["steps"] = steps;
			result["message"] =
					"Plan submitted for human review. Wait for approval before calling mutating tools.";
			envelope = AIOSJson::ok(result);
		}
	} else if (p_tool == "get_world_model") {
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
	} else if (p_tool == "import_asset") {
		envelope = AIOSSceneTools::import_asset(p_params);
	} else if (p_tool == "save_scene") {
		envelope = AIOSSceneTools::save_scene(p_params);
	} else if (p_tool == "open_scene") {
		envelope = AIOSSceneTools::open_scene(p_params);
	} else if (p_tool == "capture_viewport_screenshot") {
		envelope = AIOSVision::capture_viewport_screenshot(p_params);
	} else if (p_tool == "generate_3d_asset") {
		envelope = asset_pipeline != nullptr
				? asset_pipeline->generate_3d_asset(p_params)
				: AIOSJson::error("not_initialised", "Asset generation is unavailable.");
	} else if (p_tool == "import_asset_from_url") {
		envelope = asset_pipeline != nullptr
				? asset_pipeline->import_from_url(p_params)
				: AIOSJson::error("not_initialised", "Asset importing is unavailable.");
	} else if (p_tool == "cleanup_mesh") {
		envelope = AIOSBlenderBridge::cleanup_mesh(p_params);
	} else if (p_tool == "run_blender_script") {
		envelope = AIOSBlenderBridge::run_script(p_params);
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
			Dictionary result = Dictionary(envelope["result"]).duplicate();
			Dictionary cp = checkpoint["result"];
			if ((bool)AIOSJson::get_bool(cp, "created", false)) {
				result["checkpoint"] = cp["short_sha"];
				envelope["result"] = result;
			}
		}
	}

	Dictionary result_or_error =
			envelope.has("result") ? Dictionary(envelope["result"]).duplicate() : Dictionary();
	result_or_error["elapsed_msec"] = (double)(Time::get_singleton()->get_ticks_usec() - started) / 1000.0;
	if (envelope.has("result")) {
		envelope["result"] = result_or_error;
	}

	emit_signal("tool_executed", p_tool, ok, envelope);
	return envelope;
}
