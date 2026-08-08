/**************************************************************************/
/*  aios_playtest.cpp                                                     */
/**************************************************************************/

#include "aios_playtest.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#define AIOS_PLAYTEST_DIR "user://godot_ai_os"
#define AIOS_MAX_OUTPUT_LINES 2000
#define AIOS_MAX_DIAGNOSTICS 200

AIOSPlaytest::~AIOSPlaytest() {
	if (running && pid > 0) {
		OS::get_singleton()->kill(pid);
	}
}

void AIOSPlaytest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "params"), &AIOSPlaytest::start);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &AIOSPlaytest::poll);
	ClassDB::bind_method(D_METHOD("stop"), &AIOSPlaytest::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &AIOSPlaytest::is_running);
	ClassDB::bind_method(D_METHOD("get_diagnostics"), &AIOSPlaytest::get_diagnostics);
	ClassDB::bind_method(D_METHOD("get_scene"), &AIOSPlaytest::get_scene);
	ClassDB::bind_method(D_METHOD("get_elapsed"), &AIOSPlaytest::get_elapsed);

	ADD_SIGNAL(MethodInfo("playtest_started", PropertyInfo(Variant::STRING, "scene"), PropertyInfo(Variant::INT, "pid")));
	ADD_SIGNAL(MethodInfo("playtest_output", PropertyInfo(Variant::STRING, "stream"), PropertyInfo(Variant::STRING, "line")));
	ADD_SIGNAL(MethodInfo("playtest_diagnostic", PropertyInfo(Variant::DICTIONARY, "diagnostic")));
	ADD_SIGNAL(MethodInfo("playtest_finished", PropertyInfo(Variant::DICTIONARY, "report")));
}

String AIOSPlaytest::_log_file_path() {
	// user:// keeps the log out of the project folder — a playtest log inside
	// res:// would show up in the FileSystem dock and get committed.
	return String(AIOS_PLAYTEST_DIR) + "/playtest.log";
}

/* -------------------------------------------------------------------------- */
/*  Launching                                                                  */
/* -------------------------------------------------------------------------- */

Dictionary AIOSPlaytest::start(const Dictionary &p_params) {
	if (running) {
		return AIOSJson::error("already_running",
				"A playtest is already running. Stop it before starting another.");
	}

	OS *os = OS::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();

	String scene = AIOSJson::get_string(p_params, "scene", "");
	if (scene.is_empty()) {
		scene = ps->get_setting("application/run/main_scene", "");
	}
	if (scene.is_empty()) {
		return AIOSJson::error("no_scene",
				"No scene to run: none was given and the project has no main scene set.");
	}
	if (!FileAccess::file_exists(scene)) {
		return AIOSJson::error("scene_not_found", "No scene file at '" + scene + "'.");
	}

	// Reset per-run state.
	diagnostics.clear();
	output_lines.clear();
	error_count = 0;
	warning_count = 0;
	rx_buffer = "";
	read_offset = 0;
	elapsed = 0.0;
	has_pending = false;
	pending = Dictionary();
	scene_path = scene;
	timeout_sec = (double)AIOSJson::get_int(p_params, "timeout_sec", 60);
	if (timeout_sec <= 0.0) {
		timeout_sec = 60.0;
	}
	quit_after_frames = AIOSJson::get_int(p_params, "quit_after_frames", 0);
	if (quit_after_frames < 0) {
		quit_after_frames = 0;
	}

	if (!DirAccess::dir_exists_absolute(AIOS_PLAYTEST_DIR)) {
		DirAccess::make_dir_recursive_absolute(AIOS_PLAYTEST_DIR);
	}
	log_path = _log_file_path();
	// Truncate any previous run's log, so a stale file cannot be mistaken for
	// this run's output if the child fails to start.
	{
		Ref<FileAccess> truncate = FileAccess::open(log_path, FileAccess::WRITE);
		if (truncate.is_valid()) {
			truncate->close();
		}
	}

	const String project_dir = ps->globalize_path("res://");
	const String absolute_log = ps->globalize_path(log_path);

	PackedStringArray args;
	args.push_back("--path");
	args.push_back(project_dir);
	args.push_back("--log-file");
	args.push_back(absolute_log);

	if (AIOSJson::get_bool(p_params, "headless", false)) {
		// Headless is the right default for CI and for a machine with no
		// display, but a human watching an agent work usually wants to see the
		// window, so it stays opt-in.
		args.push_back("--headless");
	}

	if (quit_after_frames > 0) {
		// Deterministic exit for smoke tests: the game shuts itself down after
		// N frames instead of relying on the timeout to kill it.
		args.push_back("--quit-after");
		args.push_back(String::num_int64(quit_after_frames));
	}

	args.push_back(scene);

	// create_process rather than execute: execute blocks until the child exits,
	// which would freeze the editor for the whole playtest.
	pid = (int)os->create_process(os->get_executable_path(), args, false);
	if (pid <= 0) {
		return AIOSJson::error("launch_failed",
				"Could not start a Godot process from '" + os->get_executable_path() + "'.");
	}

	running = true;
	started_usec = Time::get_singleton()->get_ticks_usec();
	emit_signal("playtest_started", scene_path, pid);

	Dictionary result;
	result["scene"] = scene_path;
	result["pid"] = pid;
	result["timeout_sec"] = timeout_sec;
	result["log_file"] = log_path;
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Output parsing                                                             */
/* -------------------------------------------------------------------------- */

// Godot writes a diagnostic as a message line followed by an indented location:
//
//   SCRIPT ERROR: Invalid access to property 'x' on a base object of type 'Nil'.
//      at: _ready (res://scripts/player.gd:12)
//
// Getting file and line out of that second line is the whole reason an agent
// can fix the bug instead of guessing at it, so the parser holds each finding
// open until it has seen whether a location follows.
void AIOSPlaytest::_flush_pending() {
	if (!has_pending) {
		return;
	}
	if (diagnostics.size() < AIOS_MAX_DIAGNOSTICS) {
		diagnostics.push_back(pending);
		emit_signal("playtest_diagnostic", pending);
	}
	has_pending = false;
	pending = Dictionary();
}

void AIOSPlaytest::_consume_line(const String &p_line) {
	const String line = p_line.strip_edges();
	if (line.is_empty()) {
		return;
	}

	// Location continuation for the diagnostic we are holding.
	if (has_pending && line.begins_with("at:")) {
		const String rest = line.substr(3).strip_edges();
		const int open = rest.rfind("(");
		const int close = rest.rfind(")");
		if (open >= 0 && close > open) {
			const String location = rest.substr(open + 1, close - open - 1);
			const int colon = location.rfind(":");
			if (colon >= 0) {
				pending["file"] = location.substr(0, colon);
				pending["line"] = location.substr(colon + 1).to_int();
			} else {
				pending["file"] = location;
			}
			pending["function"] = rest.substr(0, open).strip_edges();
		} else {
			pending["function"] = rest;
		}
		_flush_pending();
		return;
	}

	struct Marker {
		const char *prefix;
		const char *severity;
		const char *kind;
	};
	static const Marker markers[] = {
		{ "SCRIPT ERROR:", "error", "script" },
		{ "USER SCRIPT ERROR:", "error", "script" },
		{ "USER ERROR:", "error", "user" },
		{ "ERROR:", "error", "engine" },
		{ "USER WARNING:", "warning", "user" },
		{ "WARNING:", "warning", "engine" },
	};

	for (size_t i = 0; i < sizeof(markers) / sizeof(Marker); i++) {
		const String prefix = markers[i].prefix;
		if (!line.begins_with(prefix)) {
			continue;
		}
		_flush_pending();

		pending = Dictionary();
		pending["severity"] = markers[i].severity;
		pending["kind"] = markers[i].kind;
		pending["message"] = line.substr(prefix.length()).strip_edges();
		pending["raw"] = line;
		has_pending = true;

		if (String(markers[i].severity) == "error") {
			error_count++;
		} else {
			warning_count++;
		}

		emit_signal("playtest_output", "stderr", line);
		return;
	}

	// Anything we were holding is complete: this line is not its location.
	_flush_pending();

	if (output_lines.size() < AIOS_MAX_OUTPUT_LINES) {
		output_lines.push_back(line);
	}
	emit_signal("playtest_output", "stdout", line);
}

void AIOSPlaytest::_drain_log() {
	Ref<FileAccess> file = FileAccess::open(log_path, FileAccess::READ);
	if (file.is_null()) {
		return;
	}

	const int64_t size = file->get_length();
	if (size < read_offset) {
		// The file shrank, which means the child truncated and restarted it.
		read_offset = 0;
	}
	if (size == read_offset) {
		file->close();
		return;
	}

	file->seek(read_offset);
	rx_buffer += file->get_buffer(size - read_offset).get_string_from_utf8();
	read_offset = size;
	file->close();

	PackedStringArray lines = rx_buffer.split("\n");
	// The last element is a partial line unless the buffer ended on a newline;
	// hold it back so a diagnostic is never split across two polls.
	rx_buffer = lines[lines.size() - 1];
	for (int i = 0; i < lines.size() - 1; i++) {
		_consume_line(lines[i]);
	}
}

/* -------------------------------------------------------------------------- */
/*  Polling                                                                    */
/* -------------------------------------------------------------------------- */

void AIOSPlaytest::poll(double p_delta) {
	if (!running) {
		return;
	}
	elapsed += p_delta;

	_drain_log();

	OS *os = OS::get_singleton();
	if (!os->is_process_running(pid)) {
		// Give the log one more read: the child may have written its final
		// lines (including the error that killed it) between the last drain
		// and the process actually exiting.
		_drain_log();
		if (!rx_buffer.strip_edges().is_empty()) {
			_consume_line(rx_buffer);
			rx_buffer = "";
		}
		_flush_pending();

		running = false;
		// is_process_running() tells us the child is gone but not why. Godot's
		// OS API has no cross-platform exit-status query, so the outcome is
		// derived from what the run logged — plus a heuristic for silent
		// immediate death that would otherwise look like a clean pass.
		Outcome outcome = OUTCOME_CLEAN;
		if (error_count > 0) {
			outcome = OUTCOME_ERRORS;
		} else if (quit_after_frames <= 0 && elapsed < 0.75 && output_lines.size() < 3) {
			// No quit_after_frames, almost no output, died in under a second:
			// treat as a crash rather than a false clean. Intentional smoke
			// tests set quit_after_frames and are exempt.
			outcome = OUTCOME_CRASHED;
		} else if (quit_after_frames <= 0 && elapsed < 0.15) {
			outcome = OUTCOME_CRASHED;
		}
		emit_signal("playtest_finished", _build_report(outcome, 0));
		return;
	}

	if (elapsed >= timeout_sec) {
		os->kill(pid);
		running = false;
		_drain_log();
		_flush_pending();
		emit_signal("playtest_finished", _build_report(OUTCOME_TIMEOUT, -1));
	}
}

Dictionary AIOSPlaytest::stop() {
	if (!running) {
		return AIOSJson::error("not_running", "No playtest is running.");
	}
	OS::get_singleton()->kill(pid);
	running = false;
	_drain_log();
	_flush_pending();

	Dictionary report = _build_report(OUTCOME_TIMEOUT, -1);
	report["stopped_by_user"] = true;
	emit_signal("playtest_finished", report);
	return AIOSJson::ok(report);
}

/* -------------------------------------------------------------------------- */
/*  Reporting                                                                  */
/* -------------------------------------------------------------------------- */

Dictionary AIOSPlaytest::_build_report(Outcome p_outcome, int p_exit_code) {
	String outcome_name;
	String summary;
	switch (p_outcome) {
		case OUTCOME_CLEAN:
			outcome_name = "clean";
			summary = "The scene ran and exited without logging any errors.";
			break;
		case OUTCOME_ERRORS:
			outcome_name = "errors";
			summary = "The scene ran but logged " + String::num_int64(error_count) + " error(s).";
			break;
		case OUTCOME_CRASHED:
			outcome_name = "crashed";
			if (error_count == 0 && elapsed < 0.75) {
				summary = "The process exited almost immediately without logging errors — treated as a "
						  "crash (Godot exposes no portable exit code here). Re-run with quit_after_frames "
						  "for a deterministic smoke test, or check the engine log.";
			} else {
				summary = "The process exited abnormally.";
			}
			break;
		case OUTCOME_TIMEOUT:
			outcome_name = "timeout";
			summary = "The scene was still running after " + String::num(timeout_sec, 1) +
					"s and was terminated. That is expected for a game with no exit condition - pass "
					"quit_after_frames for a deterministic smoke test.";
			break;
		default:
			outcome_name = "none";
			break;
	}

	Dictionary report;
	report["outcome"] = outcome_name;
	report["summary"] = summary;
	report["scene"] = scene_path;
	report["elapsed_sec"] = elapsed;
	report["exit_code"] = p_exit_code;
	report["error_count"] = error_count;
	report["warning_count"] = warning_count;
	report["diagnostics"] = diagnostics;
	report["passed"] = p_outcome == OUTCOME_CLEAN;

	// The tail rather than the head: when something goes wrong, the last thing
	// printed before it is what explains it.
	Array tail;
	const int start = output_lines.size() > 40 ? output_lines.size() - 40 : 0;
	for (int i = start; i < output_lines.size(); i++) {
		tail.push_back(output_lines[i]);
	}
	report["output_tail"] = tail;
	report["output_truncated"] = output_lines.size() >= AIOS_MAX_OUTPUT_LINES;

	return report;
}
