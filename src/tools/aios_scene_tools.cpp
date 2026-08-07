/**************************************************************************/
/*  aios_scene_tools.cpp                                                  */
/**************************************************************************/

#include "aios_scene_tools.h"

#include "../util/aios_json.h"
#include "../world/aios_world_model.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                    */
/* -------------------------------------------------------------------------- */

Node *AIOSSceneTools::get_edited_root() {
	EditorInterface *ei = EditorInterface::get_singleton();
	return ei != nullptr ? ei->get_edited_scene_root() : nullptr;
}

void AIOSSceneTools::collect_owned_nodes(Node *p_node, Node *p_root, std::vector<Node *> &r_out) {
	if (p_node == nullptr) {
		return;
	}
	r_out.push_back(p_node);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (child != nullptr && child->get_owner() == p_root) {
			collect_owned_nodes(child, p_root, r_out);
		}
	}
}

bool AIOSSceneTools::is_valid_node_name(const String &p_name, String &r_reason) {
	if (p_name.strip_edges().is_empty()) {
		r_reason = "node names cannot be empty";
		return false;
	}
	// Mirrors the characters Godot itself strips in Node::set_name().
	const String forbidden = ".:@/\"%";
	for (int i = 0; i < forbidden.length(); i++) {
		if (p_name.contains(forbidden.substr(i, 1))) {
			r_reason = "node names cannot contain any of . : @ / \" %";
			return false;
		}
	}
	return true;
}

String AIOSSceneTools::unique_child_name(Node *p_parent, const String &p_base) {
	if (!p_parent->has_node(NodePath(p_base))) {
		return p_base;
	}
	for (int i = 2; i < 10000; i++) {
		const String candidate = p_base + String::num_int64(i);
		if (!p_parent->has_node(NodePath(candidate))) {
			return candidate;
		}
	}
	return p_base;
}

static void mark_dirty() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei != nullptr) {
		ei->mark_scene_as_unsaved();
	}
}

// Builds name -> {type, hint_string} for every settable property of a class,
// used both for validation and for the "did you mean" hints on typos.
static Dictionary class_property_types(const String &p_class) {
	Dictionary out;
	TypedArray<Dictionary> props = ClassDBSingleton::get_singleton()->class_get_property_list(p_class, false);
	for (int i = 0; i < props.size(); i++) {
		Dictionary p = props[i];
		const int64_t usage = p.has("usage") ? (int64_t)p["usage"] : 0;
		// Group/subgroup/category rows are inspector chrome, not properties.
		if (usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		Dictionary info;
		info["type"] = p.has("type") ? (int)(int64_t)p["type"] : 0;
		info["hint_string"] = p.has("hint_string") ? String(p["hint_string"]) : String();
		out[String(p["name"])] = info;
	}
	return out;
}

static Array closest_property_names(const Dictionary &p_known, const String &p_needle) {
	Array suggestions;
	Array keys = p_known.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		if (key.similarity(p_needle) > 0.6f || key.contains(p_needle) || p_needle.contains(key)) {
			suggestions.push_back(key);
		}
		if (suggestions.size() >= 5) {
			break;
		}
	}
	return suggestions;
}

/* -------------------------------------------------------------------------- */
/*  create_node_safe                                                           */
/* -------------------------------------------------------------------------- */

Dictionary AIOSSceneTools::create_node_safe(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene",
				"No scene is currently open in the editor. Open a scene (or create one) before adding nodes.");
	}

	const String parent_path = AIOSJson::get_string(p_params, "parent", ".");
	Node *parent = AIOSWorldModel::resolve_node(root, parent_path);
	if (parent == nullptr) {
		return AIOSJson::error("parent_not_found",
				"No node at '" + parent_path + "' in the open scene. Call get_world_model to see valid paths.");
	}
	if (parent != root && parent->get_owner() != root) {
		return AIOSJson::error("parent_not_editable",
				"'" + parent_path + "' belongs to an instanced sub-scene. Edit that scene directly, or enable editable children first.");
	}

	const String instance_scene = AIOSJson::get_string(p_params, "instance_scene", "");
	const String type = AIOSJson::get_string(p_params, "type", "");
	Ref<PackedScene> packed;

	if (!instance_scene.is_empty()) {
		if (!ResourceLoader::get_singleton()->exists(instance_scene)) {
			return AIOSJson::error("scene_not_found", "No scene at '" + instance_scene + "'.");
		}
		packed = ResourceLoader::get_singleton()->load(instance_scene);
		if (packed.is_null()) {
			return AIOSJson::error("scene_load_failed", "'" + instance_scene + "' could not be loaded as a PackedScene.");
		}
	} else {
		if (type.is_empty()) {
			return AIOSJson::error("missing_parameter", "Provide either 'type' (a Node class) or 'instance_scene' (a res:// path).");
		}
		if (!ClassDBSingleton::get_singleton()->class_exists(type)) {
			return AIOSJson::error("unknown_type", "'" + type + "' is not a class known to this Godot build.");
		}
		if (!ClassDBSingleton::get_singleton()->is_parent_class(type, "Node")) {
			return AIOSJson::error("not_a_node", "'" + type + "' is not a Node subclass, so it cannot be added to a scene tree.");
		}
		if (!ClassDBSingleton::get_singleton()->can_instantiate(type)) {
			return AIOSJson::error("abstract_type", "'" + type + "' is abstract or virtual and cannot be instantiated directly.");
		}
	}

	const String effective_class = instance_scene.is_empty() ? type : String("PackedScene instance");

	// --- name -------------------------------------------------------------
	String name = AIOSJson::get_string(p_params, "name", "");
	if (name.is_empty()) {
		name = instance_scene.is_empty() ? type : instance_scene.get_file().get_basename().capitalize().replace(" ", "");
	}
	String name_reason;
	if (!is_valid_node_name(name, name_reason)) {
		return AIOSJson::error("invalid_name", name_reason, p_params);
	}

	Array warnings;
	if (parent->has_node(NodePath(name))) {
		if (!AIOSJson::get_bool(p_params, "auto_rename", true)) {
			return AIOSJson::error("name_conflict",
					"'" + parent_path + "' already has a child named '" + name + "'. Pass a different name, or auto_rename: true.");
		}
		const String renamed = unique_child_name(parent, name);
		warnings.push_back("'" + name + "' was taken; used '" + renamed + "' instead.");
		name = renamed;
	}

	// --- properties (validated before anything is created) -----------------
	const Dictionary requested = AIOSJson::get_dict(p_params, "properties");
	Dictionary known;
	if (instance_scene.is_empty()) {
		known = class_property_types(type);
	}

	Dictionary resolved; // name -> coerced Variant
	Array property_errors;
	Array keys = requested.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];

		if (instance_scene.is_empty() && !known.has(key)) {
			Dictionary e;
			e["property"] = key;
			e["problem"] = "unknown property for " + type;
			Array suggestions = closest_property_names(known, key);
			if (suggestions.size() > 0) {
				e["did_you_mean"] = suggestions;
			}
			property_errors.push_back(e);
			continue;
		}

		int target_type = Variant::NIL;
		if (known.has(key)) {
			Dictionary info = known[key];
			target_type = (int)(int64_t)info["type"];
		}

		Variant coerced;
		String coerce_error;
		if (!AIOSJson::coerce(requested[key], target_type, coerced, coerce_error)) {
			Dictionary e;
			e["property"] = key;
			e["problem"] = coerce_error;
			property_errors.push_back(e);
			continue;
		}
		resolved[key] = coerced;
	}

	if (property_errors.size() > 0) {
		Dictionary details;
		details["errors"] = property_errors;
		return AIOSJson::error("invalid_properties",
				"One or more properties were rejected; nothing was created.", details);
	}

	// --- dry run -----------------------------------------------------------
	Dictionary plan;
	plan["parent"] = AIOSWorldModel::node_path_in_scene(root, parent);
	plan["name"] = name;
	plan["type"] = effective_class;
	if (!instance_scene.is_empty()) {
		plan["instance_scene"] = instance_scene;
	}
	Array applied_names = resolved.keys();
	plan["properties"] = applied_names;
	plan["warnings"] = warnings;

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		plan["dry_run"] = true;
		plan["would_create"] = (parent == root ? String(".") : String(root->get_path_to(parent))) + "/" + name;
		return AIOSJson::ok(plan);
	}

	// --- create ------------------------------------------------------------
	Node *node = nullptr;
	if (packed.is_valid()) {
		node = packed->instantiate();
	} else {
		Variant created = ClassDBSingleton::get_singleton()->instantiate(type);
		node = Object::cast_to<Node>(created);
	}
	if (node == nullptr) {
		return AIOSJson::error("instantiation_failed", "The engine refused to instantiate '" + effective_class + "'.");
	}

	node->set_name(name);

	Array resolved_keys = resolved.keys();
	Array applied;
	for (int i = 0; i < resolved_keys.size(); i++) {
		const String key = resolved_keys[i];
		node->set(key, resolved[key]);
		applied.push_back(key);
	}

	parent->add_child(node);
	// Without an owner the node exists at runtime but is never written to the
	// .tscn, which is the single most common way agent-built scenes "vanish".
	node->set_owner(root);

	const int64_t index = AIOSJson::get_int(p_params, "index", -1);
	if (index >= 0 && index < parent->get_child_count()) {
		parent->move_child(node, (int)index);
	}

	if (AIOSJson::get_bool(p_params, "unique_name_in_owner", false)) {
		node->set_unique_name_in_owner(true);
	}

	Array groups = AIOSJson::get_array(p_params, "groups");
	for (int i = 0; i < groups.size(); i++) {
		node->add_to_group(StringName(String(groups[i])), true);
	}

	mark_dirty();

	Dictionary result;
	result["path"] = AIOSWorldModel::node_path_in_scene(root, node);
	result["name"] = String(node->get_name());
	result["type"] = node->get_class();
	result["parent"] = AIOSWorldModel::node_path_in_scene(root, parent);
	result["applied_properties"] = applied;
	result["index"] = node->get_index();
	result["warnings"] = warnings;
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  attach_script_safe                                                         */
/* -------------------------------------------------------------------------- */

static String default_script_source(const String &p_base_class, const String &p_class_name) {
	String src = "extends " + p_base_class + "\n\n";
	if (!p_class_name.is_empty()) {
		src = "class_name " + p_class_name + "\nextends " + p_base_class + "\n\n";
	}
	src += "func _ready() -> void:\n\tpass\n";
	return src;
}

// Returns the class name on the script's `extends` line, or "" if absent.
static String parse_extends(const String &p_source) {
	PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = String(lines[i]).strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("@") || line.begins_with("class_name")) {
			continue;
		}
		if (line.begins_with("extends ")) {
			return line.substr(8).strip_edges();
		}
		// `extends` must precede any other statement, so once we see real code
		// there is nothing left to find.
		return String();
	}
	return String();
}

Dictionary AIOSSceneTools::attach_script_safe(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "This tool only works inside the Godot editor.");
	}

	Node *root = get_edited_root();
	const String node_path = AIOSJson::get_string(p_params, "node", "");
	Node *node = nullptr;

	if (!node_path.is_empty()) {
		if (root == nullptr) {
			return AIOSJson::error("no_open_scene", "No scene is open, so there is no node to attach a script to.");
		}
		node = AIOSWorldModel::resolve_node(root, node_path);
		if (node == nullptr) {
			return AIOSJson::error("node_not_found", "No node at '" + node_path + "' in the open scene.");
		}
		if (node != root && node->get_owner() != root) {
			return AIOSJson::error("node_not_editable",
					"'" + node_path + "' belongs to an instanced sub-scene and cannot be modified from here.");
		}
	}

	// --- destination path --------------------------------------------------
	String script_path = AIOSJson::get_string(p_params, "path", "");
	if (script_path.is_empty()) {
		const String base = node != nullptr ? String(node->get_name()) : String("script");
		script_path = "res://scripts/" + base.to_snake_case() + ".gd";
	}
	if (!script_path.begins_with("res://")) {
		return AIOSJson::error("invalid_path", "Script paths must live inside the project and start with res://.");
	}
	if (script_path.get_extension().to_lower() != "gd") {
		return AIOSJson::error("invalid_path", "Only GDScript (.gd) files are supported by this tool.");
	}

	const bool exists = FileAccess::file_exists(script_path);
	const bool overwrite = AIOSJson::get_bool(p_params, "overwrite", false);
	const bool has_source = p_params.has("source") && !String(p_params["source"]).strip_edges().is_empty();

	if (exists && has_source && !overwrite) {
		return AIOSJson::error("file_exists",
				"'" + script_path + "' already exists. Pass overwrite: true to replace it, or omit 'source' to attach the existing file.");
	}

	Array warnings;

	// --- source ------------------------------------------------------------
	String source;
	if (has_source) {
		source = String(p_params["source"]);
		if (!source.ends_with("\n")) {
			source += "\n";
		}
	} else if (!exists) {
		String base_class = AIOSJson::get_string(p_params, "base_class", "");
		if (base_class.is_empty()) {
			base_class = node != nullptr ? node->get_class() : String("Node");
		}
		source = default_script_source(base_class, AIOSJson::get_string(p_params, "class_name", ""));
	}

	// --- extends compatibility --------------------------------------------
	if (node != nullptr && !source.is_empty()) {
		const String extends_class = parse_extends(source);
		if (extends_class.is_empty()) {
			warnings.push_back("Script has no `extends` line; Godot will treat it as extending RefCounted, which cannot be attached to a node.");
		} else if (!extends_class.begins_with("\"") && !extends_class.begins_with("res://")) {
			if (ClassDBSingleton::get_singleton()->class_exists(extends_class)) {
				if (!ClassDBSingleton::get_singleton()->is_parent_class(node->get_class(), extends_class)) {
					return AIOSJson::error("incompatible_base",
							"The script extends " + extends_class + " but '" + String(node->get_name()) +
									"' is a " + node->get_class() + ". Attaching it would break the node.");
				}
			} else {
				// Could be a class_name from another script; we cannot resolve
				// that cheaply here, so flag it rather than guess.
				warnings.push_back("Could not verify that `extends " + extends_class + "` is compatible with " + node->get_class() + ".");
			}
		}
	}

	if (node != nullptr) {
		Ref<Script> current = node->get_script();
		if (current.is_valid() && !AIOSJson::get_bool(p_params, "replace", false)) {
			return AIOSJson::error("script_already_attached",
					"'" + String(node->get_name()) + "' already has " + current->get_path() +
							" attached. Pass replace: true if you really mean to swap it.");
		}
	}

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		Dictionary plan;
		plan["dry_run"] = true;
		plan["script"] = script_path;
		plan["node"] = node != nullptr ? AIOSWorldModel::node_path_in_scene(root, node) : String();
		plan["would_write"] = !source.is_empty();
		plan["would_overwrite"] = exists && !source.is_empty();
		plan["warnings"] = warnings;
		return AIOSJson::ok(plan);
	}

	// --- write -------------------------------------------------------------
	bool created = false;
	if (!source.is_empty()) {
		const String dir = script_path.get_base_dir();
		if (!DirAccess::dir_exists_absolute(dir)) {
			if (DirAccess::make_dir_recursive_absolute(dir) != OK) {
				return AIOSJson::error("mkdir_failed", "Could not create directory '" + dir + "'.");
			}
		}
		Ref<FileAccess> file = FileAccess::open(script_path, FileAccess::WRITE);
		if (file.is_null()) {
			return AIOSJson::error("write_failed",
					"Could not open '" + script_path + "' for writing (error " + String::num_int64(FileAccess::get_open_error()) + ").");
		}
		file->store_string(source);
		file->close();
		created = !exists;

		EditorFileSystem *efs = ei->get_resource_filesystem();
		if (efs != nullptr) {
			efs->update_file(script_path);
		}
	}

	// --- load & attach -----------------------------------------------------
	Ref<Script> script = ResourceLoader::get_singleton()->load(script_path, "Script", ResourceLoader::CACHE_MODE_REPLACE);

	// A non-null Script is not a working one: ResourceLoader happily returns a
	// GDScript object whose body failed to parse, and attaching that to a node
	// produces a scene that breaks only at runtime. reload() is the call that
	// actually compiles it and tells us.
	// can_instantiate() is deliberately NOT part of this check: the editor
	// disables script instantiation, so it returns false for every non-@tool
	// script. reload() is the call that actually compiles the source.
	const bool parsed = script.is_valid() && script->reload(false) == OK;
	if (!parsed) {
		if (created) {
			// We made the mess, we clean it up: leaving a broken .gd behind
			// would poison the next scan of the project.
			DirAccess::remove_absolute(script_path);
			EditorFileSystem *efs = ei->get_resource_filesystem();
			if (efs != nullptr) {
				efs->update_file(script_path);
			}
		}
		return AIOSJson::error("script_load_failed",
				"'" + script_path + "' does not compile as GDScript. The parser error is in the Godot output panel. " +
						String(created ? "The file was removed again." : "The file on disk was left as-is."));
	}

	if (node != nullptr) {
		node->set_script(script);
		mark_dirty();
	}

	Dictionary result;
	result["script"] = script_path;
	result["created"] = created;
	result["attached"] = node != nullptr;
	if (node != nullptr) {
		result["node"] = AIOSWorldModel::node_path_in_scene(root, node);
	}
	result["warnings"] = warnings;
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  safe_delete_node                                                           */
/* -------------------------------------------------------------------------- */

// Signal bookkeeping the editor and engine install on every node (SceneTreeEditor,
// Viewport, Control layout) vastly outnumbers the connections a designer made.
// The distinction Godot itself draws is CONNECT_PERSIST: only connections
// authored in the Node dock are flagged persistent and written to the .tscn.
// Everything else is runtime plumbing that will be rebuilt anyway.
static bool is_authored_connection(const Dictionary &p_connection) {
	if (!p_connection.has("flags")) {
		return false;
	}
	return ((int64_t)p_connection["flags"] & Object::CONNECT_PERSIST) != 0;
}

static Node *node_in_scene(Object *p_object, Node *p_root) {
	Node *node = Object::cast_to<Node>(p_object);
	if (node == nullptr || p_root == nullptr) {
		return nullptr;
	}
	if (node == p_root || node->get_owner() == p_root) {
		return node;
	}
	return nullptr;
}

// Target of an outgoing connection, or null if it is not in this scene.
static Node *connection_peer(const Dictionary &p_connection, Node *p_root) {
	if (!p_connection.has("callable")) {
		return nullptr;
	}
	Callable callable = p_connection["callable"];
	return node_in_scene(callable.get_object(), p_root);
}

// Emitter of an incoming connection, or null if it is not in this scene.
static Node *connection_source(const Dictionary &p_connection, Node *p_root) {
	if (p_connection.has("signal")) {
		Signal sig = p_connection["signal"];
		Node *from = node_in_scene(sig.get_object(), p_root);
		if (from != nullptr) {
			return from;
		}
	}
	if (p_connection.has("source")) {
		return node_in_scene(Object::cast_to<Object>(p_connection["source"]), p_root);
	}
	return nullptr;
}

// Scans project scripts for textual references to a node. This is a heuristic —
// GDScript can build node paths at runtime — but it catches the overwhelmingly
// common $Path / %Unique / get_node("Path") forms that silently break at
// runtime rather than at parse time.
static Array find_script_references(const String &p_node_name, const String &p_node_path, bool p_unique_name) {
	Array hits;

	PackedStringArray extensions;
	extensions.push_back("gd");
	PackedStringArray scripts;
	AIOSWorldModel::scan_files("res://", extensions, scripts, 400);

	PackedStringArray needles;
	needles.push_back("$" + p_node_name);
	needles.push_back("\"" + p_node_name + "\"");
	needles.push_back("'" + p_node_name + "'");
	needles.push_back("\"" + p_node_path + "\"");
	if (p_unique_name) {
		needles.push_back("%" + p_node_name);
	}

	for (int i = 0; i < scripts.size(); i++) {
		const String path = scripts[i];
		const String text = FileAccess::get_file_as_string(path);
		if (text.is_empty()) {
			continue;
		}
		PackedStringArray lines = text.split("\n");
		for (int line_no = 0; line_no < lines.size(); line_no++) {
			const String line = lines[line_no];
			for (int n = 0; n < needles.size(); n++) {
				if (line.contains(needles[n])) {
					Dictionary hit;
					hit["file"] = path;
					hit["line"] = line_no + 1;
					hit["text"] = line.strip_edges();
					hit["matched"] = needles[n];
					hits.push_back(hit);
					break;
				}
			}
			if (hits.size() >= 50) {
				return hits;
			}
		}
	}
	return hits;
}

Dictionary AIOSSceneTools::safe_delete_node(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is open in the editor.");
	}

	const String node_path = AIOSJson::get_string(p_params, "node", "");
	if (node_path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'node' is required.");
	}

	Node *node = AIOSWorldModel::resolve_node(root, node_path);
	if (node == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + node_path + "' in the open scene.");
	}

	Array blockers;
	Array warnings;

	auto blocker = [&blockers](const String &p_code, const String &p_message, const Variant &p_data) {
		Dictionary d;
		d["code"] = p_code;
		d["message"] = p_message;
		if (p_data.get_type() != Variant::NIL) {
			d["data"] = p_data;
		}
		blockers.push_back(d);
	};
	auto warn = [&warnings](const String &p_code, const String &p_message, const Variant &p_data) {
		Dictionary d;
		d["code"] = p_code;
		d["message"] = p_message;
		if (p_data.get_type() != Variant::NIL) {
			d["data"] = p_data;
		}
		warnings.push_back(d);
	};

	// --- structural blockers ----------------------------------------------
	if (node == root) {
		blocker("is_scene_root", "This is the scene root; deleting it would empty the scene. Close or replace the scene instead.", Variant());
	}
	if (node != root && node->get_owner() != root) {
		blocker("not_editable", "This node comes from an instanced sub-scene and is not owned by the open scene.", Variant());
	}

	// --- descendants -------------------------------------------------------
	std::vector<Node *> subtree;
	collect_owned_nodes(node, root, subtree);
	if (subtree.size() > 1) {
		Array descendants;
		for (size_t i = 1; i < subtree.size(); i++) {
			descendants.push_back(AIOSWorldModel::node_path_in_scene(root, subtree[i]));
		}
		warn("has_children", vformat("%d descendant node(s) will be deleted with it.", (int)descendants.size()), descendants);
	}

	// --- attached scripts --------------------------------------------------
	Array scripts_lost;
	for (size_t i = 0; i < subtree.size(); i++) {
		Ref<Script> s = subtree[i]->get_script();
		if (s.is_valid()) {
			Dictionary d;
			d["node"] = AIOSWorldModel::node_path_in_scene(root, subtree[i]);
			d["script"] = s->get_path();
			scripts_lost.push_back(d);
		}
	}
	if (scripts_lost.size() > 0) {
		warn("scripts_detached", "Scripts attached to the deleted subtree will be orphaned (the .gd files stay on disk).", scripts_lost);
	}

	// --- signal connections -------------------------------------------------
	Array connections;
	for (size_t i = 0; i < subtree.size(); i++) {
		Node *n = subtree[i];
		TypedArray<Dictionary> signal_list = n->get_signal_list();
		for (int s = 0; s < signal_list.size(); s++) {
			Dictionary sig = signal_list[s];
			TypedArray<Dictionary> conns = n->get_signal_connection_list(StringName(String(sig["name"])));
			for (int c = 0; c < conns.size(); c++) {
				Dictionary conn = conns[c];
				if (!is_authored_connection(conn)) {
					continue;
				}
				Node *peer = connection_peer(conn, root);
				if (peer == nullptr) {
					continue;
				}
				Dictionary d;
				d["direction"] = "outgoing";
				d["from"] = AIOSWorldModel::node_path_in_scene(root, n);
				d["signal"] = sig["name"];
				d["to"] = AIOSWorldModel::node_path_in_scene(root, peer);
				d["method"] = conn.has("callable") ? String(Callable(conn["callable"]).get_method()) : String();
				connections.push_back(d);
			}
		}
		TypedArray<Dictionary> incoming = n->get_incoming_connections();
		for (int c = 0; c < incoming.size(); c++) {
			Dictionary conn = incoming[c];
			if (!is_authored_connection(conn)) {
				continue;
			}
			Node *peer = connection_source(conn, root);
			if (peer == nullptr) {
				continue;
			}
			Dictionary d;
			d["direction"] = "incoming";
			d["to"] = AIOSWorldModel::node_path_in_scene(root, n);
			d["signal"] = conn.has("signal") ? String(conn["signal"]) : String();
			d["from"] = AIOSWorldModel::node_path_in_scene(root, peer);
			connections.push_back(d);
		}
	}
	if (connections.size() > 0) {
		warn("signal_connections", vformat("%d signal connection(s) reference this subtree and will be dropped.", (int)connections.size()), connections);
	}

	// --- NodePath properties pointing into the subtree ----------------------
	std::vector<Node *> all_nodes;
	collect_owned_nodes(root, root, all_nodes);

	Array nodepath_refs;
	for (size_t i = 0; i < all_nodes.size(); i++) {
		Node *other = all_nodes[i];
		bool in_subtree = false;
		for (size_t s = 0; s < subtree.size(); s++) {
			if (subtree[s] == other) {
				in_subtree = true;
				break;
			}
		}
		if (in_subtree) {
			continue;
		}

		TypedArray<Dictionary> props = other->get_property_list();
		for (int p = 0; p < props.size(); p++) {
			Dictionary prop = props[p];
			if ((int)(int64_t)prop["type"] != Variant::NODE_PATH) {
				continue;
			}
			const String prop_name = prop["name"];
			NodePath np = other->get(prop_name);
			if (np.is_empty()) {
				continue;
			}
			Node *target = other->get_node_or_null(np);
			if (target == nullptr) {
				continue;
			}
			for (size_t s = 0; s < subtree.size(); s++) {
				if (subtree[s] == target) {
					Dictionary d;
					d["node"] = AIOSWorldModel::node_path_in_scene(root, other);
					d["property"] = prop_name;
					d["points_to"] = AIOSWorldModel::node_path_in_scene(root, target);
					nodepath_refs.push_back(d);
					break;
				}
			}
		}
	}
	if (nodepath_refs.size() > 0) {
		blocker("referenced_by_nodepath",
				"Other nodes in this scene hold NodePath properties pointing into the subtree; deleting it would leave them dangling.",
				nodepath_refs);
	}

	// --- script text references --------------------------------------------
	if (AIOSJson::get_bool(p_params, "check_scripts", true)) {
		Array refs = find_script_references(String(node->get_name()),
				AIOSWorldModel::node_path_in_scene(root, node),
				node->is_unique_name_in_owner());
		if (refs.size() > 0) {
			warn("referenced_in_script", "Project scripts mention this node by name; those lookups will fail at runtime.", refs);
		}
	}

	// --- groups -------------------------------------------------------------
	TypedArray<StringName> groups = node->get_groups();
	Array group_names;
	for (int i = 0; i < groups.size(); i++) {
		const String group = String(groups[i]);
		if (group.begins_with("_")) {
			continue; // Editor-internal bookkeeping group.
		}
		group_names.push_back(group);
	}
	if (group_names.size() > 0) {
		warn("in_groups", "The node is a member of groups that other code may query.", group_names);
	}

	// --- report / execute ---------------------------------------------------
	Dictionary report;
	report["node"] = AIOSWorldModel::node_path_in_scene(root, node);
	report["type"] = node->get_class();
	report["descendants"] = (int)subtree.size() - 1;
	report["blockers"] = blockers;
	report["warnings"] = warnings;
	report["safe"] = blockers.is_empty();

	const bool force = AIOSJson::get_bool(p_params, "force", false);
	const bool dry_run = AIOSJson::get_bool(p_params, "dry_run", false);

	if (dry_run) {
		report["dry_run"] = true;
		report["deleted"] = false;
		return AIOSJson::ok(report);
	}

	if (!blockers.is_empty() && !force) {
		Dictionary details = report;
		return AIOSJson::error("unsafe_delete",
				vformat("%d blocking issue(s) found. Resolve them, or repeat the call with force: true if you accept the consequences.", (int)blockers.size()),
				details);
	}

	Node *parent = node->get_parent();
	if (parent == nullptr) {
		return AIOSJson::error("no_parent", "The node has no parent and cannot be removed from the tree.");
	}
	parent->remove_child(node);
	node->queue_free();
	mark_dirty();

	report["deleted"] = true;
	report["forced"] = force && !blockers.is_empty();
	return AIOSJson::ok(report);
}

/* -------------------------------------------------------------------------- */
/*  save_scene                                                                 */
/* -------------------------------------------------------------------------- */

Dictionary AIOSSceneTools::save_scene(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "This tool only works inside the Godot editor.");
	}
	Node *root = ei->get_edited_scene_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is open in the editor.");
	}

	const String save_as = AIOSJson::get_string(p_params, "path", "");
	if (!save_as.is_empty()) {
		if (!save_as.begins_with("res://")) {
			return AIOSJson::error("invalid_path", "Scene paths must start with res://.");
		}
		const String dir = save_as.get_base_dir();
		if (!DirAccess::dir_exists_absolute(dir) && DirAccess::make_dir_recursive_absolute(dir) != OK) {
			return AIOSJson::error("mkdir_failed", "Could not create directory '" + dir + "'.");
		}
		ei->save_scene_as(save_as, true);
		Dictionary result;
		result["path"] = save_as;
		return AIOSJson::ok(result);
	}

	if (root->get_scene_file_path().is_empty()) {
		return AIOSJson::error("scene_never_saved",
				"This scene has never been saved, so it has no path. Call save_scene with an explicit 'path' first.");
	}

	const Error err = ei->save_scene();
	if (err != OK) {
		return AIOSJson::error("save_failed", vformat("The editor could not save the scene (error %d).", (int)err));
	}

	Dictionary result;
	result["path"] = root->get_scene_file_path();
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  open_scene                                                                 */
/* -------------------------------------------------------------------------- */

Dictionary AIOSSceneTools::open_scene(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "This tool only works inside the Godot editor.");
	}

	const String path = AIOSJson::get_string(p_params, "path", "");
	if (path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'path' is required.");
	}
	if (!FileAccess::file_exists(path)) {
		return AIOSJson::error("scene_not_found", "No file at '" + path + "'.");
	}

	ei->open_scene_from_path(path, AIOSJson::get_bool(p_params, "inherited", false));

	// The editor swaps the edited scene synchronously, but a failed load leaves
	// the previous scene in place — so report what actually ended up open.
	Node *root = ei->get_edited_scene_root();
	Dictionary result;
	result["requested"] = path;
	result["current_scene"] = root != nullptr ? root->get_scene_file_path() : String();
	result["opened"] = root != nullptr && root->get_scene_file_path() == path;
	if (!(bool)result["opened"]) {
		result["note"] = "The editor did not switch to this scene; check the Output panel for a load error.";
	}
	return AIOSJson::ok(result);
}
