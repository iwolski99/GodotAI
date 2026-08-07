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
}

void AIOSGitCheckpoint::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &AIOSGitCheckpoint::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &AIOSGitCheckpoint::is_enabled);
	ClassDB::bind_method(D_METHOD("is_available"), &AIOSGitCheckpoint::is_available);
	ClassDB::bind_method(D_METHOD("has_changes"), &AIOSGitCheckpoint::has_changes);
	ClassDB::bind_method(D_METHOD("create_checkpoint", "label"), &AIOSGitCheckpoint::create_checkpoint);
	ClassDB::bind_method(D_METHOD("rollback_last"), &AIOSGitCheckpoint::rollback_last);
	ClassDB::bind_method(D_METHOD("list_checkpoints"), &AIOSGitCheckpoint::list_checkpoints);
	ClassDB::bind_method(D_METHOD("get_status"), &AIOSGitCheckpoint::get_status);

	ADD_SIGNAL(MethodInfo("checkpoint_created", PropertyInfo(Variant::STRING, "sha"), PropertyInfo(Variant::STRING, "label")));
	ADD_SIGNAL(MethodInfo("rolled_back", PropertyInfo(Variant::STRING, "sha"), PropertyInfo(Variant::STRING, "label")));
}

Dictionary AIOSGitCheckpoint::_run_git(const PackedStringArray &p_args) const {
	// -C keeps us honest about the working directory: OS::execute inherits the
	// editor's cwd, which is not necessarily the project folder.
	PackedStringArray args;
	args.push_back("-C");
	args.push_back(repo_path);
	args.append_array(p_args);

	Array output;
	const int exit_code = (int)OS::get_singleton()->execute("git", args, output, true);

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

Dictionary AIOSGitCheckpoint::create_checkpoint(const String &p_label) {
	if (!enabled) {
		return AIOSJson::error("checkpoints_disabled", "Automatic checkpoints are turned off in Project Settings (ai_agent_os/vcs/auto_checkpoint).");
	}
	if (!is_available()) {
		return AIOSJson::error("git_unavailable",
				"No git work tree at " + repo_path + ". Run `git init` in the project folder to enable checkpoints and rollback.");
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
