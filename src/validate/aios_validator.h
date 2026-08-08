/**************************************************************************/
/*  aios_validator.h                                                      */
/*  Pre-execution static checks: the Validate stage of the pipeline.      */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The Validate stage exists because of an asymmetry: a bad edit costs a
// playtest cycle (tens of seconds, plus a rollback, plus a confused repair
// attempt), while checking the same edit costs a millisecond. Catching it here
// is three orders of magnitude cheaper than catching it in Observe.
//
// Everything in this file is side-effect free. In particular a script is
// compiled *in memory* — GDScript is instantiable, so set_source_code() +
// reload() gives real parser diagnostics without a file ever reaching disk.
// That matters: a broken .gd written to res:// poisons the editor's script
// cache and shows up in the FileSystem dock even if we delete it a moment later.
//
// Findings are graded, and the grading is the useful part:
//   error   — will not work. Blocks execution.
//   warning — suspicious. Reported, does not block.
//   info    — worth knowing.
class AIOSValidator : public RefCounted {
	GDCLASS(AIOSValidator, RefCounted)

private:
	static void _add(Array &r_findings, const String &p_severity, const String &p_code,
			const String &p_message, const Dictionary &p_data = Dictionary());

	// Extracts the node paths a GDScript refers to: $Literal, $"quoted",
	// %Unique, get_node("path"), get_node_or_null("path").
	static Array _extract_node_references(const String &p_source);

protected:
	static void _bind_methods();

public:
	// Compiles GDScript in memory. Returns {ok, findings[], extends, class_name}.
	// Never writes anything.
	static Dictionary validate_script_source(const String &p_source, const String &p_target_class);

	// Checks that every node path a script mentions resolves against a node in
	// the open scene. Heuristic by nature — GDScript can build paths at runtime
	// — so unresolved paths are warnings, not errors.
	static Dictionary validate_node_references(const String &p_source, const String &p_attach_path);

	// Validates a property bag against a class without instantiating anything.
	static Dictionary validate_properties(const String &p_class, const Dictionary &p_properties);

	// Whole-scene sweep: dangling NodePath exports, missing resource files,
	// scripts that failed to load, nodes with no owner. This is the check that
	// runs after Execute and before Observe, because it catches the damage a
	// sequence of individually-valid edits can do.
	static Dictionary validate_scene();

	// Dispatches a planned tool call to the matching checks above without
	// executing it. This is what the pipeline calls for each step of a plan.
	static Dictionary validate_planned_call(const String &p_tool, const Dictionary &p_params);
};
