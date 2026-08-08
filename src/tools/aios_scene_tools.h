/**************************************************************************/
/*  aios_scene_tools.h                                                    */
/*  Implementations of the mutating scene tools exposed to agents.        */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

using namespace godot;

// Every tool here follows the same contract:
//
//   * validate everything first, mutate nothing until all checks pass;
//   * support "dry_run": true, which runs the full validation and reports what
//     *would* happen — this is what makes an agent's plan step reviewable;
//   * return AIOSJson::ok()/error() envelopes, never raise or push_error, so a
//     misbehaving agent cannot fill the user's editor log with red text.
//
// The functions are static because they hold no state: the authoritative state
// is the live scene tree, and pretending otherwise is how caches go stale.
class AIOSSceneTools {
public:
	static Dictionary create_node_safe(const Dictionary &p_params);
	static Dictionary attach_script_safe(const Dictionary &p_params);
	static Dictionary safe_delete_node(const Dictionary &p_params);
	static Dictionary save_scene(const Dictionary &p_params);
	static Dictionary open_scene(const Dictionary &p_params);

	// --- Milestone 3: editing an existing project rather than only adding to it
	static Dictionary set_node_properties(const Dictionary &p_params);
	static Dictionary create_scene(const Dictionary &p_params);
	static Dictionary reparent_node(const Dictionary &p_params);
	static Dictionary connect_signal_safe(const Dictionary &p_params);
	static Dictionary disconnect_signal_safe(const Dictionary &p_params);
	static Dictionary read_script(const Dictionary &p_params);
	static Dictionary patch_script(const Dictionary &p_params);

	// Shared helpers, also used by the world model and the tool registry.
	static Node *get_edited_root();
	static void collect_owned_nodes(Node *p_node, Node *p_root, std::vector<Node *> &r_out);
	static bool is_valid_node_name(const String &p_name, String &r_reason);
	static String unique_child_name(Node *p_parent, const String &p_base);
};
