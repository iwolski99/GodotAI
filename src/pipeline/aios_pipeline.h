/**************************************************************************/
/*  aios_pipeline.h                                                       */
/*  The autonomous execution loop.                                        */
/**************************************************************************/

#pragma once

#include "../agent/aios_llm_client.h"
#include "../playtest/aios_playtest.h"
#include "../tools/aios_tool_registry.h"
#include "../vcs/aios_git_checkpoint.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The pipeline is the thing that makes this an agent OS rather than a remote
// control. It runs one strict cycle per model turn:
//
//   Plan ──▶ Intent ──▶ Validate ──▶ Execute ──▶ Observe ──▶ Repair ──▶ Snapshot
//     ▲                     │            │           │          │           │
//     │                     │ findings   │ tool      │ runtime  │ reset     │ commit
//     │                     ▼            ▼ result    ▼ errors   ▼ --hard    ▼
//     └───────────────── feed back into the model ───────────────────── Continue
//
// Two properties are load-bearing:
//
//   1. Validation happens before execution, and a step that fails validation is
//      never executed. The model gets the findings as a tool error and revises.
//      A rejected call costs one round trip; a bad executed call costs a
//      playtest, a rollback, and a confused repair attempt.
//
//   2. Every executed step is bracketed by a git snapshot. If validation or the
//      playtest fails afterwards, `git reset --hard` returns the project to the
//      exact state it was in before the step — and that reset is safe precisely
//      because the snapshot committed everything first.
//
// The repair budget is what stops a model that has misdiagnosed a problem from
// digging: after `max_repair_attempts` consecutive failures the pipeline resets
// to the last known-good snapshot and hands control back to the human.
class AIOSPipeline : public RefCounted {
	GDCLASS(AIOSPipeline, RefCounted)

public:
	enum Stage {
		STAGE_IDLE,
		STAGE_PLANNING,
		STAGE_VALIDATING,
		STAGE_EXECUTING,
		STAGE_PLAYTESTING,
		STAGE_REPAIRING,
		STAGE_ERROR,
	};

private:
	Ref<AIOSToolRegistry> registry;
	Ref<AIOSGitCheckpoint> git;
	Ref<AIOSPlaytest> playtest;
	AIOSLlmClient *llm = nullptr; // Owned by the plugin's scene tree.

	Stage stage = STAGE_IDLE;
	String goal;
	String mode = "architect";

	// Snapshot taken before the current batch of tool calls. The only SHA that
	// may ever be passed to reset_to_snapshot().
	String step_snapshot;
	String last_good_snapshot;

	int repair_attempts = 0;
	int max_repair_attempts = 3;
	int steps_executed = 0;

	// Tool results accumulated for the current turn, sent back together once
	// every call in the batch has resolved.
	Array pending_results;
	// Set while a playtest is in flight: the turn cannot complete until the
	// report arrives, so the remaining calls queue behind it.
	bool awaiting_playtest = false;
	String playtest_tool_use_id;
	Array deferred_calls;

	bool auto_playtest = true;
	bool auto_rollback = true;

	void _set_stage(Stage p_stage, const String &p_detail);
	void _handle_tool_calls(const Array &p_calls);
	void _execute_call(const Dictionary &p_call);
	void _finish_turn();
	void _push_result(const String &p_id, const Dictionary &p_payload, bool p_is_error);
	void _abort(const String &p_code, const String &p_message);

	// The prompt wrapper: turns machine findings into text a model can act on.
	static String build_validation_feedback(const String &p_tool, const Array &p_findings);
	static String build_playtest_feedback(const Dictionary &p_report);
	static String build_rollback_notice(const String &p_reason, const String &p_sha);

	void _on_model_response(const Dictionary &p_response);
	void _on_model_failed(const Dictionary &p_error);
	void _on_playtest_finished(const Dictionary &p_report);

protected:
	static void _bind_methods();

public:
	void setup(const Ref<AIOSToolRegistry> &p_registry,
			const Ref<AIOSGitCheckpoint> &p_git,
			const Ref<AIOSPlaytest> &p_playtest,
			AIOSLlmClient *p_llm);

	void set_max_repair_attempts(int p_attempts) { max_repair_attempts = p_attempts > 0 ? p_attempts : 1; }
	void set_auto_playtest(bool p_enabled) { auto_playtest = p_enabled; }
	void set_auto_rollback(bool p_enabled) { auto_rollback = p_enabled; }

	// Kicks off a run. Returns an error envelope if preconditions fail.
	Dictionary start(const String &p_goal, const String &p_mode);

	// Aborts. Leaves the project as-is; the human decides whether to roll back.
	void stop();

	// Drives the playtest. Called every frame by the plugin.
	void poll(double p_delta);

	Stage get_stage() const { return stage; }
	String get_stage_name() const;
	Dictionary get_status() const;

	// The system prompt handed to the model, assembled from the mode and the
	// project's state.
	static String build_system_prompt(const String &p_mode, bool p_git_available);
};
