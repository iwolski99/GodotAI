/**************************************************************************/
/*  aios_world_model.cpp                                                  */
/**************************************************************************/

#include "aios_world_model.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

void AIOSWorldModel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("invalidate"), &AIOSWorldModel::invalidate);
	ClassDB::bind_method(D_METHOD("is_dirty"), &AIOSWorldModel::is_dirty);
	ClassDB::bind_method(D_METHOD("get_revision"), &AIOSWorldModel::get_revision);
	ClassDB::bind_method(D_METHOD("get_world_model", "params"), &AIOSWorldModel::get_world_model);

	ADD_SIGNAL(MethodInfo("model_invalidated", PropertyInfo(Variant::INT, "revision")));
}

void AIOSWorldModel::invalidate() {
	dirty = true;
	revision++;
	emit_signal("model_invalidated", revision);
}

Node *AIOSWorldModel::resolve_node(Node *p_scene_root, const String &p_path) {
	if (p_scene_root == nullptr) {
		return nullptr;
	}
	String path = p_path.strip_edges();
	if (path.is_empty() || path == "." || path == "/" || path == "./") {
		return p_scene_root;
	}
	// Agents habitually prefix the root's own name; accept it either way.
	const String root_name = String(p_scene_root->get_name());
	if (path == root_name) {
		return p_scene_root;
	}
	if (path.begins_with(root_name + String("/"))) {
		path = path.substr(root_name.length() + 1);
	}
	if (path.begins_with("./")) {
		path = path.substr(2);
	}
	if (path.begins_with("/")) {
		path = path.substr(1);
	}
	return p_scene_root->get_node_or_null(NodePath(path));
}

String AIOSWorldModel::node_path_in_scene(Node *p_scene_root, Node *p_node) {
	if (p_scene_root == nullptr || p_node == nullptr) {
		return String();
	}
	if (p_node == p_scene_root) {
		return ".";
	}
	return String(p_scene_root->get_path_to(p_node));
}

void AIOSWorldModel::scan_files(const String &p_dir, const PackedStringArray &p_extensions, PackedStringArray &r_out, int p_max) {
	if (r_out.size() >= p_max) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	String name = dir->get_next();
	while (!name.is_empty()) {
		if (name.begins_with(".")) {
			name = dir->get_next();
			continue;
		}
		const String full = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			// addons/ holds this plugin and third-party code; agents almost
			// never want it in a project inventory.
			if (name != "addons" && name != "godot-cpp") {
				scan_files(full, p_extensions, r_out, p_max);
			}
		} else {
			const String ext = name.get_extension().to_lower();
			for (int i = 0; i < p_extensions.size(); i++) {
				if (ext == p_extensions[i]) {
					r_out.push_back(full);
					break;
				}
			}
		}
		if (r_out.size() >= p_max) {
			break;
		}
		name = dir->get_next();
	}
	dir->list_dir_end();
}

Dictionary AIOSWorldModel::_describe_project() {
	Dictionary d;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	d["name"] = ps->get_setting("application/config/name", "");
	d["main_scene"] = ps->get_setting("application/run/main_scene", "");
	d["path"] = ps->globalize_path("res://");

	Dictionary version = Engine::get_singleton()->get_version_info();
	d["godot_version"] = version.has("string") ? String(version["string"]) : String("unknown");
	return d;
}

Dictionary AIOSWorldModel::_describe_editor() {
	Dictionary d;
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return d;
	}

	d["open_scenes"] = ei->get_open_scenes();
	d["is_playing"] = ei->is_playing_scene();
	d["playing_scene"] = ei->get_playing_scene();

	Node *root = ei->get_edited_scene_root();
	d["current_scene"] = root != nullptr ? root->get_scene_file_path() : String();

	Array selected;
	EditorSelection *selection = ei->get_selection();
	if (selection != nullptr && root != nullptr) {
		TypedArray<Node> nodes = selection->get_selected_nodes();
		for (int i = 0; i < nodes.size(); i++) {
			Node *n = Object::cast_to<Node>(nodes[i]);
			if (n != nullptr) {
				selected.push_back(node_path_in_scene(root, n));
			}
		}
	}
	d["selected_nodes"] = selected;
	return d;
}

Dictionary AIOSWorldModel::_collect_properties(Node *p_node) {
	Dictionary out;
	TypedArray<Dictionary> props = p_node->get_property_list();
	const String node_class = p_node->get_class();

	for (int i = 0; i < props.size(); i++) {
		Dictionary p = props[i];
		const int64_t usage = p.has("usage") ? (int64_t)p["usage"] : 0;
		if (!(usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		const String name = p["name"];
		if (name == "script" || name.begins_with("metadata/")) {
			continue;
		}

		const Variant value = p_node->get(name);
		// Only report what the user (or an agent) actually changed. A full dump
		// of every default is thousands of tokens of noise per node.
		const Variant def = ClassDBSingleton::get_singleton()->class_get_property_default_value(node_class, name);
		if (def.get_type() == value.get_type() && def == value) {
			continue;
		}
		out[name] = AIOSJson::to_json(value);
	}
	return out;
}

// Where a node actually is, in world space.
//
// This exists because an agent building a level is doing spatial reasoning with
// no eyes. `position` in the property dump is a *local* offset and is omitted
// entirely when it equals the default, so a model reading the world model could
// see a scene full of walls and have no idea where any of them were, whether
// they overlapped, or how big they were.
//
// Rotations are reported in degrees rather than radians: every model has seen
// far more "rotate 90 degrees" than "rotate 1.5708", and the conversion is a
// reliable source of silent off-by-a-factor bugs.
Dictionary AIOSWorldModel::_describe_spatial(Node *p_node) {
	Dictionary out;

	Node3D *n3d = Object::cast_to<Node3D>(p_node);
	if (n3d != nullptr) {
		out["space"] = "3d";
		out["position"] = AIOSJson::to_json(n3d->get_global_position());
		out["rotation_degrees"] = AIOSJson::to_json(n3d->get_rotation_degrees());
		out["scale"] = AIOSJson::to_json(n3d->get_scale());
		if (!n3d->is_visible()) {
			out["visible"] = false;
		}

		// The bounding box is what makes "does this overlap that" answerable.
		// get_aabb() is in local space, so it is transformed into world space
		// here — an untransformed AABB on a moved node is worse than none.
		VisualInstance3D *vis = Object::cast_to<VisualInstance3D>(p_node);
		if (vis != nullptr) {
			const AABB local = vis->get_aabb();
			if (local.size != Vector3()) {
				const AABB world = n3d->get_global_transform().xform(local);
				Dictionary bounds;
				bounds["min"] = AIOSJson::to_json(world.position);
				bounds["max"] = AIOSJson::to_json(world.position + world.size);
				bounds["size"] = AIOSJson::to_json(world.size);
				bounds["center"] = AIOSJson::to_json(world.get_center());
				out["bounds"] = bounds;
			}
		}
		return out;
	}

	Control *control = Object::cast_to<Control>(p_node);
	if (control != nullptr) {
		// Controls come before Node2D deliberately: Control extends CanvasItem,
		// not Node2D, and its rect is the useful thing rather than a position.
		out["space"] = "ui";
		const Rect2 rect = control->get_global_rect();
		out["position"] = AIOSJson::to_json(rect.position);
		out["size"] = AIOSJson::to_json(rect.size);
		Dictionary bounds;
		bounds["min"] = AIOSJson::to_json(rect.position);
		bounds["max"] = AIOSJson::to_json(rect.position + rect.size);
		out["bounds"] = bounds;
		if (!control->is_visible()) {
			out["visible"] = false;
		}
		return out;
	}

	Node2D *n2d = Object::cast_to<Node2D>(p_node);
	if (n2d != nullptr) {
		out["space"] = "2d";
		out["position"] = AIOSJson::to_json(n2d->get_global_position());
		out["rotation_degrees"] = n2d->get_global_rotation_degrees();
		out["scale"] = AIOSJson::to_json(n2d->get_global_scale());
		if (!n2d->is_visible()) {
			out["visible"] = false;
		}
		return out;
	}

	return out; // Not a spatial node; nothing to say.
}

Array AIOSWorldModel::_collect_signals(Node *p_node, Node *p_scene_root) {
	Array out;

	TypedArray<Dictionary> signal_list = p_node->get_signal_list();
	for (int i = 0; i < signal_list.size(); i++) {
		Dictionary s = signal_list[i];
		const String signal_name = s["name"];
		TypedArray<Dictionary> connections = p_node->get_signal_connection_list(StringName(signal_name));
		for (int j = 0; j < connections.size(); j++) {
			Dictionary conn = connections[j];
			// Only connections authored in the editor (CONNECT_PERSIST) are part
			// of the scene's design; the rest is engine plumbing.
			if (!conn.has("flags") || ((int64_t)conn["flags"] & Object::CONNECT_PERSIST) == 0) {
				continue;
			}
			Dictionary entry;
			entry["direction"] = "outgoing";
			entry["signal"] = signal_name;
			Variant callable = conn.has("callable") ? conn["callable"] : Variant();
			entry["callable"] = String(callable);
			if (conn.has("flags")) {
				entry["flags"] = conn["flags"];
			}
			out.push_back(entry);
		}
	}

	TypedArray<Dictionary> incoming = p_node->get_incoming_connections();
	for (int i = 0; i < incoming.size(); i++) {
		Dictionary conn = incoming[i];
		if (!conn.has("flags") || ((int64_t)conn["flags"] & Object::CONNECT_PERSIST) == 0) {
			continue;
		}
		Dictionary entry;
		entry["direction"] = "incoming";
		entry["signal"] = conn.has("signal") ? String(conn["signal"]) : String();
		entry["callable"] = conn.has("callable") ? String(conn["callable"]) : String();
		out.push_back(entry);
	}

	(void)p_scene_root;
	return out;
}

Dictionary AIOSWorldModel::_describe_node(Node *p_node, Node *p_scene_root, const Dictionary &p_opts, int p_depth) {
	Dictionary d;
	d["name"] = String(p_node->get_name());
	d["path"] = node_path_in_scene(p_scene_root, p_node);
	d["type"] = p_node->get_class();

	Ref<Script> script = p_node->get_script();
	d["script"] = script.is_valid() ? Variant(script->get_path()) : Variant();

	if (p_node->is_unique_name_in_owner()) {
		d["unique_name"] = true;
	}

	// A node whose scene_file_path is set inside another scene is an instance
	// of that packed scene; agents need to know they are looking at a boundary.
	const String scene_file = p_node->get_scene_file_path();
	if (p_node != p_scene_root && !scene_file.is_empty()) {
		d["instance_of"] = scene_file;
	}

	TypedArray<StringName> groups = p_node->get_groups();
	Array g;
	for (int i = 0; i < groups.size(); i++) {
		const String group = String(groups[i]);
		// The editor puts internal bookkeeping groups (_root_canvas..., etc.) on
		// nodes. They are not part of the project's design and only confuse an
		// agent reading the model.
		if (group.begins_with("_")) {
			continue;
		}
		g.push_back(group);
	}
	if (g.size() > 0) {
		d["groups"] = g;
	}

	// On by default, unlike properties: this is small (one nested dict, only for
	// spatial nodes) and it is the difference between an agent that can place a
	// wall next to another wall and one that is guessing.
	if (AIOSJson::get_bool(p_opts, "include_transforms", true)) {
		Dictionary spatial = _describe_spatial(p_node);
		if (!spatial.is_empty()) {
			d["spatial"] = spatial;
		}
	}

	if (AIOSJson::get_bool(p_opts, "include_properties", false)) {
		d["properties"] = _collect_properties(p_node);
	}
	if (AIOSJson::get_bool(p_opts, "include_signals", false)) {
		Array sig = _collect_signals(p_node, p_scene_root);
		if (sig.size() > 0) {
			d["connections"] = sig;
		}
	}

	const int64_t max_depth = AIOSJson::get_int(p_opts, "max_depth", -1);
	const bool include_instanced = AIOSJson::get_bool(p_opts, "include_instanced_children", false);

	Array children;
	int hidden = 0;
	const int child_count = (int)p_node->get_child_count();
	for (int i = 0; i < child_count; i++) {
		Node *child = p_node->get_child(i);
		if (child == nullptr) {
			continue;
		}
		// Children of an instanced sub-scene have that sub-scene's root as
		// owner. They are not editable from here, so hide them by default.
		if (!include_instanced && child->get_owner() != p_scene_root) {
			hidden++;
			continue;
		}
		if (max_depth >= 0 && p_depth >= max_depth) {
			hidden++;
			continue;
		}
		children.push_back(_describe_node(child, p_scene_root, p_opts, p_depth + 1));
	}

	d["child_count"] = child_count;
	if (hidden > 0) {
		d["hidden_children"] = hidden;
	}
	d["children"] = children;
	return d;
}

Dictionary AIOSWorldModel::_describe_filesystem(int p_max_entries) {
	Dictionary d;

	PackedStringArray scenes;
	PackedStringArray scene_ext;
	scene_ext.push_back("tscn");
	scene_ext.push_back("scn");
	scan_files("res://", scene_ext, scenes, p_max_entries);

	PackedStringArray scripts;
	PackedStringArray script_ext;
	script_ext.push_back("gd");
	script_ext.push_back("cs");
	scan_files("res://", script_ext, scripts, p_max_entries);

	d["scenes"] = scenes;
	d["scripts"] = scripts;
	d["truncated"] = scenes.size() >= p_max_entries || scripts.size() >= p_max_entries;
	return d;
}

Dictionary AIOSWorldModel::get_world_model(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "The editor interface is not available. get_world_model only works inside the Godot editor.");
	}

	// The cache key folds in every option that changes the shape of the output,
	// so switching detail levels never serves a stale, thinner model.
	const String key = vformat("%d|%d|%d|%d|%d|%d",
			(int)AIOSJson::get_bool(p_params, "include_properties", false),
			(int)AIOSJson::get_bool(p_params, "include_signals", false),
			(int)AIOSJson::get_bool(p_params, "include_instanced_children", false),
			(int)AIOSJson::get_bool(p_params, "include_filesystem", true),
			(int)AIOSJson::get_int(p_params, "max_depth", -1),
			(int)AIOSJson::get_int(p_params, "max_files", 512));

	const bool force = AIOSJson::get_bool(p_params, "refresh", false);
	if (!force && !dirty && key == cache_key && !cache.is_empty()) {
		Dictionary cached = cache.duplicate(true);
		cached["cached"] = true;
		return AIOSJson::ok(cached);
	}

	const uint64_t started = Time::get_singleton()->get_ticks_usec();

	Dictionary model;
	model["revision"] = revision;
	model["generated_at"] = Time::get_singleton()->get_unix_time_from_system();
	model["project"] = _describe_project();
	model["editor"] = _describe_editor();

	Node *root = ei->get_edited_scene_root();
	if (root != nullptr) {
		Dictionary scene;
		scene["path"] = root->get_scene_file_path();
		scene["root"] = _describe_node(root, root, p_params, 0);
		model["scene"] = scene;
	} else {
		model["scene"] = Variant();
		model["hint"] = "No scene is open in the editor. Open or create one before editing nodes.";
	}

	if (AIOSJson::get_bool(p_params, "include_filesystem", true)) {
		model["filesystem"] = _describe_filesystem((int)AIOSJson::get_int(p_params, "max_files", 512));
	}

	last_build_msec = (double)(Time::get_singleton()->get_ticks_usec() - started) / 1000.0;
	model["build_msec"] = last_build_msec;

	cache = model;
	cache_key = key;
	dirty = false;

	Dictionary out = model.duplicate(true);
	out["cached"] = false;
	return AIOSJson::ok(out);
}
