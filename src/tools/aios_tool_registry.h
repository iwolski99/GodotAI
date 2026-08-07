/**************************************************************************/
/*  aios_tool_registry.h                                                  */
/*  Name -> implementation dispatch for every agent-callable tool.        */
/**************************************************************************/

#pragma once

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
	Dictionary schema_cache;
	bool auto_checkpoint = true;

	Dictionary _load_schema(const String &p_tool);

protected:
	static void _bind_methods();

public:
	void setup(const Ref<AIOSWorldModel> &p_world_model, const Ref<AIOSGitCheckpoint> &p_git);

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
};
