/**************************************************************************/
/*  aios_playtest.h                                                       */
/*  Launches the game and turns its console output into diagnostics.      */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The Observe stage. An agent that cannot run the game is guessing, so this is
// what turns "the code compiles" into "the game works".
//
// Capturing a child process's output is the awkward part, and the obvious
// approaches both fail here:
//
//   * OS::execute() captures stdout but blocks — it would freeze the editor for
//     the entire playtest.
//   * OS::execute_with_pipe() is non-blocking, but reading from the returned
//     pipe FileAccess blocks whenever no data is ready, which reintroduces the
//     freeze at a less predictable moment.
//
// So we use Godot's own `--log-file` flag: the child writes its full stdout and
// stderr to a file we name, and we tail that file from _process. Non-blocking,
// cross-platform, no autoload required in the game, and it captures engine
// errors and GDScript stack traces that never reach stdout in a readable form.
//
// Poll() must be called every frame while a playtest is running; the plugin
// does that from its own _process.
class AIOSPlaytest : public RefCounted {
	GDCLASS(AIOSPlaytest, RefCounted)

public:
	enum Outcome {
		OUTCOME_NONE,
		OUTCOME_CLEAN, // exited by itself, no errors logged
		OUTCOME_ERRORS, // ran to completion but logged errors
		OUTCOME_CRASHED, // non-zero exit
		OUTCOME_TIMEOUT, // had to be killed
	};

private:
	int pid = -1;
	bool running = false;
	String log_path;
	String scene_path;
	int64_t read_offset = 0;
	String rx_buffer;
	double elapsed = 0.0;
	double timeout_sec = 60.0;
	uint64_t started_usec = 0;

	Array diagnostics;
	Array output_lines;
	int error_count = 0;
	int warning_count = 0;

	// Godot prints a diagnostic's location on the line after the message, so
	// the parser holds the pending finding until it has seen (or ruled out)
	// that continuation line.
	Dictionary pending;
	bool has_pending = false;

	void _consume_line(const String &p_line);
	void _flush_pending();
	void _drain_log();
	Dictionary _build_report(Outcome p_outcome, int p_exit_code);
	static String _log_file_path();

protected:
	static void _bind_methods();

public:
	~AIOSPlaytest();

	// Starts a run. p_params: {scene, timeout_sec, quit_after_frames, headless}.
	Dictionary start(const Dictionary &p_params);

	// Call once per frame. Emits playtest_output / playtest_finished.
	void poll(double p_delta);

	// Kills the process. Reported as a timeout unless it had already exited.
	Dictionary stop();

	bool is_running() const { return running; }
	Array get_diagnostics() const { return diagnostics; }
	String get_scene() const { return scene_path; }
	double get_elapsed() const { return elapsed; }
};
