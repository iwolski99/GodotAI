/**************************************************************************/
/*  aios_git_checkpoint.h                                                 */
/*  Transactional git engine: snapshots, checkpoints, rollback.           */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Undo for an autonomous agent has to survive editor restarts, span both scene
// files and generated scripts, and be inspectable by a human afterwards. Git
// already does all of that, so every step of the pipeline is bracketed by
// commits.
//
// Two rollback modes, and the difference matters:
//
//   * reset_to_snapshot() — `git reset --hard <sha>`. Used by the pipeline when
//     a step it just executed broke the project. This is only safe because a
//     snapshot is taken *before* the step runs, so by construction there is no
//     uncommitted work to destroy: everything the user had is already inside
//     the snapshot commit we are resetting to.
//
//   * rollback_last() — `git revert`. Used by the human's Rollback button, where
//     that guarantee does not hold. Reverting is additive, so it cannot destroy
//     work the user never committed. The cost is a noisier history, which is the
//     right trade for a button pressed with unknown state on disk.
//
// Mixing these up is the single most destructive bug this file could have, so
// each method states which invariant it relies on.
class AIOSGitCheckpoint : public RefCounted {
	GDCLASS(AIOSGitCheckpoint, RefCounted)

private:
	String repo_path;
	String git_executable;
	bool enabled = true;
	Array checkpoints; // Newest last. Entries: {sha, label, created_at}.

	Dictionary _run_git(const PackedStringArray &p_args) const;
	static String _resolve_git_executable();

protected:
	static void _bind_methods();

public:
	AIOSGitCheckpoint();

	void set_enabled(bool p_enabled) { enabled = p_enabled; }
	bool is_enabled() const { return enabled; }

	// True when the project directory is inside a git work tree and a git binary
	// was found. Everything else here degrades to a clean error if not.
	bool is_available() const;
	bool has_changes() const;

	String get_git_executable() const { return git_executable; }
	String get_head_sha() const;

	// --- transactional API (used by the pipeline) --------------------------
	//
	// Commits the current work tree and returns its SHA, whether or not there
	// was anything new to commit — the caller needs a SHA to reset to either
	// way. This is the "before" half of a transaction.
	Dictionary create_snapshot(const String &p_label);

	// HARD reset to a SHA taken from create_snapshot(). Destroys everything
	// after it, which is safe only because create_snapshot() committed the work
	// tree first. Never call this with a SHA from anywhere else.
	Dictionary reset_to_snapshot(const String &p_sha);

	// --- checkpoint API (used by tools and the dock) -----------------------
	Dictionary create_checkpoint(const String &p_label);
	Dictionary rollback_last();

	Array list_checkpoints() const { return checkpoints; }
	Dictionary get_status() const;
};
