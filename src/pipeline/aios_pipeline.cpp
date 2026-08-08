/**************************************************************************/
/*  aios_pipeline.cpp                                                     */
/**************************************************************************/

#include "aios_pipeline.h"

#include "../util/aios_json.h"
#include "../validate/aios_validator.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

void AIOSPipeline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_model_response", "response"), &AIOSPipeline::_on_model_response);
	ClassDB::bind_method(D_METHOD("_on_model_failed", "error"), &AIOSPipeline::_on_model_failed);
	ClassDB::bind_method(D_METHOD("_on_playtest_finished", "report"), &AIOSPipeline::_on_playtest_finished);

	ClassDB::bind_method(D_METHOD("start", "goal", "mode"), &AIOSPipeline::start);
	ClassDB::bind_method(D_METHOD("continue_session", "text", "mode"), &AIOSPipeline::continue_session);
	ClassDB::bind_method(D_METHOD("reset_session"), &AIOSPipeline::reset_session);
	ClassDB::bind_method(D_METHOD("has_session_context"), &AIOSPipeline::has_session_context);
	ClassDB::bind_method(D_METHOD("continue_with_user_answer", "answer"), &AIOSPipeline::continue_with_user_answer);
	ClassDB::bind_method(D_METHOD("skip_clarification_and_build"), &AIOSPipeline::skip_clarification_and_build);
	ClassDB::bind_method(D_METHOD("stop"), &AIOSPipeline::stop);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &AIOSPipeline::poll);
	ClassDB::bind_method(D_METHOD("get_stage_name"), &AIOSPipeline::get_stage_name);
	ClassDB::bind_method(D_METHOD("get_status"), &AIOSPipeline::get_status);
	ClassDB::bind_method(D_METHOD("set_max_repair_attempts", "attempts"), &AIOSPipeline::set_max_repair_attempts);
	ClassDB::bind_method(D_METHOD("set_auto_playtest", "enabled"), &AIOSPipeline::set_auto_playtest);
	ClassDB::bind_method(D_METHOD("set_auto_rollback", "enabled"), &AIOSPipeline::set_auto_rollback);
	ClassDB::bind_method(D_METHOD("set_require_brief", "enabled"), &AIOSPipeline::set_require_brief);
	ClassDB::bind_method(D_METHOD("set_require_plan_approval", "enabled"), &AIOSPipeline::set_require_plan_approval);
	ClassDB::bind_method(D_METHOD("is_clarifying"), &AIOSPipeline::is_clarifying);
	ClassDB::bind_method(D_METHOD("is_awaiting_user"), &AIOSPipeline::is_awaiting_user);
	ClassDB::bind_method(D_METHOD("is_awaiting_plan_approval"), &AIOSPipeline::is_awaiting_plan_approval);
	ClassDB::bind_method(D_METHOD("approve_plan"), &AIOSPipeline::approve_plan);

	ADD_SIGNAL(MethodInfo("stage_changed", PropertyInfo(Variant::STRING, "stage"), PropertyInfo(Variant::STRING, "detail")));
	ADD_SIGNAL(MethodInfo("agent_message", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("agent_thinking", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("pipeline_log", PropertyInfo(Variant::STRING, "level"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("tool_invoked", PropertyInfo(Variant::STRING, "tool"), PropertyInfo(Variant::DICTIONARY, "params")));
	ADD_SIGNAL(MethodInfo("tool_completed", PropertyInfo(Variant::STRING, "tool"), PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::DICTIONARY, "envelope")));
	ADD_SIGNAL(MethodInfo("user_questions_requested", PropertyInfo(Variant::DICTIONARY, "payload")));
	ADD_SIGNAL(MethodInfo("brief_committed", PropertyInfo(Variant::DICTIONARY, "brief")));
	ADD_SIGNAL(MethodInfo("plan_proposed", PropertyInfo(Variant::DICTIONARY, "plan"), PropertyInfo(Variant::STRING, "diff_preview")));
	ADD_SIGNAL(MethodInfo("run_finished", PropertyInfo(Variant::DICTIONARY, "summary")));
}

void AIOSPipeline::setup(const Ref<AIOSToolRegistry> &p_registry,
		const Ref<AIOSGitCheckpoint> &p_git,
		const Ref<AIOSPlaytest> &p_playtest,
		AIOSLlmClient *p_llm) {
	registry = p_registry;
	git = p_git;
	playtest = p_playtest;
	llm = p_llm;

	if (llm != nullptr) {
		llm->connect("response_received", Callable(this, "_on_model_response"));
		llm->connect("request_failed", Callable(this, "_on_model_failed"));
	}
	if (playtest.is_valid()) {
		playtest->connect("playtest_finished", Callable(this, "_on_playtest_finished"));
	}
}

String AIOSPipeline::get_stage_name() const {
	switch (stage) {
		case STAGE_CLARIFYING:
			return "CLARIFYING";
		case STAGE_PLANNING:
			return "PLANNING";
		case STAGE_VALIDATING:
			return "VALIDATING";
		case STAGE_EXECUTING:
			return "EXECUTING";
		case STAGE_PLAYTESTING:
			return "PLAYTESTING";
		case STAGE_REPAIRING:
			return "REPAIRING";
		case STAGE_AWAITING_APPROVAL:
			return "AWAITING_APPROVAL";
		case STAGE_ERROR:
			return "ERROR";
		default:
			return "IDLE";
	}
}

void AIOSPipeline::_set_stage(Stage p_stage, const String &p_detail) {
	stage = p_stage;
	emit_signal("stage_changed", get_stage_name(), p_detail);
}

Dictionary AIOSPipeline::get_status() const {
	Dictionary d;
	d["stage"] = get_stage_name();
	d["goal"] = goal;
	d["mode"] = mode;
	d["steps_executed"] = steps_executed;
	d["repair_attempts"] = repair_attempts;
	d["max_repair_attempts"] = max_repair_attempts;
	d["step_snapshot"] = step_snapshot.substr(0, 8);
	d["last_good_snapshot"] = last_good_snapshot.substr(0, 8);
	d["auto_playtest"] = auto_playtest;
	d["auto_rollback"] = auto_rollback;
	d["require_brief"] = require_brief;
	d["require_plan_approval"] = require_plan_approval;
	d["clarifying"] = is_clarifying();
	d["awaiting_user"] = awaiting_user;
	d["awaiting_plan_approval"] = awaiting_plan_approval;
	d["plan_approved"] = plan_approved;
	d["brief_ready"] = brief_ready;
	d["brief"] = committed_brief;
	return d;
}

/* -------------------------------------------------------------------------- */
/*  The prompt wrapper                                                         */
/* -------------------------------------------------------------------------- */

// A model repairs what it can see. Handing back "validation failed" produces a
// guess; handing back the specific finding, its location, and what the pipeline
// did about it produces a fix. These three builders are the entire self-healing
// mechanism — everything else is plumbing that gets their text to the model.

String AIOSPipeline::build_validation_feedback(const String &p_tool, const Array &p_findings) {
	String out = "VALIDATION FAILED - this call was NOT executed. The project is unchanged.\n\n";
	out += "Tool: " + p_tool + "\n\nFindings:\n";

	for (int i = 0; i < p_findings.size(); i++) {
		Dictionary f = p_findings[i];
		const String severity = String(f["severity"]).to_upper();
		out += "  [" + severity + "] " + String(f["code"]) + ": " + String(f["message"]) + "\n";

		if (f.has("data")) {
			Dictionary data = f["data"];
			if (data.has("file") && data.has("line")) {
				out += "      at " + String(data["file"]) + ":" + String::num_int64((int64_t)data["line"]) + "\n";
			} else if (data.has("line")) {
				out += "      at line " + String::num_int64((int64_t)data["line"]) + "\n";
			}
			if (data.has("property")) {
				out += "      property: " + String(data["property"]) + "\n";
			}
			if (data.has("path")) {
				out += "      path: " + String(data["path"]) + "\n";
			}
		}
	}

	out += "\nFix the errors above and call the tool again with corrected arguments. ";
	out += "Warnings did not block execution and only need attention if they are actually wrong. ";
	out += "If a finding is a false positive - for example a node reference to something you are about to create ";
	out += "later in this plan - say so and proceed with the corrected call anyway.";
	return out;
}

String AIOSPipeline::build_playtest_feedback(const Dictionary &p_report) {
	const String outcome = String(p_report["outcome"]);
	String out = "PLAYTEST RESULT: " + outcome.to_upper() + "\n";
	out += String(p_report["summary"]) + "\n";
	out += "Scene: " + String(p_report["scene"]) + "\n";
	out += "Ran for " + String::num((double)p_report["elapsed_sec"], 1) + "s. ";
	out += String::num_int64((int64_t)p_report["error_count"]) + " error(s), " +
			String::num_int64((int64_t)p_report["warning_count"]) + " warning(s).\n";

	Array diagnostics = p_report.get("diagnostics", Array());
	if (diagnostics.size() > 0) {
		out += "\nDiagnostics (in the order they were logged):\n";
		// Cap the list: a script erroring every frame produces hundreds of
		// identical lines, and the first few are the ones that explain it.
		const int limit = diagnostics.size() < 25 ? diagnostics.size() : 25;
		for (int i = 0; i < limit; i++) {
			Dictionary d = diagnostics[i];
			out += "  [" + String(d["severity"]).to_upper() + "/" + String(d["kind"]) + "] " +
					String(d["message"]) + "\n";
			if (d.has("file")) {
				out += "      at " + String(d["file"]);
				if (d.has("line")) {
					out += ":" + String::num_int64((int64_t)d["line"]);
				}
				if (d.has("function")) {
					out += " in " + String(d["function"]) + "()";
				}
				out += "\n";
			}
		}
		if (diagnostics.size() > limit) {
			out += "  ... and " + String::num_int64(diagnostics.size() - limit) + " more.\n";
		}
	}

	Array tail = p_report.get("output_tail", Array());
	if (tail.size() > 0 && outcome != "clean") {
		out += "\nLast console output before the run ended:\n";
		for (int i = 0; i < tail.size(); i++) {
			out += "  " + String(tail[i]) + "\n";
		}
	}

	if (outcome == "clean") {
		out += "\nThe scene ran without errors. Continue with the plan, or report that the goal is complete.";
	} else if (outcome == "timeout") {
		out += "\nA timeout is normal for a game with no exit condition - it is not by itself a failure. ";
		out += "Judge the run by the diagnostics above. If there are none, treat this as a pass.";
	} else {
		out += "\nDiagnose from the file and line above rather than guessing. ";
		out += "Read the offending script before editing it, fix the specific cause, then run the playtest again.";
	}
	return out;
}

String AIOSPipeline::build_rollback_notice(const String &p_reason, const String &p_sha) {
	String out = "ROLLBACK PERFORMED.\n\n";
	out += "Reason: " + p_reason + "\n";
	out += "The project was reset to snapshot " + p_sha.substr(0, 8) + " - every file change from your last step ";
	out += "has been undone. The scene tree and the filesystem are back to the state they were in before it.\n\n";
	out += "Your previous approach did not work. Do not retry it unchanged. ";
	out += "Call get_world_model to see the restored state, work out what actually went wrong, and take a ";
	out += "different approach. If you cannot see a different approach, say so plainly instead of retrying.";
	return out;
}

/* -------------------------------------------------------------------------- */
/*  Clarification helpers                                                      */
/* -------------------------------------------------------------------------- */

bool AIOSPipeline::_mode_requires_brief(const String &p_mode) const {
	if (!require_brief) {
		return false;
	}
	const String m = p_mode.to_lower();
	// Debugger and playtester work on an existing game; interviewing would be noise.
	return m == "architect" || m == "coder" || m.is_empty();
}

void AIOSPipeline::_apply_tools_for_phase() {
	if (llm == nullptr || !registry.is_valid()) {
		return;
	}

	Array manifest = registry->list_tools();
	Array tools;
	for (int i = 0; i < manifest.size(); i++) {
		Dictionary entry = manifest[i];
		const String name = String(entry["name"]);
		if (is_clarifying() && !AIOSToolRegistry::is_clarify_phase_tool(name)) {
			continue;
		}
		if (!AIOSToolRegistry::is_allowed_for_role(name, mode)) {
			continue;
		}
		Dictionary tool;
		tool["name"] = name;
		tool["description"] = entry["summary"];
		tool["input_schema"] = entry.has("input_schema") ? entry["input_schema"] : Variant(Dictionary());
		tools.push_back(tool);
	}
	llm->set_tools(tools);
}

String AIOSPipeline::_format_brief(const Dictionary &p_brief) {
	String out;
	out += "Title: " + String(p_brief.get("title", "")) + "\n";
	out += "Dimensions: " + String(p_brief.get("dimensions", "")).to_upper() + "\n";
	if (!String(p_brief.get("genre", "")).is_empty()) {
		out += "Genre: " + String(p_brief["genre"]) + "\n";
	}
	out += "Summary: " + String(p_brief.get("summary", "")) + "\n";
	out += "Core loop: " + String(p_brief.get("core_loop", "")) + "\n";
	if (!String(p_brief.get("controls", "")).is_empty()) {
		out += "Controls: " + String(p_brief["controls"]) + "\n";
	}
	if (!String(p_brief.get("win_lose", "")).is_empty()) {
		out += "Win/lose: " + String(p_brief["win_lose"]) + "\n";
	}
	out += "Scope: " + String(p_brief.get("scope", "")) + "\n";
	if (!String(p_brief.get("art_direction", "")).is_empty()) {
		out += "Art: " + String(p_brief["art_direction"]) + "\n";
	}
	if (!String(p_brief.get("technical_notes", "")).is_empty()) {
		out += "Technical notes: " + String(p_brief["technical_notes"]) + "\n";
	}
	return out;
}

String AIOSPipeline::_format_plan(const Dictionary &p_plan) {
	String out = String(p_plan.get("summary", ""));
	Array steps = p_plan.get("steps", Array());
	if (steps.size() > 0) {
		out += "\n\nSteps:\n";
		for (int i = 0; i < steps.size(); i++) {
			out += String::num_int64(i + 1) + ". " + String(steps[i]) + "\n";
		}
	}
	return out;
}

Dictionary AIOSPipeline::_minimal_brief_from_goal(const String &p_goal) {
	Dictionary brief;
	brief["title"] = "Untitled prototype";
	brief["dimensions"] = "2d";
	brief["genre"] = "";
	brief["summary"] = p_goal;
	brief["core_loop"] = "Implement a playable slice matching the user's request as closely as possible.";
	brief["controls"] = "Use Godot's default ui_* actions unless the request specifies otherwise.";
	brief["win_lose"] = "Define a simple win or fail condition if the genre implies one; otherwise sandbox.";
	brief["scope"] = "Ship a small playable MVP in this session: one main scene, player control, and one core interaction. "
					 "Prefer placeholders over missing art.";
	brief["art_direction"] = "Primitive shapes and clear colours; no external asset dependency.";
	brief["technical_notes"] =
			"Defaulting to 2D because the interview was skipped. Switch to 3D node types only if the "
			"original request clearly requires 3D.";
	const String lower = p_goal.to_lower();
	const bool explicit_2d = lower.contains("2d") || lower.contains("top-down") || lower.contains("top down") ||
			lower.contains("side-scroll") || lower.contains("side scroll") || lower.contains("platformer");
	const bool explicit_3d = lower.contains("3d") || lower.contains("first person") ||
			lower.contains("first-person") || lower.contains("third person") || lower.contains("third-person");
	// Bare "fps" often means a shooter genre, not necessarily 3D — only treat it
	// as 3D when the goal does not also ask for a 2D presentation.
	const bool fps_like = lower.contains("fps") || lower.contains("first person shooter") ||
			lower.contains("first-person shooter");
	if ((explicit_3d || (fps_like && !explicit_2d)) && !explicit_2d) {
		brief["dimensions"] = "3d";
		brief["technical_notes"] =
				"Using 3D node types (Node3D / CharacterBody3D / Camera3D) because the request implies a 3D game.";
	} else if (fps_like && explicit_2d) {
		brief["technical_notes"] =
				"Treating this as a 2D shooter (Node2D / CharacterBody2D) because the request asks for 2D.";
	}
	return brief;
}

String AIOSPipeline::_memory_block() const {
	return memory.is_valid() ? memory->format_for_prompt() : String();
}

void AIOSPipeline::_enter_build_phase(const Dictionary &p_brief, bool p_skipped_interview) {
	committed_brief = p_brief;
	brief_ready = true;
	clarifying = false;
	awaiting_user = false;
	ask_user_tool_use_id = String();
	pending_questions.clear();

	const bool git_ok = git.is_valid() && git->is_available();
	if (llm != nullptr) {
		llm->set_system_prompt(build_system_prompt(mode, git_ok, false, _memory_block()));
	}
	_apply_tools_for_phase();

	emit_signal("brief_committed", committed_brief);
	emit_signal("pipeline_log", "success",
			p_skipped_interview
					? "Interview skipped. Building from a minimal brief derived from your goal."
					: "Brief locked. Build tools unlocked — implementing now.");
	emit_signal("agent_message", String("**Game brief**\n\n") + _format_brief(committed_brief));
}

String AIOSPipeline::_build_session_message(const String &p_text, const String &p_previous_mode,
		bool p_mode_changed) const {
	String message;

	if (p_mode_changed) {
		message += "[System: Role switched from " + p_previous_mode + " to " + mode + ". ";
		message += "Continue the same task using the conversation above. ";
		if (mode.to_lower() == "coder") {
			message +=
					"You now have full mutating build tools. Implement the committed brief and any approved "
					"plan — do not restart the interview or re-propose from scratch unless something is "
					"genuinely missing.";
		} else if (mode.to_lower() == "architect") {
			message += "Focus on planning and inspection; mutating tools are not available in this role.";
		}
		message += "]\n\n";
	}

	if (!committed_brief.is_empty()) {
		message += "## Committed brief (still in effect)\n\n" + _format_brief(committed_brief) + "\n";
	}
	if (!pending_plan.is_empty()) {
		message += "## Plan from the prior role\n\n" + _format_plan(pending_plan) + "\n";
	}

	message += p_text;
	return message;
}

void AIOSPipeline::_prepare_run_state(const String &p_goal, const String &p_mode, bool p_fresh_session) {
	goal = p_goal;
	mode = p_mode;
	repair_attempts = 0;
	steps_executed = 0;
	pending_results.clear();
	deferred_calls.clear();
	awaiting_playtest = false;
	awaiting_user = false;
	ask_user_tool_use_id = String();
	pending_questions.clear();
	propose_plan_tool_use_id = String();
	batch_saved_scene = false;
	batch_had_scene_edits = false;

	if (p_fresh_session) {
		committed_brief.clear();
		brief_ready = false;
		clarifying = _mode_requires_brief(mode);
		awaiting_plan_approval = false;
		plan_approved = false;
		pending_plan.clear();
	} else {
		const bool has_prior_conversation = llm != nullptr && llm->get_history().size() > 0;
		if (!committed_brief.is_empty()) {
			brief_ready = true;
			clarifying = false;
		} else if (has_prior_conversation) {
			// The prior role already interviewed or planned in chat — do not restart.
			clarifying = false;
		} else {
			clarifying = _mode_requires_brief(mode) && !brief_ready;
		}

		// A handoff into coder mode means the human wants implementation now.
		if (mode.to_lower() == "coder" && !pending_plan.is_empty()) {
			awaiting_plan_approval = false;
			plan_approved = true;
		}
	}

	if (registry.is_valid()) {
		registry->set_active_role(mode);
	}
	if (memory.is_valid()) {
		memory->load();
	}
}

bool AIOSPipeline::has_session_context() const {
	if (llm != nullptr && llm->get_history().size() > 0) {
		return true;
	}
	if (!committed_brief.is_empty()) {
		return true;
	}
	if (!pending_plan.is_empty()) {
		return true;
	}
	if (!goal.is_empty() && brief_ready) {
		return true;
	}
	return false;
}

void AIOSPipeline::reset_session() {
	if (is_running()) {
		stop();
	}

	goal.clear();
	mode = "architect";
	committed_brief.clear();
	brief_ready = false;
	clarifying = false;
	pending_plan.clear();
	awaiting_plan_approval = false;
	plan_approved = false;
	propose_plan_tool_use_id = String();
	pending_results.clear();
	deferred_calls.clear();
	awaiting_playtest = false;
	awaiting_user = false;
	ask_user_tool_use_id = String();
	pending_questions.clear();
	batch_saved_scene = false;
	batch_had_scene_edits = false;
	stage = STAGE_IDLE;

	if (llm != nullptr) {
		llm->cancel();
		llm->reset_conversation();
	}
	if (registry.is_valid()) {
		registry->set_active_role(String());
	}
}

Dictionary AIOSPipeline::export_session_state() const {
	Dictionary d;
	d["goal"] = goal;
	d["mode"] = mode;
	d["brief_ready"] = brief_ready;
	d["committed_brief"] = committed_brief;
	d["pending_plan"] = pending_plan;
	d["awaiting_plan_approval"] = awaiting_plan_approval;
	d["plan_approved"] = plan_approved;
	d["clarifying"] = clarifying;
	return d;
}

void AIOSPipeline::import_session_state(const Dictionary &p_state) {
	if (p_state.is_empty()) {
		return;
	}

	goal = String(p_state.get("goal", ""));
	mode = String(p_state.get("mode", "architect"));
	brief_ready = (bool)p_state.get("brief_ready", false);
	committed_brief = p_state.get("committed_brief", Dictionary());
	pending_plan = p_state.get("pending_plan", Dictionary());
	awaiting_plan_approval = (bool)p_state.get("awaiting_plan_approval", false);
	plan_approved = (bool)p_state.get("plan_approved", false);
	clarifying = (bool)p_state.get("clarifying", false);

	if (!committed_brief.is_empty()) {
		brief_ready = true;
		clarifying = false;
	}

	if (registry.is_valid()) {
		registry->set_active_role(mode);
	}
}

void AIOSPipeline::sync_session_tools() {
	if (llm != nullptr && llm->is_configured()) {
		_apply_tools_for_phase();
	}
}

/* -------------------------------------------------------------------------- */
/*  System prompt                                                              */
/* -------------------------------------------------------------------------- */

String AIOSPipeline::build_system_prompt(const String &p_mode, bool p_git_available, bool p_clarifying,
		const String &p_memory_block) {
	String prompt =
			"You are an autonomous game developer working inside the Godot 4 editor through the AI Agent OS "
			"plugin. You have tools that read and modify a live Godot project. The human can see everything you "
			"do in a dock and can stop you at any time.\n\n";

	if (p_clarifying) {
		prompt +=
				"## Clarification phase (you are here now)\n\n"
				"The human's goal is not yet a build brief. Your job is to interview them until you can write "
				"a precise brief for a 2D or 3D game, then lock it with commit_brief. Build tools are withheld "
				"until that happens — you cannot create nodes or scripts yet.\n\n"
				"Rules for this phase:\n"
				"1. If the goal is vague (e.g. \"make me an FPS\", \"make a platformer\", \"build a game\"), "
				"call ask_user with focused questions. Do NOT invent a generic game and start building.\n"
				"2. Always resolve 2D vs 3D. Never guess. Also cover genre specifics, controls, win/lose, "
				"MVP scope, and a buildable art direction.\n"
				"3. Ask 2–5 questions per turn. Prefer multiple-choice options plus freeform. Follow up if "
				"answers are still thin.\n"
				"4. When — and only when — you have enough to build well, call commit_brief with dimensions "
				"set to \"2d\" or \"3d\". Summarise the brief in plain language for the human as you do.\n"
				"5. If the human already gave a complete brief (dimensions, loop, controls, scope), call "
				"commit_brief immediately without asking filler questions.\n"
				"6. You may call get_world_model to see what already exists in the project before deciding "
				"what to ask.\n\n";
	}

	prompt +=
			"## How your work is executed\n\n"
			"Every tool call you make goes through a fixed pipeline: it is validated statically, then executed, "
			"then the scene is re-checked, and the result comes back to you. A call that fails validation is "
			"NOT executed - you get the findings and a chance to correct it, and the project is untouched.\n\n";

	if (p_git_available) {
		prompt +=
				"Before each batch of changes the pipeline takes a git snapshot. If your changes break the "
				"project, it resets to that snapshot and tells you. Rollback is real: your edits genuinely "
				"disappear. Retrying the same thing after a rollback wastes a cycle.\n\n";
	} else {
		prompt +=
				"This project is NOT a git repository, so there is no rollback. Every change you make is "
				"permanent. Be correspondingly careful: prefer dry_run first, and prefer small reversible "
				"steps over large ones.\n\n";
	}

	prompt +=
			"## Rules that will save you a cycle\n\n"
			"Call get_world_model before your first edit and after anything unexpected. Node paths are relative "
			"to the scene root, which is \".\". Never guess a path.\n\n"
			"Choose 2D or 3D node families deliberately from the committed brief: Node2D/CharacterBody2D/"
			"Camera2D for 2D, Node3D/CharacterBody3D/Camera3D for 3D. Do not mix them without a reason.\n\n"
			"Scene edits live in the editor's memory until you call save_scene. Call it once at the end of a "
			"coherent batch of edits - not after every node, and never forget it, because an unsaved scene is "
			"not captured by a snapshot.\n\n"
			"When a tool returns an error, read it. The code says what class of problem it is, the message says "
			"what to do, and details usually contains the fix. Correct the call rather than trying a different "
			"tool.\n\n"
			"Use dry_run when you are unsure. It runs every check and reports what would happen, at no cost to "
			"the project.\n\n"
			"Finish the whole task, not the easy part of it. Only report completion when it is actually done. "
			"If something is genuinely blocked, do the rest and say plainly what is missing and why.\n\n"
			"Tell the human what you are doing as you go, in plain sentences. They are watching a dock, not "
			"reading a log file.\n\n";

	if (!p_memory_block.is_empty()) {
		prompt += p_memory_block + String("\n");
	}

	// Mode is the one thing that changes what the agent is *for*. Keeping the
	// difference small and concrete beats four divergent prompts that drift.
	const String m = p_mode.to_lower();
	prompt += "## Your role right now: " + m + "\n\n";
	if (m == "architect") {
		prompt +=
				"Plan before you build. Inspect the project, propose a concrete structure with propose_plan, "
				"and explain the trade-offs in a sentence or two before creating anything. You cannot call "
				"mutating tools in architect mode — hand off implementation to coder mode or wait for approval.";
	} else if (m == "coder") {
		prompt +=
				"Implement what was asked, completely. Write GDScript that reads like the rest of the "
				"project. Attach scripts to the right nodes, verify the base class matches, and save the "
				"scene when the batch is done. Do not add abstractions, error handling for impossible "
				"states, or features nobody asked for.";
	} else if (m == "debugger") {
		prompt +=
				"Find the actual cause before changing anything. Run the playtest, read the diagnostics, and "
				"open the file the error points at. Fix the specific cause, then run the playtest again to "
				"confirm. Do not fix things that are not broken, and do not declare it fixed without a clean "
				"run to show for it.";
	} else if (m == "playtester") {
		prompt +=
				"Exercise the game and report what actually happens. Run the playtest, read every diagnostic, "
				"and describe both what worked and what did not. Report findings faithfully - a run that "
				"failed is a useful result, not something to work around.";
	} else {
		prompt += "Work on the task as asked, using the tools available.";
	}

	return prompt;
}

/* -------------------------------------------------------------------------- */
/*  Run lifecycle                                                              */
/* -------------------------------------------------------------------------- */

Dictionary AIOSPipeline::start(const String &p_goal, const String &p_mode) {
	if (stage != STAGE_IDLE && stage != STAGE_ERROR) {
		return AIOSJson::error("already_running",
				"A run is already in progress (" + get_stage_name() + "). Stop it before starting another.");
	}
	if (llm == nullptr || !llm->is_configured()) {
		return AIOSJson::error("no_model",
				"No model is configured. Open the AI Agent dock's Settings panel and add an API key.");
	}

	_prepare_run_state(p_goal, p_mode, true);

	const bool git_ok = git.is_valid() && git->is_available();
	if (!git_ok) {
		emit_signal("pipeline_log", "warn",
				"This project is not a git repository, so the pipeline cannot roll back a bad step. "
				"Run `git init` and make an initial commit to enable that safety net.");
	} else {
		// The baseline for the whole run. If everything goes wrong, this is
		// where the human can get back to.
		Dictionary snap = git->create_snapshot("run start: " + p_goal.substr(0, 60));
		if ((bool)snap["ok"]) {
			last_good_snapshot = String(Dictionary(snap["result"])["sha"]);
		} else {
			emit_signal("pipeline_log", "warn",
					"Could not take a starting snapshot: " + String(Dictionary(snap["error"])["message"]));
		}
	}

	llm->reset_conversation();
	llm->set_system_prompt(build_system_prompt(mode, git_ok, clarifying, _memory_block()));
	_apply_tools_for_phase();

	if (clarifying) {
		_set_stage(STAGE_CLARIFYING, "interviewing before build — answer in the box below");
		emit_signal("pipeline_log", "info",
				"Clarifying what to build first. Answer the agent's questions, or press Skip & Build.");
		String kickoff = p_goal;
		kickoff += "\n\n[System: You are in the clarification phase. If this goal needs more detail, call "
				   "ask_user. If it is already a complete brief including 2D vs 3D, call commit_brief "
				   "immediately. Do not invent a generic game.]";
		llm->send_user_message(kickoff);
	} else {
		_set_stage(STAGE_PLANNING, "sending the goal to " + llm->describe_target());
		llm->send_user_message(p_goal);
	}

	Dictionary result;
	result["goal"] = goal;
	result["mode"] = mode;
	result["model"] = llm->describe_target();
	result["git"] = git_ok;
	result["clarifying"] = clarifying;
	result["continued"] = false;
	return AIOSJson::ok(result);
}

Dictionary AIOSPipeline::continue_session(const String &p_text, const String &p_mode) {
	if (stage != STAGE_IDLE && stage != STAGE_ERROR) {
		return AIOSJson::error("already_running",
				"A run is already in progress (" + get_stage_name() + "). Stop it before continuing.");
	}
	if (llm == nullptr || !llm->is_configured()) {
		return AIOSJson::error("no_model",
				"No model is configured. Open the AI Agent dock's Settings panel and add an API key.");
	}
	if (llm->is_busy()) {
		return AIOSJson::error("busy", "A model request is already in flight.");
	}
	if (!has_session_context()) {
		return start(p_text, p_mode);
	}

	const String previous_mode = mode;
	const bool mode_changed = previous_mode.to_lower() != p_mode.to_lower();
	const String session_goal = goal.is_empty() ? p_text : goal;

	_prepare_run_state(session_goal, p_mode, false);

	const bool git_ok = git.is_valid() && git->is_available();
	if (git_ok) {
		Dictionary snap = git->create_snapshot("session continue: " + p_text.substr(0, 60));
		if ((bool)snap["ok"]) {
			last_good_snapshot = String(Dictionary(snap["result"])["sha"]);
		}
	}

	llm->set_system_prompt(build_system_prompt(mode, git_ok, clarifying, _memory_block()));
	_apply_tools_for_phase();

	const String message = _build_session_message(p_text, previous_mode, mode_changed);

	if (clarifying) {
		_set_stage(STAGE_CLARIFYING, "continuing clarification in " + mode);
	} else {
		_set_stage(STAGE_PLANNING, "continuing in " + mode + " — " + llm->describe_target());
	}

	if (mode_changed) {
		emit_signal("pipeline_log", "info",
				"Continuing the session in " + mode + " mode with prior chat history preserved.");
	}

	llm->send_user_message(message);

	Dictionary result;
	result["goal"] = goal;
	result["mode"] = mode;
	result["previous_mode"] = previous_mode;
	result["model"] = llm->describe_target();
	result["git"] = git_ok;
	result["clarifying"] = clarifying;
	result["continued"] = true;
	result["mode_changed"] = mode_changed;
	return AIOSJson::ok(result);
}

Dictionary AIOSPipeline::continue_with_user_answer(const String &p_answer) {
	if (!awaiting_user) {
		return AIOSJson::error("not_awaiting_user",
				"The agent is not waiting for an answer right now.");
	}
	if (llm == nullptr) {
		return AIOSJson::error("no_model", "The language-model client is unavailable.");
	}
	if (llm->is_busy()) {
		return AIOSJson::error("busy", "A model request is already in flight.");
	}

	const String answer = p_answer.strip_edges();
	if (answer.is_empty()) {
		return AIOSJson::error("empty_answer", "Type an answer before sending.");
	}

	awaiting_user = false;
	_set_stage(STAGE_CLARIFYING, "incorporating your answers");

	if (!ask_user_tool_use_id.is_empty()) {
		Dictionary payload;
		payload["answered"] = true;
		payload["raw_answer"] = answer;
		payload["questions"] = pending_questions.get("questions", Array());
		payload["intro"] = pending_questions.get("intro", "");

		Dictionary block;
		block["type"] = "tool_result";
		block["tool_use_id"] = ask_user_tool_use_id;
		block["content"] = JSON::stringify(payload);

		Array results;
		results.push_back(block);
		// Any calls that were queued behind ask_user are abandoned: the model
		// should react to the answers on the next turn, not continue a plan
		// drafted before it heard the human.
		deferred_calls.clear();
		pending_results.clear();
		ask_user_tool_use_id = String();
		pending_questions.clear();

		llm->send_tool_results(results);
	} else {
		// Freeform: the model asked in prose without ask_user.
		llm->send_user_message(answer);
	}

	Dictionary result;
	result["accepted"] = true;
	return AIOSJson::ok(result);
}

Dictionary AIOSPipeline::skip_clarification_and_build() {
	if (!is_clarifying() && !awaiting_user) {
		return AIOSJson::error("not_clarifying",
				"Nothing to skip — the agent is not in the clarification interview.");
	}
	if (llm == nullptr || !llm->is_configured()) {
		return AIOSJson::error("no_model",
				"No model is configured. Open the AI Agent dock's Settings panel and add an API key.");
	}
	if (llm->is_busy()) {
		llm->cancel();
	}

	// Drop any open ask_user tool_use by resetting the conversation. Leaving an
	// unanswered tool_use in history makes Anthropic reject the next request.
	const String original_goal = goal;
	Dictionary brief = _minimal_brief_from_goal(original_goal);
	_enter_build_phase(brief, true);

	pending_results.clear();
	deferred_calls.clear();
	awaiting_playtest = false;

	const bool git_ok = git.is_valid() && git->is_available();
	llm->reset_conversation();
	llm->set_system_prompt(build_system_prompt(mode, git_ok, false, _memory_block()));
	_apply_tools_for_phase();

	_set_stage(STAGE_PLANNING, "building from the skipped-interview brief");
	String message =
			"Original goal: " + original_goal + "\n\n"
			"The human skipped the clarification interview. Build the best playable MVP you can from this "
			"brief. Prefer asking nothing further unless a single critical choice is still impossible.\n\n";
	message += _format_brief(brief);
	llm->send_user_message(message);

	Dictionary result;
	result["skipped"] = true;
	result["brief"] = brief;
	return AIOSJson::ok(result);
}

Dictionary AIOSPipeline::approve_plan() {
	if (!awaiting_plan_approval) {
		return AIOSJson::error("no_plan_pending",
				"The agent has not proposed a plan that is waiting for approval.");
	}
	if (llm == nullptr) {
		return AIOSJson::error("no_model", "The language-model client is unavailable.");
	}
	if (llm->is_busy()) {
		return AIOSJson::error("busy", "A model request is already in flight.");
	}

	awaiting_plan_approval = false;
	plan_approved = true;

	if (!propose_plan_tool_use_id.is_empty()) {
		Dictionary payload;
		payload["approved"] = true;
		payload["plan"] = pending_plan;

		Dictionary block;
		block["type"] = "tool_result";
		block["tool_use_id"] = propose_plan_tool_use_id;
		block["content"] = JSON::stringify(payload);

		Array results;
		results.push_back(block);
		propose_plan_tool_use_id = String();
		llm->send_tool_results(results);
	} else {
		llm->send_user_message(
				"The human approved your plan. Proceed with implementation using mutating tools. "
				"Call get_world_model first if you have not inspected the scene recently.");
	}

	_set_stage(STAGE_PLANNING, "plan approved — implementing");
	emit_signal("pipeline_log", "success", "Plan approved. The agent may now mutate the project.");

	Dictionary result;
	result["approved"] = true;
	result["plan"] = pending_plan;
	return AIOSJson::ok(result);
}

void AIOSPipeline::stop() {
	if (stage == STAGE_IDLE) {
		return;
	}
	// Drop ownership before killing the playtest: AIOSPlaytest::stop() emits
	// playtest_finished synchronously, and if awaiting_playtest were still
	// true the handler would restart a model turn after the human pressed Stop.
	awaiting_playtest = false;
	awaiting_user = false;
	ask_user_tool_use_id = String();
	pending_questions.clear();
	pending_results.clear();
	deferred_calls.clear();
	propose_plan_tool_use_id = String();

	// Preserve brief/plan artifacts so a mode switch or follow-up prompt can
	// continue the same session without re-interviewing.
	if (committed_brief.is_empty()) {
		brief_ready = false;
		clarifying = false;
	} else {
		brief_ready = true;
		clarifying = false;
	}

	if (registry.is_valid()) {
		registry->set_active_role(String());
	}

	if (llm != nullptr) {
		llm->cancel();
	}
	if (playtest.is_valid() && playtest->is_running()) {
		playtest->stop();
	}
	_set_stage(STAGE_IDLE, "stopped by user");
	emit_signal("pipeline_log", "warn",
			"Run stopped. The project was left exactly as it is - nothing was rolled back. "
			"Use the Rollback button if you want the last step undone.");
}

void AIOSPipeline::poll(double p_delta) {
	if (playtest.is_valid()) {
		playtest->poll(p_delta);
	}
}

void AIOSPipeline::_abort(const String &p_code, const String &p_message) {
	_set_stage(STAGE_ERROR, p_code);
	emit_signal("pipeline_log", "error", p_message);

	Dictionary summary;
	summary["ok"] = false;
	summary["code"] = p_code;
	summary["message"] = p_message;
	summary["steps_executed"] = steps_executed;
	// Any executed step may have written to disk, and a rollback certainly did,
	// so the editor is told to rescan whenever a run got as far as step one.
	summary["filesystem_changed"] = steps_executed > 0;
	if (memory.is_valid()) {
		memory->add_failure("pipeline", p_message);
		memory->record_run_finished(summary);
	}
	emit_signal("run_finished", summary);
	stage = STAGE_IDLE;
	clarifying = false;
	awaiting_user = false;
	awaiting_plan_approval = false;
	plan_approved = false;
	if (registry.is_valid()) {
		registry->set_active_role(String());
	}
}

/* -------------------------------------------------------------------------- */
/*  Model responses                                                            */
/* -------------------------------------------------------------------------- */

void AIOSPipeline::_on_model_response(const Dictionary &p_response) {
	if (stage == STAGE_IDLE) {
		return; // A late response from a cancelled run.
	}

	const String thinking = String(p_response.get("thinking", ""));
	if (!thinking.is_empty()) {
		emit_signal("agent_thinking", thinking);
	}
	const String text = String(p_response.get("text", ""));
	if (!text.is_empty()) {
		emit_signal("agent_message", text);
	}

	Array calls = p_response.get("tool_calls", Array());
	if (calls.is_empty()) {
		if (is_clarifying()) {
			// The model asked in prose or summarised without tools. Wait for
			// the human rather than ending the run — that is how "make me an
			// FPS" becomes a real brief instead of silence.
			awaiting_user = true;
			ask_user_tool_use_id = String();
			pending_questions.clear();
			_set_stage(STAGE_CLARIFYING, "waiting for your answers in the box below");
			Dictionary payload;
			payload["intro"] = text;
			payload["questions"] = Array();
			payload["freeform"] = true;
			emit_signal("user_questions_requested", payload);
			emit_signal("pipeline_log", "info",
					"Answer in the prompt box, or press Skip & Build to proceed with a minimal brief.");
			return;
		}

		// No tool calls means the model considers the task finished.
		_set_stage(STAGE_IDLE, "done");

		Dictionary summary;
		summary["ok"] = true;
		summary["steps_executed"] = steps_executed;
		summary["final_message"] = text;
		summary["filesystem_changed"] = steps_executed > 0;
		summary["message"] = steps_executed > 0
				? "Run finished after " + String::num_int64(steps_executed) + " step(s)."
				: String("Run finished without changing anything.");
		if (memory.is_valid()) {
			memory->record_run_finished(summary);
		}
		emit_signal("run_finished", summary);
		return;
	}

	_handle_tool_calls(calls);
}

void AIOSPipeline::_on_model_failed(const Dictionary &p_error) {
	if (stage == STAGE_IDLE) {
		return;
	}
	_abort(String(p_error.get("code", "model_error")), String(p_error.get("message", "The model request failed.")));
}

/* -------------------------------------------------------------------------- */
/*  Validate -> Execute                                                        */
/* -------------------------------------------------------------------------- */

void AIOSPipeline::_handle_tool_calls(const Array &p_calls) {
	pending_results.clear();
	deferred_calls.clear();
	batch_saved_scene = false;
	batch_had_scene_edits = false;

	// One snapshot per batch, not per call. A model routinely emits several
	// calls that only make sense together (create a node, then attach its
	// script); rolling back to the middle of that would leave a half-built
	// scene that is worse than either end state.
	bool may_mutate = false;
	for (int i = 0; i < p_calls.size(); i++) {
		Dictionary call = p_calls[i];
		if (AIOSToolRegistry::is_mutating(String(call.get("name", "")))) {
			may_mutate = true;
			break;
		}
	}
	if (may_mutate && git.is_valid() && git->is_available()) {
		Dictionary snap = git->create_snapshot("before step " + String::num_int64(steps_executed + 1));
		if ((bool)snap["ok"]) {
			step_snapshot = String(Dictionary(snap["result"])["sha"]);
		} else {
			step_snapshot = String();
			emit_signal("pipeline_log", "warn",
					"No snapshot for this step: " + String(Dictionary(snap["error"])["message"]) +
							" Rollback will not be available if it goes wrong.");
		}
	}

	for (int i = 0; i < p_calls.size(); i++) {
		Dictionary call = p_calls[i];
		const String tool = String(call.get("name", ""));

		if (require_plan_approval && !plan_approved && AIOSToolRegistry::is_mutating(tool) && tool != "save_scene") {
			_set_stage(STAGE_AWAITING_APPROVAL, "mutating tools blocked until plan approval");
			Dictionary details;
			details["tool"] = tool;
			const String id = String(call.get("id", ""));
			Dictionary envelope = AIOSJson::error("plan_approval_required",
					"Call propose_plan and wait for the human to approve before mutating the project.",
					details);
			emit_signal("tool_completed", tool, false, envelope);
			_push_result(id, envelope["error"], true);
			continue;
		}

		if (awaiting_playtest || awaiting_user || awaiting_plan_approval) {
			// Everything after a playtest or an ask_user call waits for the
			// human/runtime to finish — continuing would change the world under
			// an answer that has not arrived yet.
			deferred_calls.push_back(call);
			continue;
		}
		_execute_call(call);
	}

	if (!awaiting_playtest && !awaiting_user && !awaiting_plan_approval) {
		_finish_turn();
	}
}

void AIOSPipeline::_execute_call(const Dictionary &p_call) {
	const String tool = String(p_call.get("name", ""));
	const String id = String(p_call.get("id", ""));
	Dictionary params = p_call.get("input", Dictionary());

	emit_signal("tool_invoked", tool, params);

	// Soft gate: even if a mutating tool somehow appears in the clarifying
	// tool list, refuse it until the brief is locked.
	if (is_clarifying() && !AIOSToolRegistry::is_clarify_phase_tool(tool)) {
		_set_stage(STAGE_CLARIFYING, "refused " + tool + " until the brief is committed");
		Dictionary details;
		details["tool"] = tool;
		Dictionary envelope = AIOSJson::error("brief_required",
				"Build tools are locked until you finish interviewing the human and call commit_brief. "
				"Use ask_user for remaining questions, or commit_brief if you already have enough detail.",
				details);
		emit_signal("tool_completed", tool, false, envelope);
		_push_result(id, envelope["error"], true);
		return;
	}

	// Role gate for built-in agent modes.
	if (!AIOSToolRegistry::is_allowed_for_role(tool, mode)) {
		_set_stage(STAGE_REPAIRING, tool + String(" forbidden for ") + mode + String(" mode"));
		Dictionary details;
		details["role"] = mode;
		details["tool"] = tool;
		Dictionary envelope = AIOSJson::error("role_forbidden",
				"The '" + mode + "' role cannot call '" + tool + "'. Switch mode or use a permitted tool.",
				details);
		emit_signal("tool_completed", tool, false, envelope);
		_push_result(id, envelope["error"], true);
		return;
	}

	// --- Validate ----------------------------------------------------------
	_set_stage(is_clarifying() ? STAGE_CLARIFYING : STAGE_VALIDATING, tool);
	Dictionary validation = AIOSValidator::validate_planned_call(tool, params);
	Dictionary validation_result = validation["result"];
	Array findings = validation_result["findings"];

	if (!(bool)validation_result["valid"]) {
		// Refused, not executed. The model gets the findings and revises.
		_set_stage(STAGE_REPAIRING, tool + String(" failed validation"));
		emit_signal("tool_completed", tool, false, validation);

		Dictionary payload;
		payload["validation_failed"] = true;
		payload["findings"] = findings;
		_push_result(id, payload, true);

		// Store the human-readable form as the tool_result content — that text
		// is what the model actually reads.
		Dictionary last = pending_results[pending_results.size() - 1];
		last["content"] = build_validation_feedback(tool, findings);
		pending_results[pending_results.size() - 1] = last;
		return;
	}

	// Warnings do not block, but the model should still see them.
	for (int i = 0; i < findings.size(); i++) {
		Dictionary f = findings[i];
		emit_signal("pipeline_log", "warn", tool + String(": ") + String(f["message"]));
	}

	// --- ask_user is special: pause for the dock ---------------------------
	if (tool == "ask_user") {
		Dictionary envelope = registry->call_tool(tool, params);
		const bool ok = envelope.has("ok") && (bool)envelope["ok"];
		emit_signal("tool_completed", tool, ok, envelope);
		if (!ok) {
			_push_result(id, envelope["error"], true);
			return;
		}

		awaiting_user = true;
		ask_user_tool_use_id = id;
		pending_questions = params;
		_set_stage(STAGE_CLARIFYING, "waiting for your answers");
		emit_signal("user_questions_requested", params);

		// Surface the questions in chat even if the dock listener is slow.
		String visible = AIOSJson::get_string(params, "intro", "");
		if (!visible.is_empty()) {
			visible += "\n\n";
		}
		Array questions = AIOSJson::get_array(params, "questions");
		for (int i = 0; i < questions.size(); i++) {
			Dictionary q = questions[i];
			visible += String::num_int64(i + 1) + ". " + String(q.get("prompt", "")) + "\n";
			Array options = AIOSJson::get_array(q, "options");
			for (int j = 0; j < options.size(); j++) {
				visible += "   - " + String(options[j]) + "\n";
			}
		}
		if (!visible.is_empty()) {
			emit_signal("agent_message", visible);
		}
		emit_signal("pipeline_log", "info",
				"Your turn — answer in the prompt box, or press Skip & Build.");
		return;
	}

	// --- propose_plan pauses for human approval ------------------------------
	if (tool == "propose_plan") {
		Dictionary envelope = registry->call_tool(tool, params);
		const bool ok = envelope.has("ok") && (bool)envelope["ok"];
		emit_signal("tool_completed", tool, ok, envelope);
		if (!ok) {
			_push_result(id, envelope["error"], true);
			return;
		}

		Dictionary result = envelope["result"];
		pending_plan = result;
		propose_plan_tool_use_id = id;
		awaiting_plan_approval = true;
		plan_approved = false;

		String diff_preview;
		if (git.is_valid() && git->is_available()) {
			Dictionary diff = git->diff_working_tree(120);
			if ((bool)diff["ok"]) {
				Dictionary diff_result = diff["result"];
				const String stat = String(diff_result.get("stat", ""));
				const String diff_text = String(diff_result.get("diff", ""));
				diff_preview = stat;
				if (!diff_text.is_empty()) {
					diff_preview += "\n\n" + diff_text;
				}
				if (diff_preview.is_empty()) {
					diff_preview = "(no uncommitted changes on disk yet)";
				}
			}
		} else {
			diff_preview = "(git unavailable — diff preview skipped)";
		}

		_set_stage(STAGE_AWAITING_APPROVAL, "waiting for plan approval");
		emit_signal("plan_proposed", result, diff_preview);

		String visible = "**Proposed plan**\n\n" + String(result.get("summary", "")) + "\n\nSteps:\n";
		Array steps = result.get("steps", Array());
		for (int i = 0; i < steps.size(); i++) {
			visible += String::num_int64(i + 1) + ". " + String(steps[i]) + "\n";
		}
		visible += "\nPress **Approve Plan** to let the agent proceed, or reply with changes.";
		emit_signal("agent_message", visible);
		emit_signal("pipeline_log", "info", "Plan submitted. Approve it in the dock to unlock mutating tools.");
		_push_result(id, result, false);
		return;
	}

	// --- commit_brief unlocks the build phase ------------------------------
	if (tool == "commit_brief") {
		Dictionary envelope = registry->call_tool(tool, params);
		const bool ok = envelope.has("ok") && (bool)envelope["ok"];
		emit_signal("tool_completed", tool, ok, envelope);
		if (!ok) {
			_push_result(id, envelope["error"], true);
			return;
		}

		Dictionary result = envelope["result"];
		Dictionary brief = result.get("brief", Dictionary());
		_enter_build_phase(brief, false);
		_push_result(id, result, false);
		return;
	}

	// --- Playtest is special: it is asynchronous ---------------------------
	if (tool == "run_playtest") {
		_set_stage(STAGE_PLAYTESTING, "launching " + String(params.get("scene", "the main scene")));
		Dictionary launched = playtest->start(params);
		if (!(bool)launched["ok"]) {
			emit_signal("tool_completed", tool, false, launched);
			_push_result(id, launched["error"], true);
			return;
		}
		awaiting_playtest = true;
		playtest_tool_use_id = id;
		emit_signal("tool_completed", tool, true, launched);
		return;
	}

	// --- Execute -----------------------------------------------------------
	_set_stage(STAGE_EXECUTING, tool);
	Dictionary envelope = registry->call_tool(tool, params);
	const bool ok = envelope.has("ok") && (bool)envelope["ok"];
	emit_signal("tool_completed", tool, ok, envelope);

	if (ok) {
		steps_executed++;
		if (tool == "save_scene") {
			batch_saved_scene = true;
		}
		if (AIOSToolRegistry::is_scene_edit(tool)) {
			batch_had_scene_edits = true;
		}
		_push_result(id, envelope["result"], false);
	} else {
		_push_result(id, envelope["error"], true);
	}
}

void AIOSPipeline::_push_result(const String &p_id, const Dictionary &p_payload, bool p_is_error) {
	Dictionary block;
	block["type"] = "tool_result";
	block["tool_use_id"] = p_id;

	// A screenshot has to reach the model as an actual image block, not as a
	// base64 string buried in JSON — a model handed 400 KB of base64 text will
	// dutifully try to read it as text and learn nothing.
	if (!p_is_error && p_payload.has("image_base64")) {
		Dictionary described = p_payload.duplicate();
		const String base64 = described["image_base64"];
		const String media_type = String(described.get("media_type", "image/png"));
		// Strip the payload out of the JSON summary so it is not sent twice.
		described.erase("image_base64");

		Array content;
		Dictionary text;
		text["type"] = "text";
		text["text"] = JSON::stringify(described);
		content.push_back(text);
		content.push_back(AIOSProvider::image_block(base64, media_type));

		block["content"] = content;
		pending_results.push_back(block);
		return;
	}

	block["content"] = JSON::stringify(p_payload);
	if (p_is_error) {
		block["is_error"] = true;
	}
	pending_results.push_back(block);
}

/* -------------------------------------------------------------------------- */
/*  Observe -> Repair -> Snapshot -> Continue                                  */
/* -------------------------------------------------------------------------- */

void AIOSPipeline::_finish_turn() {
	// Clarifying turns do not mutate the scene; skip the expensive sweep and
	// keep the stage label honest for the dock.
	if (is_clarifying() || (!brief_ready && clarifying)) {
		if (pending_results.is_empty()) {
			_set_stage(STAGE_CLARIFYING, "waiting for the next interview step");
			return;
		}
		_set_stage(STAGE_CLARIFYING, "sending your answers back to the model");
		llm->send_tool_results(pending_results);
		pending_results.clear();
		return;
	}

	// --- post-execution scene sweep ----------------------------------------
	// Individually valid edits can still combine into a broken scene: a delete
	// that orphans a NodePath another step set. This is the check that catches
	// that class of damage, and it is why validation runs twice.
	_set_stage(STAGE_VALIDATING, "checking the scene after the changes");
	Dictionary sweep = AIOSValidator::validate_scene();
	Dictionary sweep_result = sweep["result"];
	Array sweep_findings = sweep_result["findings"];

	if (!(bool)sweep_result["valid"]) {
		repair_attempts++;
		_set_stage(STAGE_REPAIRING,
				"scene broken (attempt " + String::num_int64(repair_attempts) + " of " +
						String::num_int64(max_repair_attempts) + ")");

		for (int i = 0; i < sweep_findings.size(); i++) {
			Dictionary f = sweep_findings[i];
			if (String(f["severity"]) == "error") {
				emit_signal("pipeline_log", "error", String(f["message"]));
			}
		}

		const bool budget_spent = repair_attempts >= max_repair_attempts;
		if (budget_spent) {
			// Always stop when the budget is spent. auto_rollback only chooses
			// whether to leave the wreckage on disk for inspection.
			if (auto_rollback && !step_snapshot.is_empty() && git.is_valid()) {
				Dictionary reset = git->reset_to_snapshot(step_snapshot);
				if ((bool)reset["ok"]) {
					emit_signal("pipeline_log", "warn",
							"Repair budget spent after " + String::num_int64(repair_attempts) +
									" attempts. Reset to snapshot " + step_snapshot.substr(0, 8) + ".");
					_abort("repair_budget_exhausted",
							"The agent could not produce a working scene in " +
									String::num_int64(max_repair_attempts) +
									" attempts, so the project was rolled back to where it was before this step. "
									"The findings are in the log above - this is a good moment to look at them yourself.");
					return;
				}
				emit_signal("pipeline_log", "error",
						"Rollback failed: " + String(Dictionary(reset["error"])["message"]));
			}
			_abort("repair_budget_exhausted",
					"The agent could not produce a working scene in " + String::num_int64(max_repair_attempts) +
							" attempts. Rollback is off (or failed), so the broken state was left on disk for "
							"inspection. last_good_snapshot=" +
							last_good_snapshot.substr(0, 8) + ".");
			return;
		}

		// Still inside budget: feed the findings back and let the model fix it.
		Dictionary payload;
		payload["scene_validation_failed"] = true;
		payload["findings"] = sweep_findings;

		Dictionary block;
		block["type"] = "text";
		block["text"] = build_validation_feedback("scene consistency check", sweep_findings);
		Array extra;
		extra.append_array(pending_results);
		extra.push_back(block);

		llm->send_tool_results(extra);
		pending_results.clear();
		return;
	}

	// --- success -----------------------------------------------------------
	repair_attempts = 0;

	// Auto-playtest only after the agent saved scene edits to disk. Running the
	// main scene before that tests stale files and spams the dock during planning.
	if (auto_playtest && batch_saved_scene && batch_had_scene_edits && playtest.is_valid() && !awaiting_playtest) {
		_set_stage(STAGE_PLAYTESTING, "auto smoke test after scene save");
		Dictionary play_params;
		play_params["timeout_sec"] = 20;
		Dictionary launched = playtest->start(play_params);
		if ((bool)launched["ok"]) {
			awaiting_playtest = true;
			playtest_tool_use_id = "__auto_playtest__";
			batch_saved_scene = false;
			batch_had_scene_edits = false;
			emit_signal("pipeline_log", "info", "Running automatic playtest after the scene was saved.");
			return;
		}
		emit_signal("pipeline_log", "warn",
				"Auto-playtest could not start: " + String(Dictionary(launched["error"])["message"]));
	}

	if (git.is_valid() && git->is_available() && steps_executed > 0) {
		Dictionary checkpoint = git->create_checkpoint("agent step " + String::num_int64(steps_executed));
		if ((bool)checkpoint["ok"]) {
			Dictionary cp = checkpoint["result"];
			if (AIOSJson::get_bool(cp, "created", false)) {
				last_good_snapshot = String(cp["sha"]);
				emit_signal("pipeline_log", "success",
						"Checkpoint " + String(cp["short_sha"]) + " - this step is now recoverable.");
			}
		}
	}

	if (pending_results.is_empty()) {
		_set_stage(STAGE_IDLE, "done");
		Dictionary summary;
		summary["ok"] = true;
		summary["steps_executed"] = steps_executed;
		summary["filesystem_changed"] = steps_executed > 0;
		summary["message"] = "Run finished after " + String::num_int64(steps_executed) + " step(s).";
		if (memory.is_valid()) {
			memory->record_run_finished(summary);
		}
		emit_signal("run_finished", summary);
		return;
	}

	_set_stage(STAGE_PLANNING, "sending results back to the model");
	llm->send_tool_results(pending_results);
	pending_results.clear();
}

void AIOSPipeline::_on_playtest_finished(const Dictionary &p_report) {
	if (!awaiting_playtest) {
		return; // A manual playtest, not one the pipeline asked for.
	}
	const bool is_auto = playtest_tool_use_id == "__auto_playtest__";
	awaiting_playtest = false;

	const String outcome = String(p_report["outcome"]);
	const bool failed = outcome == "errors" || outcome == "crashed";

	emit_signal("pipeline_log", failed ? "error" : "success", String(p_report["summary"]));

	if (is_auto) {
		playtest_tool_use_id = String();
		Dictionary block;
		block["type"] = "text";
		block["text"] = "AUTO PLAYTEST (" + outcome + "):\n" + build_playtest_feedback(p_report);
		pending_results.push_back(block);
		if (failed && memory.is_valid()) {
			memory->add_failure("run_playtest", String(p_report["summary"]), "Fix runtime errors before continuing.");
		}
	} else {
		// The report goes back as the tool result, formatted for a model rather
		// than for a log viewer.
		Dictionary block;
		block["type"] = "tool_result";
		block["tool_use_id"] = playtest_tool_use_id;
		block["content"] = build_playtest_feedback(p_report);
		if (failed) {
			block["is_error"] = true;
		}
		pending_results.push_back(block);
		playtest_tool_use_id = String();
	}

	if (failed) {
		repair_attempts++;
		if (repair_attempts >= max_repair_attempts) {
			// Mirror the scene-sweep path: budget spent always aborts. The old
			// code reset repair_attempts and continued, which let a confused
			// model burn turns forever after a "rollback performed" notice.
			if (auto_rollback && !step_snapshot.is_empty() && git.is_valid()) {
				Dictionary reset = git->reset_to_snapshot(step_snapshot);
				if ((bool)reset["ok"]) {
					_set_stage(STAGE_REPAIRING, "rolled back after repeated playtest failures");
					emit_signal("pipeline_log", "warn",
							"Repair budget spent after " + String::num_int64(repair_attempts) +
									" playtest failures. Reset to snapshot " + step_snapshot.substr(0, 8) + ".");
					_abort("repair_budget_exhausted",
							"The playtest failed " + String::num_int64(max_repair_attempts) +
									" times in a row, so the project was rolled back to where it was before this "
									"step. The diagnostics are in the log above.");
					return;
				}
				emit_signal("pipeline_log", "error",
						"Rollback failed: " + String(Dictionary(reset["error"])["message"]));
			}
			_abort("repair_budget_exhausted",
					"The playtest failed " + String::num_int64(max_repair_attempts) +
							" times in a row. Rollback is off (or failed), so the broken state was left on disk.");
			return;
		}
	}

	if (is_auto) {
		_finish_turn();
		return;
	}

	// Anything the model queued behind the playtest runs now, against a scene
	// whose runtime behaviour is known.
	Array queued = deferred_calls;
	deferred_calls.clear();
	for (int i = 0; i < queued.size(); i++) {
		_execute_call(queued[i]);
		if (awaiting_playtest || awaiting_user) {
			// A second playtest or an ask_user in the same batch: stop and wait.
			for (int j = i + 1; j < queued.size(); j++) {
				deferred_calls.push_back(queued[j]);
			}
			return;
		}
	}

	_finish_turn();
}
