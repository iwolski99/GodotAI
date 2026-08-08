/**************************************************************************/
/*  aios_blender_bridge.cpp                                               */
/**************************************************************************/

#include "aios_blender_bridge.h"

#include "../util/aios_json.h"
#include "aios_asset_pipeline.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#define AIOS_BLENDER_SCRIPT "res://addons/godot_ai_os/tools/blender/godot_asset_pipeline.py"
#define AIOS_BLENDER_TEMP "user://godot_ai_os/blender"
#define AIOS_RESULT_MARKER "GODOT_AI_OS_RESULT "

void AIOSBlenderBridge::_bind_methods() {
	ClassDB::bind_static_method("AIOSBlenderBridge", D_METHOD("find_blender"), &AIOSBlenderBridge::find_blender);
	ClassDB::bind_static_method("AIOSBlenderBridge", D_METHOD("is_available"), &AIOSBlenderBridge::is_available);
	ClassDB::bind_static_method("AIOSBlenderBridge", D_METHOD("cleanup_mesh", "params"), &AIOSBlenderBridge::cleanup_mesh);
	ClassDB::bind_static_method("AIOSBlenderBridge", D_METHOD("run_script", "params"), &AIOSBlenderBridge::run_script);
}

/* -------------------------------------------------------------------------- */
/*  Finding Blender                                                            */
/* -------------------------------------------------------------------------- */

// Same shape as the git discovery in aios_git_checkpoint.cpp, and for the same
// reason: on Windows the tool is almost never on PATH, and telling a user
// "Blender not found" when it is sitting in Program Files is a bad experience.
String AIOSBlenderBridge::_resolve_blender_executable() {
	OS *os = OS::get_singleton();

	const String env_override = os->get_environment("GODOT_AI_OS_BLENDER");
	if (!env_override.is_empty()) {
		return env_override;
	}

	// Godot has its own Blender path setting for .blend import; if the user has
	// already told the editor where Blender is, believe them.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("filesystem/import/blender/blender_path")) {
		const String godot_path = ps->get_setting("filesystem/import/blender/blender_path", "");
		if (!godot_path.strip_edges().is_empty()) {
			return godot_path;
		}
	}

	PackedStringArray probe;
	probe.push_back("--version");
	Array discard;
	if (os->execute("blender", probe, discard, false) == 0) {
		return "blender";
	}

	PackedStringArray candidates;
	const String os_name = os->get_name();

	if (os_name == "Windows") {
		const String program_files = os->get_environment("ProgramFiles");
		// Blender installs into a versioned directory, so these are the versions
		// current at the time of writing rather than a wildcard glob. The
		// environment override exists precisely for anything not listed.
		const char *versions[] = { "4.5", "4.4", "4.3", "4.2", "4.1", "4.0", "3.6" };
		for (int v = 0; v < 7; v++) {
			if (!program_files.is_empty()) {
				candidates.push_back(program_files.replace("\\", "/") +
						String("/Blender Foundation/Blender ") + String(versions[v]) + String("/blender.exe"));
			}
			candidates.push_back(String("C:/Program Files/Blender Foundation/Blender ") + String(versions[v]) + String("/blender.exe"));
		}
	} else if (os_name == "macOS") {
		candidates.push_back("/Applications/Blender.app/Contents/MacOS/Blender");
		const String home = os->get_environment("HOME");
		if (!home.is_empty()) {
			candidates.push_back(home + String("/Applications/Blender.app/Contents/MacOS/Blender"));
		}
	} else {
		candidates.push_back("/usr/bin/blender");
		candidates.push_back("/usr/local/bin/blender");
		candidates.push_back("/snap/bin/blender");
		candidates.push_back("/var/lib/flatpak/exports/bin/org.blender.Blender");
	}

	for (int i = 0; i < candidates.size(); i++) {
		Array probe_output;
		if (os->execute(candidates[i], probe, probe_output, false) == 0) {
			return candidates[i];
		}
	}

	return String();
}

String AIOSBlenderBridge::find_blender() {
	return _resolve_blender_executable();
}

bool AIOSBlenderBridge::is_available() {
	return !_resolve_blender_executable().is_empty();
}

String AIOSBlenderBridge::_script_path() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (FileAccess::file_exists(AIOS_BLENDER_SCRIPT)) {
		return ps->globalize_path(AIOS_BLENDER_SCRIPT);
	}
	// Running from the plugin's own repository rather than an installed addon.
	const String local = "res://../tools/blender/godot_asset_pipeline.py";
	if (FileAccess::file_exists(local)) {
		return ps->globalize_path(local);
	}
	return String();
}

Dictionary AIOSBlenderBridge::_parse_result(const String &p_stdout) {
	// Blender prints a great deal before and after our line, so the marker is
	// searched for rather than the output being parsed as a whole.
	const int marker = p_stdout.rfind(AIOS_RESULT_MARKER);
	if (marker < 0) {
		return Dictionary();
	}
	String tail = p_stdout.substr(marker + String(AIOS_RESULT_MARKER).length());
	const int newline = tail.find("\n");
	if (newline >= 0) {
		tail = tail.substr(0, newline);
	}
	Variant parsed = JSON::parse_string(tail.strip_edges());
	return parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
}

/* -------------------------------------------------------------------------- */
/*  Cleanup                                                                    */
/* -------------------------------------------------------------------------- */

Dictionary AIOSBlenderBridge::cleanup_mesh(const Dictionary &p_params) {
	OS *os = OS::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();

	const String blender = _resolve_blender_executable();
	if (blender.is_empty()) {
		return AIOSJson::error("blender_not_found",
				"Blender could not be found. Install it, put it on PATH, set the GODOT_AI_OS_BLENDER "
				"environment variable to blender.exe, or set Project Settings > Filesystem > Import > "
				"Blender > Blender Path.");
	}

	const String script = _script_path();
	if (script.is_empty()) {
		return AIOSJson::error("script_missing",
				"The Blender pipeline script is missing. Expected it at " AIOS_BLENDER_SCRIPT ".");
	}

	const String path = AIOSJson::get_string(p_params, "path", "");
	if (path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'path' is required: the mesh to clean up.");
	}
	if (!FileAccess::file_exists(path)) {
		return AIOSJson::error("file_not_found", "No file at '" + path + "'.");
	}

	String output = AIOSJson::get_string(p_params, "output", "");
	if (output.is_empty()) {
		output = path;
	}
	if (!output.begins_with("res://")) {
		return AIOSJson::error("invalid_output", "'output' must be under res://.");
	}

	// Blender writes to a temp file first. Writing straight over the input means
	// a crash halfway through leaves the project holding a truncated mesh, and
	// the original is gone.
	if (!DirAccess::dir_exists_absolute(AIOS_BLENDER_TEMP)) {
		DirAccess::make_dir_recursive_absolute(AIOS_BLENDER_TEMP);
	}
	const String temp_out = String(AIOS_BLENDER_TEMP) + "/cleanup_" +
			String::num_uint64(Time::get_singleton()->get_ticks_msec()) + ".glb";

	PackedStringArray args;
	args.push_back("--background");
	args.push_back("--factory-startup"); // ignore user addons that could break the run
	args.push_back("--python");
	args.push_back(script);
	args.push_back("--");
	args.push_back("--input");
	args.push_back(ps->globalize_path(path));
	args.push_back("--output");
	args.push_back(ps->globalize_path(temp_out));

	const int64_t target_triangles = AIOSJson::get_int(p_params, "target_triangles", 0);
	if (target_triangles > 0) {
		args.push_back("--target-triangles");
		args.push_back(String::num_int64(target_triangles));
	}

	const String collision = AIOSJson::get_string(p_params, "collision", "none");
	args.push_back("--collision");
	args.push_back(collision);

	const int64_t collision_triangles = AIOSJson::get_int(p_params, "collision_triangles", 500);
	args.push_back("--collision-triangles");
	args.push_back(String::num_int64(collision_triangles));

	const double scale_to = (double)AIOSJson::get_int(p_params, "scale_to", 0);
	if (scale_to > 0.0) {
		args.push_back("--scale-to");
		args.push_back(String::num(scale_to, 4));
	}
	if (AIOSJson::get_bool(p_params, "center_origin", false)) {
		args.push_back("--center-origin");
	}
	const double merge = (double)AIOSJson::get_int(p_params, "merge_by_distance_micrometres", 0) / 1000000.0;
	if (merge > 0.0) {
		args.push_back("--merge-by-distance");
		args.push_back(String::num(merge, 8));
	}
	const int64_t smooth = AIOSJson::get_int(p_params, "smooth_angle", 0);
	if (smooth > 0) {
		args.push_back("--smooth-angle");
		args.push_back(String::num_int64(smooth));
	}

	// Blocking on purpose: a decimate on a single mesh is seconds, and the
	// alternative (a second async state machine) is not worth the complexity for
	// an operation a human triggers and waits for.
	Array output_lines;
	const int32_t exit_code = os->execute(blender, args, output_lines, true, false);

	String combined;
	for (int i = 0; i < output_lines.size(); i++) {
		combined += String(output_lines[i]) + "\n";
	}

	if (exit_code != 0) {
		Dictionary details;
		details["exit_code"] = exit_code;
		details["output_tail"] = combined.substr(combined.length() > 1200 ? combined.length() - 1200 : 0);
		return AIOSJson::error("blender_failed",
				"Blender exited with code " + String::num_int64(exit_code) + ". The tail of its output is in "
				"details; the input mesh was not modified.",
				details);
	}

	Dictionary report = _parse_result(combined);
	if (report.is_empty()) {
		Dictionary details;
		details["output_tail"] = combined.substr(combined.length() > 1200 ? combined.length() - 1200 : 0);
		return AIOSJson::error("no_result",
				"Blender ran but did not report a result. The pipeline script may be an incompatible version.",
				details);
	}

	if (!FileAccess::file_exists(temp_out)) {
		return AIOSJson::error("no_output", "Blender reported success but wrote no file.");
	}

	// Move the finished mesh into place, then import it.
	const String out_dir = output.get_base_dir();
	if (!DirAccess::dir_exists_absolute(out_dir)) {
		DirAccess::make_dir_recursive_absolute(out_dir);
	}

	Ref<FileAccess> src = FileAccess::open(temp_out, FileAccess::READ);
	if (src.is_null()) {
		return AIOSJson::error("read_failed", "Could not read Blender's output from '" + temp_out + "'.");
	}
	const PackedByteArray bytes = src->get_buffer(src->get_length());
	src->close();

	Ref<FileAccess> dst = FileAccess::open(output, FileAccess::WRITE);
	if (dst.is_null()) {
		return AIOSJson::error("write_failed", "Could not write '" + output + "'.");
	}
	dst->store_buffer(bytes);
	dst->close();
	DirAccess::remove_absolute(temp_out);

	AIOSAssetPipeline::import_file(output);

	Dictionary result;
	result["path"] = output;
	result["blender"] = blender;
	result["bytes"] = bytes.size();
	result["report"] = report;

	// Surface the numbers that matter at the top level, so an agent does not
	// have to dig into the nested report to see whether anything happened.
	if (report.has("triangles_in") && report.has("triangles_out")) {
		const int64_t before = (int64_t)report["triangles_in"];
		const int64_t after = (int64_t)report["triangles_out"];
		result["triangles_before"] = before;
		result["triangles_after"] = after;
		if (before > 0) {
			result["reduction_percent"] = (int)(100.0 - (100.0 * (double)after / (double)before));
		}
	}
	if (report.has("collision")) {
		result["collision_generated"] = true;
		result["collision_note"] = "Godot builds the collision body from the mesh name suffix on import.";
	}
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Arbitrary scripts                                                          */
/* -------------------------------------------------------------------------- */

Dictionary AIOSBlenderBridge::run_script(const Dictionary &p_params) {
	OS *os = OS::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();

	const String blender = _resolve_blender_executable();
	if (blender.is_empty()) {
		return AIOSJson::error("blender_not_found", "Blender could not be found.");
	}

	const String source = AIOSJson::get_string(p_params, "source", "");
	if (source.strip_edges().is_empty()) {
		return AIOSJson::error("missing_parameter", "'source' is required: the bpy script to run.");
	}

	// The script goes in user://, never res://. A generated .py inside the
	// project would be picked up by the FileSystem dock and committed.
	if (!DirAccess::dir_exists_absolute(AIOS_BLENDER_TEMP)) {
		DirAccess::make_dir_recursive_absolute(AIOS_BLENDER_TEMP);
	}
	const String script_path = String(AIOS_BLENDER_TEMP) + "/script_" +
			String::num_uint64(Time::get_singleton()->get_ticks_msec()) + ".py";

	Ref<FileAccess> file = FileAccess::open(script_path, FileAccess::WRITE);
	if (file.is_null()) {
		return AIOSJson::error("write_failed", "Could not stage the script at '" + script_path + "'.");
	}
	file->store_string(source);
	file->close();

	PackedStringArray args;
	args.push_back("--background");
	args.push_back("--factory-startup");
	args.push_back("--python");
	args.push_back(ps->globalize_path(script_path));

	// Anything the script should export goes here, passed through so the script
	// does not have to hardcode a path.
	const String output = AIOSJson::get_string(p_params, "output", "");
	if (!output.is_empty()) {
		if (!output.begins_with("res://")) {
			return AIOSJson::error("invalid_output", "'output' must be under res://.");
		}
		const String out_dir = output.get_base_dir();
		if (!DirAccess::dir_exists_absolute(out_dir)) {
			DirAccess::make_dir_recursive_absolute(out_dir);
		}
		args.push_back("--");
		args.push_back("--output");
		args.push_back(ps->globalize_path(output));
	}

	Array output_lines;
	const int32_t exit_code = os->execute(blender, args, output_lines, true, false);
	DirAccess::remove_absolute(script_path);

	String combined;
	for (int i = 0; i < output_lines.size(); i++) {
		combined += String(output_lines[i]) + "\n";
	}
	const String tail = combined.substr(combined.length() > 2000 ? combined.length() - 2000 : 0);

	if (exit_code != 0) {
		Dictionary details;
		details["exit_code"] = exit_code;
		details["output_tail"] = tail;
		return AIOSJson::error("blender_failed",
				"The Blender script exited with code " + String::num_int64(exit_code) +
						". Its traceback is in details.output_tail.",
				details);
	}

	Dictionary result;
	result["ran"] = true;
	result["blender"] = blender;
	result["output_tail"] = tail;

	Dictionary report = _parse_result(combined);
	if (!report.is_empty()) {
		result["report"] = report;
	}

	if (!output.is_empty()) {
		result["output"] = output;
		result["written"] = FileAccess::file_exists(output);
		if ((bool)result["written"]) {
			AIOSAssetPipeline::import_file(output);
		} else {
			result["note"] = "The script ran without error but wrote nothing to the requested output path.";
		}
	}
	return AIOSJson::ok(result);
}
