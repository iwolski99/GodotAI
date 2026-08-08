/**************************************************************************/
/*  aios_git_checkpoint.cpp                                               */
/**************************************************************************/

#include "aios_git_checkpoint.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

AIOSGitCheckpoint::AIOSGitCheckpoint() {
	repo_path = ProjectSettings::get_singleton()->globalize_path("res://");
	if (repo_path.ends_with("/")) {
		repo_path = repo_path.substr(0, repo_path.length() - 1);
	}
	git_executable = _resolve_git_executable();
}

// Finding git is a Windows problem. On Linux and macOS it is on PATH or it is
// not installed; on Windows, Godot launched from the Start menu or Steam
// inherits a PATH that frequently lacks Git for Windows even though the user
// has it, so a bare "git" fails for someone who is looking straight at a git
// repository. Probing the standard install locations turns a confusing
// "checkpoints unavailable" into a working feature.
String AIOSGitCheckpoint::_resolve_git_executable() {
	OS *os = OS::get_singleton();

	// An explicit override wins over everything — the escape hatch for portable
	// installs and for anyone running a wrapper.
	const String env_override = os->get_environment("GODOT_AI_OS_GIT");
	if (!env_override.is_empty()) {
		return env_override;
	}

	PackedStringArray probe;
	probe.push_back("--version");
	Array discard;
	if (os->execute("git", probe, discard, false) == 0) {
		return "git";
	}

	if (os->get_name() != "Windows") {
		// Nothing else to try; report "git" so the error message names the
		// command the user is expected to install.
		return "git";
	}

	PackedStringArray candidates;
	const String program_files = os->get_environment("ProgramFiles");
	const String program_files_x86 = os->get_environment("ProgramFiles(x86)");
	const String local_app_data = os->get_environment("LOCALAPPDATA");
	if (!program_files.is_empty()) {
		candidates.push_back(program_files.replace("\\", "/") + "/Git/cmd/git.exe");
	}
	if (!program_files_x86.is_empty()) {
		candidates.push_back(program_files_x86.replace("\\", "/") + "/Git/cmd/git.exe");
	}
	if (!local_app_data.is_empty()) {
		// Git for Windows' per-user install, and GitHub Desktop's bundled copy.
		candidates.push_back(local_app_data.replace("\\", "/") + "/Programs/Git/cmd/git.exe");
		candidates.push_back(local_app_data.replace("\\", "/") + "/GitHubDesktop/app/resources/app/git/cmd/git.exe");
	}
	candidates.push_back("C:/Program Files/Git/cmd/git.exe");

	for (int i = 0; i < candidates.size(); i++) {
		Array probe_output;
		if (os->execute(candidates[i], probe, probe_output, false) == 0) {
			return candidates[i];
		}
	}
	return "git";
}

void AIOSGitCheckpoint::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &AIOSGitCheckpoint::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &AIOSGitCheckpoint::is_enabled);
	ClassDB::bind_method(D_METHOD("is_available"), &AIOSGitCheckpoint::is_available);
	ClassDB::bind_method(D_METHOD("has_changes"), &AIOSGitCheckpoint::has_changes);
	ClassDB::bind_method(D_METHOD("get_git_executable"), &AIOSGitCheckpoint::get_git_executable);
	ClassDB::bind_method(D_METHOD("get_head_sha"), &AIOSGitCheckpoint::get_head_sha);
	ClassDB::bind_method(D_METHOD("create_snapshot", "label"), &AIOSGitCheckpoint::create_snapshot);
	ClassDB::bind_method(D_METHOD("reset_to_snapshot", "sha"), &AIOSGitCheckpoint::reset_to_snapshot);
	ClassDB::bind_method(D_METHOD("create_checkpoint", "label"), &AIOSGitCheckpoint::create_checkpoint);
	ClassDB::bind_method(D_METHOD("rollback_last"), &AIOSGitCheckpoint::rollback_last);
	ClassDB::bind_method(D_METHOD("list_checkpoints"), &AIOSGitCheckpoint::list_checkpoints);
	ClassDB::bind_method(D_METHOD("get_status"), &AIOSGitCheckpoint::get_status);

	ADD_SIGNAL(MethodInfo("checkpoint_created", PropertyInfo(Variant::STRING, "sha"), PropertyInfo(Variant::STRING, "label")));
	ADD_SIGNAL(MethodInfo("rolled_back", PropertyInfo(Variant::STRING, "sha"), PropertyInfo(Variant::STRING, "label")));
	ADD_SIGNAL(MethodInfo("snapshot_created", PropertyInfo(Variant::STRING, "sha"), PropertyInfo(Variant::STRING, "label")));
	ADD_SIGNAL(MethodInfo("reset_performed", PropertyInfo(Variant::STRING, "sha")));
}

Dictionary AIOSGitCheckpoint::_run_git(const PackedStringArray &p_args) const {
	// -C keeps us honest about the working directory: OS::execute inherits the
	// editor's cwd, which is not necessarily the project folder.
	PackedStringArray args;
	args.push_back("-C");
	args.push_back(repo_path);
	args.append_array(p_args);

	Array output;
	// open_console=false matters on Windows: the default would flash a console
	// window on every single git call the pipeline makes.
	const int exit_code = (int)OS::get_singleton()->execute(git_executable, args, output, true, false);

	String text;
	for (int i = 0; i < output.size(); i++) {
		text += String(output[i]);
	}

	Dictionary d;
	d["exit_code"] = exit_code;
	d["output"] = text.strip_edges();
	d["ok"] = exit_code == 0;
	return d;
}

bool AIOSGitCheckpoint::is_available() const {
	PackedStringArray args;
	args.push_back("rev-parse");
	args.push_back("--is-inside-work-tree");
	Dictionary res = _run_git(args);
	return (bool)res["ok"] && String(res["output"]).begins_with("true");
}

bool AIOSGitCheckpoint::has_changes() const {
	PackedStringArray args;
	args.push_back("status");
	args.push_back("--porcelain");
	Dictionary res = _run_git(args);
	return (bool)res["ok"] && !String(res["output"]).is_empty();
}

Dictionary AIOSGitCheckpoint::get_status() const {
	Dictionary d;
	d["repo_path"] = repo_path;
	d["enabled"] = enabled;
	const bool available = is_available();
	d["available"] = available;
	d["checkpoint_count"] = checkpoints.size();
	if (available) {
		PackedStringArray args;
		args.push_back("rev-parse");
		args.push_back("--abbrev-ref");
		args.push_back("HEAD");
		Dictionary res = _run_git(args);
		// A fresh repository with no commits has no HEAD to name; report that
		// rather than leaking git's error text into the agent's context.
		d["branch"] = (bool)res["ok"] ? res["output"] : Variant("(no commits yet)");
		d["dirty"] = has_changes();
	}
	return d;
}

String AIOSGitCheckpoint::get_head_sha() const {
	PackedStringArray args;
	args.push_back("rev-parse");
	args.push_back("HEAD");
	Dictionary res = _run_git(args);
	return (bool)res["ok"] ? String(res["output"]).strip_edges() : String();
}

/* -------------------------------------------------------------------------- */
/*  Transactional API                                                          */
/* -------------------------------------------------------------------------- */

Dictionary AIOSGitCheckpoint::create_snapshot(const String &p_label) {
	if (!is_available()) {
		return AIOSJson::error("git_unavailable",
				"No git work tree at " + repo_path + ". The pipeline needs one: without a snapshot to reset to, "
				"a failed step cannot be undone, so execution is refused rather than risking your project.");
	}

	// Commit whatever is on disk, so the returned SHA is a complete picture of
	// the project. This is the invariant reset_to_snapshot() depends on: after
	// this call there is nothing uncommitted left for a hard reset to destroy.
	if (has_changes()) {
		PackedStringArray add_args;
		add_args.push_back("add");
		add_args.push_back("-A");
		Dictionary added = _run_git(add_args);
		if (!(bool)added["ok"]) {
			return AIOSJson::error("git_add_failed", String(added["output"]));
		}

		PackedStringArray commit_args;
		commit_args.push_back("commit");
		commit_args.push_back("--no-verify");
		commit_args.push_back("--no-gpg-sign");
		commit_args.push_back("-m");
		commit_args.push_back("[ai-snapshot] " + (p_label.is_empty() ? String("before agent step") : p_label));
		Dictionary committed = _run_git(commit_args);
		if (!(bool)committed["ok"]) {
			return AIOSJson::error("git_commit_failed", String(committed["output"]));
		}
	}

	const String sha = get_head_sha();
	if (sha.is_empty()) {
		// A repository with no commits at all has no HEAD to snapshot, and
		// `reset --hard` would have nothing to aim at.
		return AIOSJson::error("no_commits",
				"The repository has no commits yet, so there is no state to roll back to. "
				"Make an initial commit before letting the agent run.");
	}

	emit_signal("snapshot_created", sha, p_label);

	Dictionary result;
	result["sha"] = sha;
	result["short_sha"] = sha.substr(0, 8);
	result["label"] = p_label;
	result["created_at"] = Time::get_singleton()->get_unix_time_from_system();
	return AIOSJson::ok(result);
}

Dictionary AIOSGitCheckpoint::reset_to_snapshot(const String &p_sha) {
	if (!is_available()) {
		return AIOSJson::error("git_unavailable", "No git work tree at " + repo_path + ".");
	}
	if (p_sha.strip_edges().is_empty()) {
		return AIOSJson::error("missing_parameter", "A snapshot SHA is required.");
	}

	// Refuse a SHA that is not an ancestor of HEAD. A caller passing something
	// from elsewhere — a stale variable, a hand-typed hash — would otherwise
	// silently discard unrelated history, and a hard reset is not recoverable
	// through this plugin.
	PackedStringArray verify;
	verify.push_back("merge-base");
	verify.push_back("--is-ancestor");
	verify.push_back(p_sha);
	verify.push_back("HEAD");
	Dictionary verified = _run_git(verify);
	if (!(bool)verified["ok"]) {
		return AIOSJson::error("not_an_ancestor",
				"'" + p_sha.substr(0, 8) + "' is not an ancestor of HEAD, so resetting to it would discard unrelated "
				"history. Refusing. Snapshots must come from create_snapshot() in this session.");
	}

	PackedStringArray args;
	args.push_back("reset");
	args.push_back("--hard");
	args.push_back(p_sha);
	Dictionary res = _run_git(args);
	if (!(bool)res["ok"]) {
		return AIOSJson::error("reset_failed", String(res["output"]));
	}

	// Untracked files a failed step left behind survive a reset, and a stray
	// half-written .gd is exactly what breaks the next run. Clean them, but
	// never touch ignored files (.godot/ holds the import cache and the session
	// token; wiping it would force a full reimport).
	PackedStringArray clean_args;
	clean_args.push_back("clean");
	clean_args.push_back("-fd");
	_run_git(clean_args);

	emit_signal("reset_performed", p_sha);

	Dictionary result;
	result["sha"] = p_sha;
	result["short_sha"] = p_sha.substr(0, 8);
	result["note"] = "Working tree reset on disk. Reopen the scene in the editor to load the restored version.";
	return AIOSJson::ok(result);
}

Dictionary AIOSGitCheckpoint::create_checkpoint(const String &p_label) {
	if (!enabled) {
		return AIOSJson::error("checkpoints_disabled", "Automatic checkpoints are turned off in Project Settings (ai_agent_os/vcs/auto_checkpoint).");
	}
	if (!is_available()) {
		return AIOSJson::error("git_unavailable",
				"No git work tree at " + repo_path + " (using '" + git_executable + "'). Run `git init` in the project folder to enable checkpoints and rollback.");
	}
	if (!has_changes()) {
		Dictionary result;
		result["created"] = false;
		result["reason"] = "nothing to commit";
		return AIOSJson::ok(result);
	}

	PackedStringArray add_args;
	add_args.push_back("add");
	add_args.push_back("-A");
	Dictionary added = _run_git(add_args);
	if (!(bool)added["ok"]) {
		return AIOSJson::error("git_add_failed", String(added["output"]));
	}

	const String label = p_label.is_empty() ? String("agent edit") : p_label;
	PackedStringArray commit_args;
	commit_args.push_back("commit");
	commit_args.push_back("--no-verify");
	commit_args.push_back("-m");
	commit_args.push_back("[ai-checkpoint] " + label);
	Dictionary committed = _run_git(commit_args);
	if (!(bool)committed["ok"]) {
		return AIOSJson::error("git_commit_failed", String(committed["output"]));
	}

	PackedStringArray sha_args;
	sha_args.push_back("rev-parse");
	sha_args.push_back("HEAD");
	Dictionary sha_res = _run_git(sha_args);
	const String sha = String(sha_res["output"]).strip_edges();

	Dictionary entry;
	entry["sha"] = sha;
	entry["label"] = label;
	entry["created_at"] = Time::get_singleton()->get_unix_time_from_system();
	checkpoints.push_back(entry);

	emit_signal("checkpoint_created", sha, label);

	Dictionary result;
	result["created"] = true;
	result["sha"] = sha;
	result["short_sha"] = sha.substr(0, 8);
	result["label"] = label;
	return AIOSJson::ok(result);
}

Dictionary AIOSGitCheckpoint::rollback_last() {
	if (!is_available()) {
		return AIOSJson::error("git_unavailable", "No git work tree at " + repo_path + ".");
	}
	if (checkpoints.is_empty()) {
		return AIOSJson::error("no_checkpoints", "No checkpoint has been recorded in this editor session, so there is nothing to roll back.");
	}

	// Uncommitted edits would block the revert and, worse, could be silently
	// mixed into it. Park them in their own checkpoint first.
	if (has_changes()) {
		Dictionary parked = create_checkpoint("uncommitted work parked before rollback");
		if (!(bool)parked["ok"]) {
			return parked;
		}
	}

	Dictionary entry = checkpoints[checkpoints.size() - 1];
	const String sha = entry["sha"];
	const String label = entry["label"];

	PackedStringArray args;
	args.push_back("revert");
	args.push_back("--no-edit");
	args.push_back("--no-gpg-sign");
	args.push_back(sha);
	Dictionary res = _run_git(args);

	if (!(bool)res["ok"]) {
		// Leave the tree exactly as the user found it rather than guessing at a
		// conflict resolution on their behalf.
		PackedStringArray abort_args;
		abort_args.push_back("revert");
		abort_args.push_back("--abort");
		_run_git(abort_args);
		return AIOSJson::error("revert_failed",
				"git could not revert checkpoint " + sha.substr(0, 8) + " cleanly; the working tree was left untouched.",
				Dictionary(res));
	}

	checkpoints.remove_at(checkpoints.size() - 1);
	emit_signal("rolled_back", sha, label);

	Dictionary result;
	result["reverted_sha"] = sha;
	result["short_sha"] = sha.substr(0, 8);
	result["label"] = label;
	result["remaining_checkpoints"] = checkpoints.size();
	result["note"] = "Files on disk were reverted. Reload the scene in the editor to see the change.";
	return AIOSJson::ok(result);
}
