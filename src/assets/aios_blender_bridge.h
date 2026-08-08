/**************************************************************************/
/*  aios_blender_bridge.h                                                 */
/*  Headless Blender as a mesh-processing service.                        */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// Blender's role here is narrow and worth stating, because the obvious
// expectation — "the AI models things in Blender" — is the one thing this is
// not good at.
//
// An LLM writing bpy code produces decent *parametric* geometry (walls, stairs,
// platforms, modular kit pieces) and poor organic geometry. Nobody is getting a
// good rifle out of generated Python. So:
//
//   * for organic/detailed props, a text-to-3D service generates the mesh;
//   * Blender then cleans that mesh up, which is where it genuinely excels and
//     where the generated output genuinely needs help.
//
// The cleanup pass is the valuable half. A generated mesh arrives at an
// arbitrary scale, Z-up, with 200k triangles and no collision. Fixing that by
// hand is five minutes per asset; fixing it here is free and repeatable.
//
// Execution is synchronous but short (a decimate on one mesh is ~1-3 seconds),
// and it runs on a copy in a temp directory rather than on anything in res://,
// so a crashed Blender cannot leave a half-written file in the project.
class AIOSBlenderBridge : public RefCounted {
	GDCLASS(AIOSBlenderBridge, RefCounted)

private:
	static String _resolve_blender_executable();
	static String _script_path();
	static Dictionary _parse_result(const String &p_stdout);

protected:
	static void _bind_methods();

public:
	// Where Blender is, or "" if it could not be found.
	static String find_blender();
	static bool is_available();

	// Runs the cleanup pass over a mesh already in the project.
	// p_params: {path, target_triangles, collision, collision_triangles,
	//            scale_to, center_origin, merge_by_distance, smooth_angle,
	//            output (defaults to overwriting `path`)}
	static Dictionary cleanup_mesh(const Dictionary &p_params);

	// Runs an arbitrary bpy script. This is the procedural-generation path:
	// the agent writes Blender Python, we run it headless and import what it
	// exports. Deliberately separate from cleanup_mesh, which is a fixed,
	// reviewed pipeline rather than model-authored code.
	static Dictionary run_script(const Dictionary &p_params);
};
