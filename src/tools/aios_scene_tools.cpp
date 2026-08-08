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
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
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

// Coerces a JSON property bag against a class's real property table.
//
// This is shared by create_node_safe and set_node_properties on purpose. The two
// tools disagreeing about whether `"position": [64, 32]` is acceptable would be a
// genuinely maddening bug to hit, so there is one implementation and both call it.
//
// p_known empty means "we cannot introspect this class" (a PackedScene instance,
// or a node whose script adds properties ClassDB does not list) — in that case
// unknown names are passed through untyped rather than rejected.
static bool coerce_property_bag(const Dictionary &p_requested,
		const Dictionary &p_known,
		const String &p_class_label,
		Dictionary &r_resolved,
		Array &r_errors) {
	const bool introspectable = !p_known.is_empty();
	Array keys = p_requested.keys();

	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];

		if (introspectable && !p_known.has(key)) {
			Dictionary e;
			e["property"] = key;
			e["problem"] = "unknown property for " + p_class_label;
			Array suggestions = closest_property_names(p_known, key);
			if (suggestions.size() > 0) {
				e["did_you_mean"] = suggestions;
			}
			r_errors.push_back(e);
			continue;
		}

		int target_type = Variant::NIL;
		if (p_known.has(key)) {
			Dictionary info = p_known[key];
			target_type = (int)(int64_t)info["type"];
		}

		Variant coerced;
		String coerce_error;
		if (!AIOSJson::coerce(p_requested[key], target_type, coerced, coerce_error)) {
			Dictionary e;
			e["property"] = key;
			e["problem"] = coerce_error;
			r_errors.push_back(e);
			continue;
		}
		r_resolved[key] = coerced;
	}

	return r_errors.is_empty();
}

// The property table for a node as it actually is, which is not the same as the
// table for its class: an attached script adds exported properties that ClassDB
// knows nothing about, and refusing to set those would make the tool useless on
// exactly the nodes an agent just built.
static Dictionary node_property_types(Node *p_node) {
	Dictionary out = class_property_types(p_node->get_class());

	TypedArray<Dictionary> props = p_node->get_property_list();
	for (int i = 0; i < props.size(); i++) {
		Dictionary p = props[i];
		const int64_t usage = p.has("usage") ? (int64_t)p["usage"] : 0;
		if (usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		if (!(usage & PROPERTY_USAGE_EDITOR) && !(usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		Dictionary info;
		info["type"] = p.has("type") ? (int)(int64_t)p["type"] : 0;
		info["hint_string"] = p.has("hint_string") ? String(p["hint_string"]) : String();
		out[String(p["name"])] = info;
	}
	return out;
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
	coerce_property_bag(requested, known, type, resolved, property_errors);

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
/*  read_script / patch_script                                                 */
/* -------------------------------------------------------------------------- */

// Until now the only way to change a script was attach_script_safe, which
// rewrites the whole file from whatever the model remembered. That is fine for a
// 20-line script and actively destructive for a 300-line one: anything the model
// did not recall is silently deleted.
//
// read_script and patch_script replace that with read-modify-write on a named
// region, so an edit to one function cannot lose another.
Dictionary AIOSSceneTools::read_script(const Dictionary &p_params) {
	String path = AIOSJson::get_string(p_params, "path", "");

	// Convenience: name a node instead of a path and get whatever script it has.
	if (path.is_empty()) {
		const String node_path = AIOSJson::get_string(p_params, "node", "");
		if (node_path.is_empty()) {
			return AIOSJson::error("missing_parameter", "Pass either 'path' (res://...) or 'node'.");
		}
		Node *root = get_edited_root();
		if (root == nullptr) {
			return AIOSJson::error("no_open_scene", "No scene is open, so 'node' cannot be resolved.");
		}
		Node *node = AIOSWorldModel::resolve_node(root, node_path);
		if (node == nullptr) {
			return AIOSJson::error("node_not_found", "No node at '" + node_path + "'.");
		}
		Ref<Script> script = node->get_script();
		if (script.is_null()) {
			return AIOSJson::error("no_script", "'" + node_path + "' has no script attached.");
		}
		path = script->get_path();
		if (path.is_empty()) {
			return AIOSJson::error("built_in_script",
					"'" + node_path + "' has a built-in script with no file on disk. This tool only reads .gd files.");
		}
	}

	if (!FileAccess::file_exists(path)) {
		return AIOSJson::error("file_not_found", "No file at '" + path + "'.");
	}

	const String source = FileAccess::get_file_as_string(path);
	PackedStringArray lines = source.split("\n");

	Dictionary result;
	result["path"] = path;
	result["line_count"] = lines.size();

	// An optional window, so reading one function out of a long file does not
	// cost the model the whole file in context.
	const int64_t from_line = AIOSJson::get_int(p_params, "from_line", 1);
	const int64_t to_line = AIOSJson::get_int(p_params, "to_line", 0);
	const int start = (int)(from_line > 1 ? from_line - 1 : 0);
	const int end = (int)(to_line > 0 && to_line < lines.size() ? to_line : lines.size());

	String windowed;
	for (int i = start; i < end; i++) {
		windowed += lines[i];
		if (i < end - 1) {
			windowed += "\n";
		}
	}
	result["source"] = windowed;
	result["from_line"] = start + 1;
	result["to_line"] = end;

	// The function index is the map patch_script edits against, so an agent can
	// go straight to "replace_function" without reading the body first.
	Array functions;
	for (int i = 0; i < lines.size(); i++) {
		const String stripped = lines[i].strip_edges();
		if (!stripped.begins_with("func ") && !stripped.begins_with("static func ")) {
			continue;
		}
		const int name_start = stripped.find("func ") + 5;
		const int paren = stripped.find("(", name_start);
		if (paren < 0) {
			continue;
		}
		Dictionary f;
		f["name"] = stripped.substr(name_start, paren - name_start).strip_edges();
		f["line"] = i + 1;
		f["signature"] = stripped;
		functions.push_back(f);
	}
	result["functions"] = functions;

	return AIOSJson::ok(result);
}

// Finds the [start, end) line range of a GDScript function, using indentation to
// find the end. GDScript is whitespace-delimited, so a function ends at the next
// line that has content at or below the `func` keyword's own indentation.
static bool find_function_range(const PackedStringArray &p_lines, const String &p_name, int &r_start, int &r_end) {
	r_start = -1;
	int base_indent = 0;

	for (int i = 0; i < p_lines.size(); i++) {
		const String line = p_lines[i];
		const String stripped = line.strip_edges();
		if (!stripped.begins_with("func ") && !stripped.begins_with("static func ")) {
			continue;
		}
		const int name_start = stripped.find("func ") + 5;
		const int paren = stripped.find("(", name_start);
		if (paren < 0) {
			continue;
		}
		if (stripped.substr(name_start, paren - name_start).strip_edges() != p_name) {
			continue;
		}
		r_start = i;
		base_indent = (int)(line.length() - line.lstrip("\t ").length());
		break;
	}

	if (r_start < 0) {
		return false;
	}

	r_end = p_lines.size();
	for (int i = r_start + 1; i < p_lines.size(); i++) {
		const String line = p_lines[i];
		if (line.strip_edges().is_empty()) {
			continue; // Blank lines belong to whatever follows them.
		}
		const int indent = (int)(line.length() - line.lstrip("\t ").length());
		if (indent <= base_indent) {
			r_end = i;
			break;
		}
	}

	// Trailing blank lines belong to the gap between functions, not the body.
	while (r_end > r_start + 1 && p_lines[r_end - 1].strip_edges().is_empty()) {
		r_end--;
	}
	return true;
}

Dictionary AIOSSceneTools::patch_script(const Dictionary &p_params) {
	const String path = AIOSJson::get_string(p_params, "path", "");
	if (path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'path' is required.");
	}
	if (!FileAccess::file_exists(path)) {
		return AIOSJson::error("file_not_found",
				"No file at '" + path + "'. Use attach_script_safe to create a new script.");
	}

	const String operation = AIOSJson::get_string(p_params, "operation", "");
	const String original = FileAccess::get_file_as_string(path);
	PackedStringArray lines = original.split("\n");

	String patched;
	Dictionary detail;

	if (operation == "replace_function") {
		const String function = AIOSJson::get_string(p_params, "function", "");
		const String body = AIOSJson::get_string(p_params, "source", "");
		if (function.is_empty() || body.is_empty()) {
			return AIOSJson::error("missing_parameter",
					"'replace_function' needs 'function' (the name) and 'source' (the complete replacement, including the func line).");
		}
		int start = 0;
		int end = 0;
		if (!find_function_range(lines, function, start, end)) {
			return AIOSJson::error("function_not_found",
					"No function named '" + function + "' in '" + path + "'. Call read_script to see what is there.");
		}
		String out;
		for (int i = 0; i < start; i++) {
			out += lines[i] + "\n";
		}
		out += body;
		if (!body.ends_with("\n")) {
			out += "\n";
		}
		for (int i = end; i < lines.size(); i++) {
			out += lines[i];
			if (i < lines.size() - 1) {
				out += "\n";
			}
		}
		patched = out;
		detail["replaced_lines"] = String::num_int64(start + 1) + "-" + String::num_int64(end);

	} else if (operation == "append") {
		const String body = AIOSJson::get_string(p_params, "source", "");
		if (body.is_empty()) {
			return AIOSJson::error("missing_parameter", "'append' needs 'source'.");
		}
		patched = original;
		if (!patched.ends_with("\n")) {
			patched += "\n";
		}
		patched += "\n" + body;
		if (!patched.ends_with("\n")) {
			patched += "\n";
		}
		detail["appended_at_line"] = lines.size();

	} else if (operation == "replace_text") {
		const String find = AIOSJson::get_string(p_params, "find", "");
		const String replace = AIOSJson::get_string(p_params, "replace", "");
		if (find.is_empty()) {
			return AIOSJson::error("missing_parameter", "'replace_text' needs 'find'.");
		}
		const int occurrences = original.count(find);
		if (occurrences == 0) {
			return AIOSJson::error("text_not_found",
					"'" + path + "' does not contain that text. Whitespace and indentation must match exactly — "
					"call read_script and copy the region verbatim.");
		}
		// Ambiguity is an error, not a coin flip: replacing the wrong one of
		// three identical blocks is the kind of bug that surfaces an hour later.
		if (occurrences > 1 && !AIOSJson::get_bool(p_params, "replace_all", false)) {
			Dictionary details;
			details["occurrences"] = occurrences;
			return AIOSJson::error("ambiguous_match",
					"That text appears " + String::num_int64(occurrences) + " times. Include more surrounding "
					"context to make it unique, or pass replace_all: true.",
					details);
		}
		patched = original.replace(find, replace);
		detail["occurrences"] = occurrences;

	} else {
		return AIOSJson::error("unknown_operation",
				"'operation' must be one of: replace_function, append, replace_text. Got '" + operation + "'.");
	}

	if (patched == original) {
		return AIOSJson::error("no_change",
				"The patch produced a file identical to the original. Nothing was written.");
	}

	// The whole point of this tool is that it cannot leave a broken script on
	// disk, so the result is compiled in memory before anything is written.
	Ref<Script> probe = ClassDBSingleton::get_singleton()->instantiate("GDScript");
	if (probe.is_valid()) {
		probe->set_source_code(patched);
		if (probe->reload(false) != OK) {
			Dictionary details;
			details["operation"] = operation;
			return AIOSJson::error("patch_would_not_compile",
					"The patched script does not compile, so it was NOT written — '" + path + "' is unchanged. "
					"The parser's message is in Godot's Output panel.",
					details);
		}
	}

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		Dictionary plan = detail;
		plan["dry_run"] = true;
		plan["path"] = path;
		plan["operation"] = operation;
		plan["compiles"] = true;
		plan["new_line_count"] = patched.split("\n").size();
		return AIOSJson::ok(plan);
	}

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return AIOSJson::error("write_failed", "Could not open '" + path + "' for writing.");
	}
	file->store_string(patched);
	file->close();

	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei != nullptr && ei->get_resource_filesystem() != nullptr) {
		ei->get_resource_filesystem()->update_file(path);
	}

	Dictionary result = detail;
	result["path"] = path;
	result["operation"] = operation;
	result["compiles"] = true;
	result["previous_line_count"] = lines.size();
	result["new_line_count"] = patched.split("\n").size();
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  connect_signal_safe / disconnect_signal_safe                               */
/* -------------------------------------------------------------------------- */

// Signals are how a Godot game is actually wired together: body_entered ->
// take_damage, pressed -> start_game, timeout -> spawn. An agent that cannot
// connect one can build a scene that looks right and does nothing.
//
// Connections are made with CONNECT_PERSIST, which is what makes Godot write
// them into the .tscn. Without that flag the connection exists until the scene
// reloads and then silently disappears — which would be a genuinely awful bug to
// track down, because everything looks correct in the editor right up until it
// does not.
Dictionary AIOSSceneTools::connect_signal_safe(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is currently open in the editor.");
	}

	const String from_path = AIOSJson::get_string(p_params, "from", "");
	const String to_path = AIOSJson::get_string(p_params, "to", "");
	const String signal_name = AIOSJson::get_string(p_params, "signal", "");
	const String method_name = AIOSJson::get_string(p_params, "method", "");

	if (from_path.is_empty() || to_path.is_empty() || signal_name.is_empty() || method_name.is_empty()) {
		return AIOSJson::error("missing_parameter",
				"'from', 'to', 'signal' and 'method' are all required.");
	}

	Node *from = AIOSWorldModel::resolve_node(root, from_path);
	if (from == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + from_path + "' (the signal emitter).");
	}
	Node *to = AIOSWorldModel::resolve_node(root, to_path);
	if (to == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + to_path + "' (the receiver).");
	}

	// --- does the signal exist? --------------------------------------------
	if (!from->has_signal(StringName(signal_name))) {
		Dictionary details;
		Array available;
		TypedArray<Dictionary> signals = from->get_signal_list();
		for (int i = 0; i < signals.size(); i++) {
			Dictionary s = signals[i];
			const String candidate = s["name"];
			if (candidate.similarity(signal_name) > 0.5f || candidate.contains(signal_name)) {
				available.push_back(candidate);
			}
		}
		if (available.size() > 0) {
			details["did_you_mean"] = available;
		}
		details["emitter_type"] = from->get_class();
		return AIOSJson::error("unknown_signal",
				"'" + from->get_class() + "' has no signal named '" + signal_name + "'.", details);
	}

	// --- does the receiver have the method? --------------------------------
	// A missing method is an error rather than a warning: Godot itself will
	// refuse the connection at load time, so allowing it here would just move
	// the failure somewhere less informative.
	Array warnings;
	if (!to->has_method(StringName(method_name))) {
		Ref<Script> script = to->get_script();
		Dictionary details;
		details["receiver_type"] = to->get_class();
		details["has_script"] = script.is_valid();
		if (script.is_null()) {
			return AIOSJson::error("no_receiver_method",
					"'" + to_path + "' has no method '" + method_name + "' and no script attached. "
					"Attach a script defining it with attach_script_safe first.",
					details);
		}
		return AIOSJson::error("no_receiver_method",
				"The script on '" + to_path + "' does not define '" + method_name + "'. "
				"Add the method first — Godot refuses connections to methods that do not exist.",
				details);
	}

	// --- argument-count sanity ---------------------------------------------
	// Reported, not enforced: GDScript allows a handler to declare fewer
	// parameters than the signal carries, and `binds` can add more.
	{
		TypedArray<Dictionary> signals = from->get_signal_list();
		int signal_args = -1;
		for (int i = 0; i < signals.size(); i++) {
			Dictionary s = signals[i];
			if (String(s["name"]) == signal_name) {
				signal_args = ((Array)s["args"]).size();
				break;
			}
		}
		TypedArray<Dictionary> methods = to->get_method_list();
		for (int i = 0; i < methods.size(); i++) {
			Dictionary m = methods[i];
			if (String(m["name"]) != method_name) {
				continue;
			}
			const int method_args = ((Array)m["args"]).size();
			if (signal_args >= 0 && method_args > signal_args) {
				warnings.push_back("'" + method_name + "' takes " + String::num_int64(method_args) +
						" argument(s) but '" + signal_name + "' emits " + String::num_int64(signal_args) +
						". The extra parameters need default values or binds, or the call will fail at runtime.");
			}
			break;
		}
	}

	const Callable callable = Callable(to, StringName(method_name));
	if (from->is_connected(StringName(signal_name), callable)) {
		return AIOSJson::error("already_connected",
				"'" + from_path + "." + signal_name + "' is already connected to '" + to_path + "." + method_name + "'.");
	}

	Dictionary plan;
	plan["from"] = AIOSWorldModel::node_path_in_scene(root, from);
	plan["to"] = AIOSWorldModel::node_path_in_scene(root, to);
	plan["signal"] = signal_name;
	plan["method"] = method_name;
	plan["warnings"] = warnings;

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		plan["dry_run"] = true;
		return AIOSJson::ok(plan);
	}

	uint32_t flags = Object::CONNECT_PERSIST;
	if (AIOSJson::get_bool(p_params, "one_shot", false)) {
		flags |= Object::CONNECT_ONE_SHOT;
	}
	if (AIOSJson::get_bool(p_params, "deferred", false)) {
		flags |= Object::CONNECT_DEFERRED;
	}

	const Error err = from->connect(StringName(signal_name), callable, flags);
	if (err != OK) {
		return AIOSJson::error("connect_failed",
				"Godot refused the connection (error " + String::num_int64(err) + ").");
	}

	mark_dirty();

	Dictionary result = plan;
	result["connected"] = true;
	result["persistent"] = true;
	return AIOSJson::ok(result);
}

Dictionary AIOSSceneTools::disconnect_signal_safe(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is currently open in the editor.");
	}

	const String from_path = AIOSJson::get_string(p_params, "from", "");
	const String to_path = AIOSJson::get_string(p_params, "to", "");
	const String signal_name = AIOSJson::get_string(p_params, "signal", "");
	const String method_name = AIOSJson::get_string(p_params, "method", "");

	if (from_path.is_empty() || to_path.is_empty() || signal_name.is_empty() || method_name.is_empty()) {
		return AIOSJson::error("missing_parameter",
				"'from', 'to', 'signal' and 'method' are all required.");
	}

	Node *from = AIOSWorldModel::resolve_node(root, from_path);
	Node *to = AIOSWorldModel::resolve_node(root, to_path);
	if (from == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + from_path + "'.");
	}
	if (to == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + to_path + "'.");
	}

	const Callable callable = Callable(to, StringName(method_name));
	if (!from->is_connected(StringName(signal_name), callable)) {
		return AIOSJson::error("not_connected",
				"'" + from_path + "." + signal_name + "' is not connected to '" + to_path + "." + method_name + "'. "
				"Call get_world_model to see the connections that do exist.");
	}

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		Dictionary plan;
		plan["dry_run"] = true;
		plan["would_disconnect"] = from_path + String(".") + signal_name + String(" -> ") + to_path + String(".") + method_name;
		return AIOSJson::ok(plan);
	}

	from->disconnect(StringName(signal_name), callable);
	mark_dirty();

	Dictionary result;
	result["from"] = AIOSWorldModel::node_path_in_scene(root, from);
	result["to"] = AIOSWorldModel::node_path_in_scene(root, to);
	result["signal"] = signal_name;
	result["method"] = method_name;
	result["disconnected"] = true;
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  reparent_node                                                              */
/* -------------------------------------------------------------------------- */

// Moving a node is where NodePath references quietly rot: every `$Sibling` and
// every exported NodePath that pointed at the old location is now wrong. This
// tool reports what it broke rather than pretending the move was free.
Dictionary AIOSSceneTools::reparent_node(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is currently open in the editor.");
	}

	const String node_path = AIOSJson::get_string(p_params, "node", "");
	if (node_path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'node' is required.");
	}
	Node *node = AIOSWorldModel::resolve_node(root, node_path);
	if (node == nullptr) {
		return AIOSJson::error("node_not_found", "No node at '" + node_path + "'.");
	}
	if (node == root) {
		return AIOSJson::error("cannot_move_root", "The scene root has no parent to move it under.");
	}
	if (node->get_owner() != root) {
		return AIOSJson::error("node_not_editable",
				"'" + node_path + "' belongs to an instanced sub-scene.");
	}

	Node *old_parent = node->get_parent();
	const String old_path = AIOSWorldModel::node_path_in_scene(root, node);

	// Reparenting is optional: with only `index` this is a pure reorder, which
	// is how you fix draw order or Control layout without restructuring.
	const String new_parent_path = AIOSJson::get_string(p_params, "new_parent", "");
	Node *new_parent = old_parent;
	if (!new_parent_path.is_empty()) {
		new_parent = AIOSWorldModel::resolve_node(root, new_parent_path);
		if (new_parent == nullptr) {
			return AIOSJson::error("parent_not_found", "No node at '" + new_parent_path + "'.");
		}
		if (new_parent != root && new_parent->get_owner() != root) {
			return AIOSJson::error("parent_not_editable",
					"'" + new_parent_path + "' belongs to an instanced sub-scene.");
		}
		// A node cannot become its own ancestor's child.
		for (Node *walk = new_parent; walk != nullptr; walk = walk->get_parent()) {
			if (walk == node) {
				return AIOSJson::error("cycle",
						"'" + new_parent_path + "' is inside '" + node_path + "', so this move would make the node its own descendant.");
			}
		}
	}

	const int64_t index = AIOSJson::get_int(p_params, "index", -1);
	const bool changing_parent = new_parent != old_parent;

	if (!changing_parent && index < 0) {
		return AIOSJson::error("nothing_to_do",
				"Pass 'new_parent' to move the node, 'index' to reorder it, or both.");
	}

	// Name collision in the destination.
	String name = node->get_name();
	Array warnings;
	if (changing_parent && new_parent->has_node(NodePath(name))) {
		if (!AIOSJson::get_bool(p_params, "auto_rename", true)) {
			return AIOSJson::error("name_conflict",
					"'" + new_parent_path + "' already has a child named '" + name + "'.");
		}
		const String renamed = unique_child_name(new_parent, name);
		warnings.push_back("'" + name + "' was taken in the new parent; renamed to '" + renamed + "'.");
		name = renamed;
	}

	// Who points at this node by path?
	Array references = find_script_references(node->get_name(), old_path, node->is_unique_name_in_owner());
	if (references.size() > 0) {
		warnings.push_back("Scripts reference this node by name or path; check them after the move.");
	}

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		Dictionary plan;
		plan["dry_run"] = true;
		plan["node"] = old_path;
		plan["from_parent"] = AIOSWorldModel::node_path_in_scene(root, old_parent);
		plan["to_parent"] = AIOSWorldModel::node_path_in_scene(root, new_parent);
		plan["warnings"] = warnings;
		plan["script_references"] = references;
		return AIOSJson::ok(plan);
	}

	// keep_global_transform matters for anything spatial: reparenting a node
	// under a transformed parent otherwise teleports it, which looks like the
	// tool corrupted the scene.
	const bool keep_transform = AIOSJson::get_bool(p_params, "keep_global_transform", true);

	if (changing_parent) {
		if (name != String(node->get_name())) {
			node->set_name(name);
		}
		node->reparent(new_parent, keep_transform);
		// reparent() preserves owner in 4.x, but a node whose owner was lost
		// silently stops being saved, so this is not worth leaving to chance.
		node->set_owner(root);
	}

	if (index >= 0) {
		Node *parent_now = node->get_parent();
		const int clamped = (int)(index < parent_now->get_child_count() ? index : parent_now->get_child_count() - 1);
		parent_now->move_child(node, clamped);
	}

	mark_dirty();

	Dictionary result;
	result["node"] = AIOSWorldModel::node_path_in_scene(root, node);
	result["previous_path"] = old_path;
	result["parent"] = AIOSWorldModel::node_path_in_scene(root, node->get_parent());
	result["index"] = node->get_index();
	result["kept_global_transform"] = changing_parent ? keep_transform : true;
	result["warnings"] = warnings;
	if (references.size() > 0) {
		result["script_references"] = references;
	}
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  set_node_properties                                                        */
/* -------------------------------------------------------------------------- */

// The counterpart to create_node_safe, and the tool that makes level building
// possible at all: without it an agent can create a node but never move it, so
// every mistake means delete-and-recreate.
Dictionary AIOSSceneTools::set_node_properties(const Dictionary &p_params) {
	Node *root = get_edited_root();
	if (root == nullptr) {
		return AIOSJson::error("no_open_scene", "No scene is currently open in the editor.");
	}

	const String node_path = AIOSJson::get_string(p_params, "node", "");
	if (node_path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'node' is required: the path of the node to modify.");
	}
	Node *node = AIOSWorldModel::resolve_node(root, node_path);
	if (node == nullptr) {
		return AIOSJson::error("node_not_found",
				"No node at '" + node_path + "'. Call get_world_model to see valid paths.");
	}
	if (node != root && node->get_owner() != root) {
		return AIOSJson::error("node_not_editable",
				"'" + node_path + "' belongs to an instanced sub-scene. Edit that scene directly, or enable editable children first.");
	}

	const Dictionary requested = AIOSJson::get_dict(p_params, "properties");
	if (requested.is_empty()) {
		return AIOSJson::error("missing_parameter", "'properties' is required and must not be empty.");
	}

	Dictionary resolved;
	Array property_errors;
	coerce_property_bag(requested, node_property_types(node), node->get_class(), resolved, property_errors);

	if (property_errors.size() > 0) {
		Dictionary details;
		details["errors"] = property_errors;
		return AIOSJson::error("invalid_properties",
				"One or more properties were rejected; nothing was changed.", details);
	}

	// Capture the old values before touching anything, so the report says what
	// actually changed rather than just what was asked for. An agent that set a
	// property to the value it already had should be able to tell.
	Array keys = resolved.keys();
	Dictionary previous;
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		previous[key] = AIOSJson::to_json(node->get(key));
	}

	Dictionary plan;
	plan["node"] = AIOSWorldModel::node_path_in_scene(root, node);
	plan["type"] = node->get_class();
	plan["properties"] = keys;
	plan["previous"] = previous;

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		plan["dry_run"] = true;
		return AIOSJson::ok(plan);
	}

	Array applied;
	Array unchanged;
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		const Variant before = node->get(key);
		node->set(key, resolved[key]);
		// Read it back rather than trusting the write: a setter can clamp, snap
		// or ignore a value, and reporting "applied" for a value the node
		// rejected would send the agent looking for the bug somewhere else.
		const Variant after = node->get(key);
		if (after == before) {
			unchanged.push_back(key);
		} else {
			applied.push_back(key);
		}
	}

	mark_dirty();

	Dictionary result;
	result["node"] = AIOSWorldModel::node_path_in_scene(root, node);
	result["type"] = node->get_class();
	result["applied"] = applied;
	result["previous"] = previous;
	if (unchanged.size() > 0) {
		result["unchanged"] = unchanged;
		result["note"] = "Some properties already held the requested value, or the node's setter overrode it. "
						 "Read them back with get_world_model if that is unexpected.";
	}
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  create_scene                                                               */
/* -------------------------------------------------------------------------- */

// Without this an agent has exactly one scene to work in — whatever the human
// happened to have open. Reusable prefabs (a Player, an Enemy, a Pickup) all
// start here.
Dictionary AIOSSceneTools::create_scene(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "This tool only works inside the Godot editor.");
	}

	String path = AIOSJson::get_string(p_params, "path", "");
	if (path.is_empty()) {
		return AIOSJson::error("missing_parameter", "'path' is required, e.g. 'res://scenes/player.tscn'.");
	}
	if (!path.begins_with("res://")) {
		return AIOSJson::error("invalid_path", "'path' must start with res://. Got '" + path + "'.");
	}
	if (path.get_extension().to_lower() != "tscn") {
		return AIOSJson::error("invalid_path", "Scene files must end in .tscn. Got '" + path + "'.");
	}
	if (FileAccess::file_exists(path) && !AIOSJson::get_bool(p_params, "overwrite", false)) {
		return AIOSJson::error("already_exists",
				"'" + path + "' already exists. Pass overwrite: true if you really mean to replace it.");
	}

	const String root_type = AIOSJson::get_string(p_params, "root_type", "Node2D");
	if (!ClassDBSingleton::get_singleton()->class_exists(root_type)) {
		return AIOSJson::error("unknown_type", "'" + root_type + "' is not a class known to this Godot build.");
	}
	if (!ClassDBSingleton::get_singleton()->is_parent_class(root_type, "Node")) {
		return AIOSJson::error("not_a_node", "'" + root_type + "' is not a Node subclass.");
	}
	if (!ClassDBSingleton::get_singleton()->can_instantiate(root_type)) {
		return AIOSJson::error("abstract_type", "'" + root_type + "' is abstract and cannot be instantiated.");
	}

	String root_name = AIOSJson::get_string(p_params, "root_name", "");
	if (root_name.is_empty()) {
		root_name = path.get_file().get_basename().capitalize().replace(" ", "");
	}
	String name_reason;
	if (!is_valid_node_name(root_name, name_reason)) {
		return AIOSJson::error("invalid_name", name_reason);
	}

	if (AIOSJson::get_bool(p_params, "dry_run", false)) {
		Dictionary plan;
		plan["dry_run"] = true;
		plan["would_create"] = path;
		plan["root_type"] = root_type;
		plan["root_name"] = root_name;
		return AIOSJson::ok(plan);
	}

	const String dir = path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir)) {
		if (DirAccess::make_dir_recursive_absolute(dir) != OK) {
			return AIOSJson::error("directory_failed", "Could not create '" + dir + "'.");
		}
	}

	Variant created = ClassDBSingleton::get_singleton()->instantiate(root_type);
	Node *root = Object::cast_to<Node>(created);
	if (root == nullptr) {
		return AIOSJson::error("instantiation_failed", "The engine refused to instantiate '" + root_type + "'.");
	}
	root->set_name(root_name);

	Ref<PackedScene> packed;
	packed.instantiate();
	const Error packed_err = packed->pack(root);
	if (packed_err != OK) {
		memdelete(root);
		return AIOSJson::error("pack_failed", "Could not pack the new scene (error " + String::num_int64(packed_err) + ").");
	}

	const Error save_err = ResourceSaver::get_singleton()->save(packed, path);
	// The template node has done its job; the PackedScene holds its own copy.
	memdelete(root);

	if (save_err != OK) {
		return AIOSJson::error("save_failed",
				"Could not write '" + path + "' (error " + String::num_int64(save_err) + "). Check the path is writable.");
	}

	// Make the new file visible to the FileSystem dock and to ResourceLoader
	// immediately — otherwise the very next instance_scene call cannot find it.
	if (ei->get_resource_filesystem() != nullptr) {
		ei->get_resource_filesystem()->update_file(path);
	}

	Dictionary result;
	result["path"] = path;
	result["root_type"] = root_type;
	result["root_name"] = root_name;

	if (AIOSJson::get_bool(p_params, "open", false)) {
		ei->open_scene_from_path(path);
		result["opened"] = true;
	} else {
		result["opened"] = false;
		result["note"] = "The scene was written but not opened. Pass open: true to edit it now, "
						 "or instance it into the current scene with create_node_safe's instance_scene.";
	}
	return AIOSJson::ok(result);
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

/* -------------------------------------------------------------------------- */
/*  import_asset                                                               */
/* -------------------------------------------------------------------------- */

static bool is_supported_asset_extension(const String &p_ext) {
	const String ext = p_ext.to_lower();
	return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg" ||
			ext == "wav" || ext == "ogg" || ext == "mp3";
}

Dictionary AIOSSceneTools::import_asset(const Dictionary &p_params) {
	const bool dry_run = AIOSJson::get_bool(p_params, "dry_run", false);
	const String source = AIOSJson::get_string(p_params, "source_path", "");
	String dest = AIOSJson::get_string(p_params, "dest_path", "");

	if (source.is_empty()) {
		return AIOSJson::error("missing_parameter", "'source_path' is required (absolute path to the file on disk).");
	}
	if (!FileAccess::file_exists(source)) {
		return AIOSJson::error("file_not_found", "No file at '" + source + "'.");
	}

	const String source_ext = source.get_extension().to_lower();
	if (!is_supported_asset_extension(source_ext)) {
		return AIOSJson::error("unsupported_format",
				"Unsupported extension '." + source_ext +
						"'. Supported: png, jpg, jpeg, webp, svg, wav, ogg, mp3.");
	}

	if (dest.is_empty()) {
		dest = "res://assets/" + source.get_file();
	}
	if (!dest.begins_with("res://")) {
		return AIOSJson::error("invalid_path", "'dest_path' must start with res://.");
	}

	const String dest_ext = dest.get_extension().to_lower();
	if (!dest_ext.is_empty() && dest_ext != source_ext) {
		return AIOSJson::error("extension_mismatch",
				"Destination extension '." + dest_ext + "' does not match source '." + source_ext + "'.");
	}

	const String global_dest = ProjectSettings::get_singleton()->globalize_path(dest);
	const String dest_dir = global_dest.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dest_dir)) {
		if (dry_run) {
			Dictionary result;
			result["dry_run"] = true;
			result["would_create_dir"] = dest_dir;
			result["would_copy"] = source + " -> " + dest;
			return AIOSJson::ok(result);
		}
		const Error mk = DirAccess::make_dir_recursive_absolute(dest_dir);
		if (mk != OK) {
			return AIOSJson::error("mkdir_failed", "Could not create directory '" + dest_dir + "'.");
		}
	}

	if (dry_run) {
		Dictionary result;
		result["dry_run"] = true;
		result["would_copy"] = source + " -> " + dest;
		result["note"] = "Godot will import the file on the next filesystem scan.";
		return AIOSJson::ok(result);
	}

	if (FileAccess::file_exists(global_dest)) {
		return AIOSJson::error("already_exists",
				"'" + dest + "' already exists. Choose a different dest_path or delete the existing file.");
	}

	const Error copied = DirAccess::copy_absolute(source, global_dest);
	if (copied != OK) {
		return AIOSJson::error("copy_failed", "Could not copy '" + source + "' to '" + dest + "'.");
	}

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs != nullptr) {
		efs->update_file(dest);
	}

	Dictionary result;
	result["dest_path"] = dest;
	result["source_path"] = source;
	result["imported"] = true;
	result["note"] = "File copied and queued for import. Use the returned res:// path in node properties.";
	return AIOSJson::ok(result);
}
