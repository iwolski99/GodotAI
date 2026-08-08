/**************************************************************************/
/*  aios_tool_registry.h                                                  */
/*  Name -> implementation dispatch for every agent-callable tool.        */
/**************************************************************************/

#pragma once

#include "../assets/aios_asset_pipeline.h"
#include "../playtest/aios_playtest.h"
#include "../vcs/aios_git_checkpoint.h"
#include "../world/aios_world_model.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The single funnel every agent request passes through. Keeping dispatch in one
// place is what lets us apply cross-cutting policy — checkpointing, cache
// invalidation, logging — without each tool remembering to do it.
class AIOSToolRegistry : public RefCounted {
	GDCLASS(AIOSToolRegistry, RefCounted)

private:
	Ref<AIOSWorldModel> world_model;
	Ref<AIOSGitCheckpoint> git;
	Ref<AIOSPlaytest> playtest;
	AIOSAssetPipeline *asset_pipeline = nullptr; // Owned by the plugin's tree.
	Dictionary schema_cache;
	bool auto_checkpoint = true;

	// Paid-API call accounting. -1 means unlimited.
	int billable_calls = 0;
	int billable_budget = 10;

	Dictionary _load_schema(const String &p_tool);

protected:
	static void _bind_methods();

public:
	void setup(const Ref<AIOSWorldModel> &p_world_model, const Ref<AIOSGitCheckpoint> &p_git,
			const Ref<AIOSPlaytest> &p_playtest, AIOSAssetPipeline *p_assets);

	// Tools that spend real money at a real provider. Kept separate from
	// "mutating" because the two properties are unrelated: a paid call can be
	// read-only, and most mutating calls are free.
	static bool is_billable(const String &p_tool);

	void set_billable_budget(int p_budget) { billable_budget = p_budget; }
	int get_billable_calls() const { return billable_calls; }
	void reset_billable_calls() { billable_calls = 0; }

	void set_auto_checkpoint(bool p_enabled) { auto_checkpoint = p_enabled; }
	bool is_auto_checkpoint() const { return auto_checkpoint; }

	// Returns an {ok, result} / {ok, error} envelope. Never throws.
	Dictionary call_tool(const String &p_tool, const Dictionary &p_params);

	PackedStringArray list_tool_names() const;
	Array list_tools(); // Full manifest: name, summary, mutating, schema.
	Dictionary get_tool_schema(const String &p_tool);

	// True for tools that change project state — these get a checkpoint and
	// invalidate the world model.
	static bool is_mutating(const String &p_tool);

	// True for tools that edit the open scene tree or its scripts.
	static bool is_scene_edit(const String &p_tool);

	// Tools allowed while the built-in agent is still interviewing the human
	// about what to build. Mutating scene/script tools are withheld until
	// commit_brief succeeds.
	static bool is_clarify_phase_tool(const String &p_tool);

	// Role-based allowlist enforced for built-in agent modes and IPC when a role
	// is set on the registry.
	static bool is_allowed_for_role(const String &p_tool, const String &p_role);

	void set_active_role(const String &p_role) { active_role = p_role; }
	String get_active_role() const { return active_role; }

private:
	String active_role;
};
