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
	ClassDB::bind_method(D_METHOD("stop"), &AIOSPipeline::stop);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &AIOSPipeline::poll);
	ClassDB::bind_method(D_METHOD("get_stage_name"), &AIOSPipeline::get_stage_name);
	ClassDB::bind_method(D_METHOD("get_status"), &AIOSPipeline::get_status);
	ClassDB::bind_method(D_METHOD("set_max_repair_attempts", "attempts"), &AIOSPipeline::set_max_repair_attempts);
	ClassDB::bind_method(D_METHOD("set_auto_playtest", "enabled"), &AIOSPipeline::set_auto_playtest);
	ClassDB::bind_method(D_METHOD("set_auto_rollback", "enabled"), &AIOSPipeline::set_auto_rollback);

	ADD_SIGNAL(MethodInfo("stage_changed", PropertyInfo(Variant::STRING, "stage"), PropertyInfo(Variant::STRING, "detail")));
	ADD_SIGNAL(MethodInfo("agent_message", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("agent_thinking", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("pipeline_log", PropertyInfo(Variant::STRING, "level"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("tool_invoked", PropertyInfo(Variant::STRING, "tool"), PropertyInfo(Variant::DICTIONARY, "params")));
	ADD_SIGNAL(MethodInfo("tool_completed", PropertyInfo(Variant::STRING, "tool"), PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::DICTIONARY, "envelope")));
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
/*  System prompt                                                              */
/* -------------------------------------------------------------------------- */

String AIOSPipeline::build_system_prompt(const String &p_mode, bool p_git_available) {
	String prompt =
			"You are an autonomous game developer working inside the Godot 4 editor through the AI Agent OS "
			"plugin. You have tools that read and modify a live Godot project. The human can see everything you "
			"do in a dock and can stop you at any time.\n\n";

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

	// Mode is the one thing that changes what the agent is *for*. Keeping the
	// difference small and concrete beats four divergent prompts that drift.
	const String m = p_mode.to_lower();
	prompt += "## Your role right now: " + m + "\n\n";
	if (m == "architect") {
		prompt +=
				"Plan before you build. Inspect the project, propose a concrete structure, and explain the "
				"trade-offs in a sentence or two before creating anything. Prefer a small number of "
				"well-named nodes over a deep tree. Do not write gameplay code unless the plan needs it to "
				"be meaningful.";
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

	goal = p_goal;
	mode = p_mode;
	repair_attempts = 0;
	steps_executed = 0;
	pending_results.clear();
	deferred_calls.clear();
	awaiting_playtest = false;

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
	llm->set_system_prompt(build_system_prompt(mode, git_ok));
	if (registry.is_valid()) {
		// Hand the model the same manifest external agents get, so the two
		// paths cannot drift apart.
		Array manifest = registry->list_tools();
		Array tools;
		for (int i = 0; i < manifest.size(); i++) {
			Dictionary entry = manifest[i];
			Dictionary tool;
			tool["name"] = entry["name"];
			tool["description"] = entry["summary"];
			tool["input_schema"] = entry.has("input_schema") ? entry["input_schema"] : Variant(Dictionary());
			tools.push_back(tool);
		}
		llm->set_tools(tools);
	}

	_set_stage(STAGE_PLANNING, "sending the goal to " + llm->describe_target());
	llm->send_user_message(p_goal);

	Dictionary result;
	result["goal"] = goal;
	result["mode"] = mode;
	result["model"] = llm->describe_target();
	result["git"] = git_ok;
	return AIOSJson::ok(result);
}

void AIOSPipeline::stop() {
	if (stage == STAGE_IDLE) {
		return;
	}
	if (llm != nullptr) {
		llm->cancel();
	}
	if (playtest.is_valid() && playtest->is_running()) {
		playtest->stop();
	}
	awaiting_playtest = false;
	pending_results.clear();
	deferred_calls.clear();
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
	emit_signal("run_finished", summary);
	stage = STAGE_IDLE;
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

	// One snapshot per batch, not per call. A model routinely emits several
	// calls that only make sense together (create a node, then attach its
	// script); rolling back to the middle of that would leave a half-built
	// scene that is worse than either end state.
	if (git.is_valid() && git->is_available()) {
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

		if (awaiting_playtest) {
			// Everything after a playtest call has to wait for its report —
			// running further edits while the game is mid-run would change the
			// files under it and make the diagnostics meaningless.
			deferred_calls.push_back(call);
			continue;
		}
		_execute_call(call);
	}

	if (!awaiting_playtest) {
		_finish_turn();
	}
}

void AIOSPipeline::_execute_call(const Dictionary &p_call) {
	const String tool = String(p_call.get("name", ""));
	const String id = String(p_call.get("id", ""));
	Dictionary params = p_call.get("input", Dictionary());

	emit_signal("tool_invoked", tool, params);

	// --- Validate ----------------------------------------------------------
	_set_stage(STAGE_VALIDATING, tool);
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
		if (budget_spent && auto_rollback && !step_snapshot.is_empty() && git.is_valid()) {
			Dictionary reset = git->reset_to_snapshot(step_snapshot);
			if ((bool)reset["ok"]) {
				emit_signal("pipeline_log", "warn",
						"Repair budget spent after " + String::num_int64(repair_attempts) +
								" attempts. Reset to snapshot " + step_snapshot.substr(0, 8) + ".");
				_abort("repair_budget_exhausted",
						"The agent could not produce a working scene in " + String::num_int64(max_repair_attempts) +
								" attempts, so the project was rolled back to where it was before this step. "
								"The findings are in the log above - this is a good moment to look at them yourself.");
				return;
			}
			emit_signal("pipeline_log", "error",
					"Rollback failed: " + String(Dictionary(reset["error"])["message"]));
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
	awaiting_playtest = false;

	const String outcome = String(p_report["outcome"]);
	const bool failed = outcome == "errors" || outcome == "crashed";

	emit_signal("pipeline_log", failed ? "error" : "success", String(p_report["summary"]));

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

	if (failed) {
		repair_attempts++;
		if (repair_attempts >= max_repair_attempts && auto_rollback && !step_snapshot.is_empty() && git.is_valid()) {
			Dictionary reset = git->reset_to_snapshot(step_snapshot);
			if ((bool)reset["ok"]) {
				_set_stage(STAGE_REPAIRING, "rolled back after repeated playtest failures");
				Dictionary notice;
				notice["type"] = "text";
				notice["text"] = build_rollback_notice(
						"the playtest failed " + String::num_int64(repair_attempts) + " times in a row",
						step_snapshot);
				pending_results.push_back(notice);
				repair_attempts = 0;
			}
		}
	}

	// Anything the model queued behind the playtest runs now, against a scene
	// whose runtime behaviour is known.
	Array queued = deferred_calls;
	deferred_calls.clear();
	for (int i = 0; i < queued.size(); i++) {
		_execute_call(queued[i]);
		if (awaiting_playtest) {
			// A second playtest in the same batch: stop again and wait.
			for (int j = i + 1; j < queued.size(); j++) {
				deferred_calls.push_back(queued[j]);
			}
			return;
		}
	}

	_finish_turn();
}
