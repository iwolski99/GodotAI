/**************************************************************************/
/*  aios_agent_memory.h                                                   */
/*  Per-project notes that survive across agent runs.                     */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Agents start every run with a blank conversation. Memory stores what worked,
// what broke, and project-specific conventions so the next run does not repeat
// the same mistakes. Lives under .godot/ai_agent_os/ — project-local, not in
// user://, so it travels with the repo when committed (and is gitignored by
// default via .godot/).
class AIOSAgentMemory : public RefCounted {
	GDCLASS(AIOSAgentMemory, RefCounted)

private:
	String memory_path;
	Dictionary data;

	void _ensure_loaded();
	bool _save() const;

protected:
	static void _bind_methods();

public:
	AIOSAgentMemory();

	void load();
	Dictionary get_data() const { return data; }

	// Rendered block for injection into the system prompt. Empty when new.
	String format_for_prompt() const;

	void add_convention(const String &p_note);
	void add_failure(const String &p_tool, const String &p_message, const String &p_resolution = String());
	void add_session_note(const String &p_note);

	void record_run_finished(const Dictionary &p_summary);
};
