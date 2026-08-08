/**************************************************************************/
/*  aios_agent_memory.cpp                                                 */
/**************************************************************************/

#include "aios_agent_memory.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#define AIOS_MEMORY_FILE "res://.godot/ai_agent_os/memory.json"
#define MAX_CONVENTIONS 20
#define MAX_FAILURES 30
#define MAX_NOTES 20

void AIOSAgentMemory::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load"), &AIOSAgentMemory::load);
	ClassDB::bind_method(D_METHOD("get_data"), &AIOSAgentMemory::get_data);
	ClassDB::bind_method(D_METHOD("format_for_prompt"), &AIOSAgentMemory::format_for_prompt);
	ClassDB::bind_method(D_METHOD("add_convention", "note"), &AIOSAgentMemory::add_convention);
	ClassDB::bind_method(D_METHOD("add_failure", "tool", "message", "resolution"), &AIOSAgentMemory::add_failure);
	ClassDB::bind_method(D_METHOD("add_session_note", "note"), &AIOSAgentMemory::add_session_note);
	ClassDB::bind_method(D_METHOD("record_run_finished", "summary"), &AIOSAgentMemory::record_run_finished);
}

AIOSAgentMemory::AIOSAgentMemory() {
	memory_path = AIOS_MEMORY_FILE;
}

void AIOSAgentMemory::_ensure_loaded() {
	if (!data.is_empty()) {
		return;
	}
	load();
}

void AIOSAgentMemory::load() {
	data.clear();
	if (!FileAccess::file_exists(memory_path)) {
		data["conventions"] = Array();
		data["failures"] = Array();
		data["session_notes"] = Array();
		data["runs"] = Array();
		return;
	}

	Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(memory_path));
	if (parsed.get_type() == Variant::DICTIONARY) {
		data = parsed;
	} else {
		data["conventions"] = Array();
		data["failures"] = Array();
		data["session_notes"] = Array();
		data["runs"] = Array();
	}
	if (!data.has("conventions")) {
		data["conventions"] = Array();
	}
	if (!data.has("failures")) {
		data["failures"] = Array();
	}
	if (!data.has("session_notes")) {
		data["session_notes"] = Array();
	}
	if (!data.has("runs")) {
		data["runs"] = Array();
	}
}

bool AIOSAgentMemory::_save() const {
	const String dir = memory_path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	Ref<FileAccess> file = FileAccess::open(memory_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_string(JSON::stringify(data, "  "));
	file->close();
	return true;
}

String AIOSAgentMemory::format_for_prompt() const {
	_ensure_loaded();
	Array conventions = data["conventions"];
	Array failures = data["failures"];
	Array notes = data["session_notes"];
	if (conventions.is_empty() && failures.is_empty() && notes.is_empty()) {
		return String();
	}

	String out = "## Project memory (from prior sessions)\n\n";
	if (!conventions.is_empty()) {
		out += "Conventions to follow:\n";
		for (int i = 0; i < conventions.size(); i++) {
			out += "- " + String(conventions[i]) + "\n";
		}
		out += "\n";
	}
	if (!failures.is_empty()) {
		out += "Recent failures — do not repeat these approaches:\n";
		const int start = MAX(0, failures.size() - 8);
		for (int i = start; i < failures.size(); i++) {
			Dictionary f = failures[i];
			out += "- [" + String(f.get("tool", "")) + "] " + String(f.get("message", ""));
			const String resolution = String(f.get("resolution", ""));
			if (!resolution.is_empty()) {
				out += " → " + resolution;
			}
			out += "\n";
		}
		out += "\n";
	}
	if (!notes.is_empty()) {
		out += "Session notes:\n";
		const int start = MAX(0, notes.size() - 5);
		for (int i = start; i < notes.size(); i++) {
			out += "- " + String(notes[i]) + "\n";
		}
		out += "\n";
	}
	return out;
}

void AIOSAgentMemory::add_convention(const String &p_note) {
	if (p_note.strip_edges().is_empty()) {
		return;
	}
	_ensure_loaded();
	Array conventions = data["conventions"];
	conventions.push_back(p_note);
	while (conventions.size() > MAX_CONVENTIONS) {
		conventions.remove_at(0);
	}
	data["conventions"] = conventions;
	_save();
}

void AIOSAgentMemory::add_failure(const String &p_tool, const String &p_message, const String &p_resolution) {
	if (p_message.strip_edges().is_empty()) {
		return;
	}
	_ensure_loaded();
	Dictionary entry;
	entry["at"] = Time::get_singleton()->get_unix_time_from_system();
	entry["tool"] = p_tool;
	entry["message"] = p_message;
	if (!p_resolution.is_empty()) {
		entry["resolution"] = p_resolution;
	}
	Array failures = data["failures"];
	failures.push_back(entry);
	while (failures.size() > MAX_FAILURES) {
		failures.remove_at(0);
	}
	data["failures"] = failures;
	_save();
}

void AIOSAgentMemory::add_session_note(const String &p_note) {
	if (p_note.strip_edges().is_empty()) {
		return;
	}
	_ensure_loaded();
	Array notes = data["session_notes"];
	notes.push_back(p_note);
	while (notes.size() > MAX_NOTES) {
		notes.remove_at(0);
	}
	data["session_notes"] = notes;
	_save();
}

void AIOSAgentMemory::record_run_finished(const Dictionary &p_summary) {
	_ensure_loaded();
	Dictionary run;
	run["at"] = Time::get_singleton()->get_unix_time_from_system();
	run["ok"] = p_summary.get("ok", false);
	run["steps"] = p_summary.get("steps_executed", 0);
	run["message"] = p_summary.get("message", "");
	Array runs = data["runs"];
	runs.push_back(run);
	while (runs.size() > 50) {
		runs.remove_at(0);
	}
	data["runs"] = runs;
	_save();
}
