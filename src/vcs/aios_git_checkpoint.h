/**************************************************************************/
/*  aios_git_checkpoint.h                                                 */
/*  Git-backed checkpoint / rollback for agent-driven edits.              */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Undo for an autonomous agent has to survive editor restarts, span both scene
// files and generated scripts, and be inspectable by a human afterwards. Git
// already does all of that, so instead of building a bespoke journal we commit
// a checkpoint after every mutating tool call.
//
// Rollback is a `git revert`, never a `git reset --hard`: reverting is additive,
// so an agent (or a bug in this plugin) can never destroy work the user had not
// committed themselves. The cost is a slightly noisier history, which is the
// right trade for an undo button an AI is allowed to press.
class AIOSGitCheckpoint : public RefCounted {
	GDCLASS(AIOSGitCheckpoint, RefCounted)

private:
	String repo_path;
	bool enabled = true;
	Array checkpoints; // Newest last. Entries: {sha, label, created_at}.

	Dictionary _run_git(const PackedStringArray &p_args) const;

protected:
	static void _bind_methods();

public:
	AIOSGitCheckpoint();

	void set_enabled(bool p_enabled) { enabled = p_enabled; }
	bool is_enabled() const { return enabled; }

	// True when the project directory is inside a git work tree and `git` is on
	// PATH. Everything else in this class degrades to a clean error if not.
	bool is_available() const;

	bool has_changes() const;

	// Commits everything currently in the work tree under an [ai-checkpoint]
	// subject line. Returns {ok, result:{sha, label, created}} — created is
	// false when there was nothing to commit.
	Dictionary create_checkpoint(const String &p_label);

	// Reverts the most recent checkpoint this session created.
	Dictionary rollback_last();

	Array list_checkpoints() const { return checkpoints; }
	Dictionary get_status() const;
};
