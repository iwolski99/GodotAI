/**************************************************************************/
/*  aios_world_model.h                                                    */
/*  Lightweight, cached JSON view of the project the agent is editing.    */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The world model is the agent's read path. Re-parsing .tscn files on every
// question is both slow and wrong (the file on disk lags the editor's live
// tree), so we walk the *live* edited scene and cache the resulting JSON.
//
// Invalidation is deliberately coarse: any structural editor event bumps a
// revision counter and marks the cache dirty. Building the model for a scene
// with a few hundred nodes costs well under a millisecond, so a conservative
// "rebuild when in doubt" policy is cheaper than tracking fine-grained deltas
// and getting them subtly wrong.
class AIOSWorldModel : public RefCounted {
	GDCLASS(AIOSWorldModel, RefCounted)

private:
	Dictionary cache;
	String cache_key;
	bool dirty = true;
	int64_t revision = 0;
	double last_build_msec = 0.0;

	Dictionary _describe_node(Node *p_node, Node *p_scene_root, const Dictionary &p_opts, int p_depth);
	Dictionary _collect_properties(Node *p_node);
	Array _collect_signals(Node *p_node, Node *p_scene_root);
	// World-space position, rotation and bounds. Empty for non-spatial nodes.
	static Dictionary _describe_spatial(Node *p_node);
	Dictionary _describe_project();
	Dictionary _describe_editor();
	Dictionary _describe_filesystem(int p_max_entries);

protected:
	static void _bind_methods();

public:
	// Marks the cache stale and bumps the revision the agent sees.
	void invalidate();

	bool is_dirty() const { return dirty; }
	int64_t get_revision() const { return revision; }

	// Main entry point. See schemas/get_world_model.json for the parameters.
	Dictionary get_world_model(const Dictionary &p_params);

	// Resolves a path as written by an agent ("." / "UI/Start" / "/root/...")
	// against the edited scene root. Returns nullptr when not found.
	static Node *resolve_node(Node *p_scene_root, const String &p_path);

	// Path of p_node relative to the scene root, in the form agents send back.
	static String node_path_in_scene(Node *p_scene_root, Node *p_node);

	// Recursively lists res:// files with any of the given extensions.
	static void scan_files(const String &p_dir, const PackedStringArray &p_extensions, PackedStringArray &r_out, int p_max);
};
