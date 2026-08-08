/**************************************************************************/
/*  aios_pipeline.h                                                       */
/*  The autonomous execution loop.                                        */
/**************************************************************************/

#pragma once

#include "../agent/aios_llm_client.h"
#include "../agent/aios_agent_memory.h"
#include "../playtest/aios_playtest.h"
#include "../tools/aios_tool_registry.h"
#include "../vcs/aios_git_checkpoint.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The pipeline is the thing that makes this an agent OS rather than a remote
// control. It runs one strict cycle per model turn:
//
//   Clarify ──▶ Plan ──▶ Validate ──▶ Execute ──▶ Observe ──▶ Repair ──▶ Snapshot
//     ▲                      │            │           │          │           │
//     │ ask_user / brief     │ findings   │ tool      │ runtime  │ reset     │ commit
//     │                      ▼            ▼ result    ▼ errors   ▼ --hard    ▼
//     └────────────────── feed back into the model ──────────────────── Continue
//
// Clarify comes first for build-oriented modes: a vague goal like "make me an
// FPS" must become a committed 2D/3D brief before any mutating tool is offered.
//
// Two properties are load-bearing after the brief is locked:
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
		STAGE_CLARIFYING,
		STAGE_PLANNING,
		STAGE_VALIDATING,
		STAGE_EXECUTING,
		STAGE_PLAYTESTING,
		STAGE_REPAIRING,
		STAGE_AWAITING_APPROVAL,
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
	bool require_brief = true;
	bool require_plan_approval = false;

	// Clarification interview: withheld build tools until commit_brief.
	bool clarifying = false;
	bool brief_ready = false;
	bool awaiting_user = false;
	String ask_user_tool_use_id;
	Dictionary pending_questions;
	Dictionary committed_brief;

	// Plan review: mutating tools blocked until the human approves propose_plan.
	bool awaiting_plan_approval = false;
	bool plan_approved = false;
	Dictionary pending_plan;
	String propose_plan_tool_use_id;

	// Tracks whether the current tool batch changed the project (for auto_playtest).
	bool batch_had_mutations = false;

	Ref<AIOSAgentMemory> memory;

	void _set_stage(Stage p_stage, const String &p_detail);
	void _handle_tool_calls(const Array &p_calls);
	void _execute_call(const Dictionary &p_call);
	void _finish_turn();
	void _push_result(const String &p_id, const Dictionary &p_payload, bool p_is_error);
	void _abort(const String &p_code, const String &p_message);

	void _apply_tools_for_phase();
	bool _mode_requires_brief(const String &p_mode) const;
	String _memory_block() const;
	void _enter_build_phase(const Dictionary &p_brief, bool p_skipped_interview);
	static String _format_brief(const Dictionary &p_brief);
	static String _format_plan(const Dictionary &p_plan);
	static Dictionary _minimal_brief_from_goal(const String &p_goal);
	String _build_session_message(const String &p_text, const String &p_previous_mode, bool p_mode_changed) const;
	void _prepare_run_state(const String &p_goal, const String &p_mode, bool p_fresh_session);

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
	void set_require_brief(bool p_enabled) { require_brief = p_enabled; }
	void set_require_plan_approval(bool p_enabled) { require_plan_approval = p_enabled; }
	void set_memory(const Ref<AIOSAgentMemory> &p_memory) { memory = p_memory; }

	// Kicks off a run. Returns an error envelope if preconditions fail.
	Dictionary start(const String &p_goal, const String &p_mode);

	// Continues the same editor session after a mode switch or follow-up prompt.
	// Preserves LLM history and any committed brief / pending plan.
	Dictionary continue_session(const String &p_text, const String &p_mode);

	// Clears session state so the next prompt starts a brand-new run.
	void reset_session();

	// True when a prior prompt or handoff artifact should be preserved.
	bool has_session_context() const;

	// Continues a clarifying interview with the human's answer from the dock.
	Dictionary continue_with_user_answer(const String &p_answer);

	// Skip the interview: synthesise a minimal brief from the original goal and
	// unlock build tools. Used by the dock's "Skip & Build" action.
	Dictionary skip_clarification_and_build();

	// Approve a proposed plan and tell the model to proceed with mutations.
	Dictionary approve_plan();

	// Aborts. Leaves the project as-is; the human decides whether to roll back.
	void stop();

	// Drives the playtest. Called every frame by the plugin.
	void poll(double p_delta);

	Stage get_stage() const { return stage; }
	String get_stage_name() const;
	Dictionary get_status() const;

	bool is_clarifying() const { return clarifying && !brief_ready; }
	bool is_awaiting_user() const { return awaiting_user; }
	bool is_awaiting_plan_approval() const { return awaiting_plan_approval; }
	bool is_running() const { return stage != STAGE_IDLE && stage != STAGE_ERROR; }
	Dictionary get_committed_brief() const { return committed_brief; }
	Dictionary get_pending_plan() const { return pending_plan; }

	// The system prompt handed to the model, assembled from the mode and the
	// project's state.
	static String build_system_prompt(const String &p_mode, bool p_git_available, bool p_clarifying = false,
			const String &p_memory_block = String());
};
