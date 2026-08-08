/**************************************************************************/
/*  aios_validator.cpp                                                    */
/**************************************************************************/

#include "aios_validator.h"

#include "../tools/aios_scene_tools.h"
#include "../util/aios_json.h"
#include "../world/aios_world_model.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <vector>

void AIOSValidator::_bind_methods() {
	ClassDB::bind_static_method("AIOSValidator", D_METHOD("validate_script_source", "source", "target_class"), &AIOSValidator::validate_script_source);
	ClassDB::bind_static_method("AIOSValidator", D_METHOD("validate_node_references", "source", "attach_path"), &AIOSValidator::validate_node_references);
	ClassDB::bind_static_method("AIOSValidator", D_METHOD("validate_properties", "class", "properties"), &AIOSValidator::validate_properties);
	ClassDB::bind_static_method("AIOSValidator", D_METHOD("validate_scene"), &AIOSValidator::validate_scene);
	ClassDB::bind_static_method("AIOSValidator", D_METHOD("validate_planned_call", "tool", "params"), &AIOSValidator::validate_planned_call);
}

void AIOSValidator::_add(Array &r_findings, const String &p_severity, const String &p_code,
		const String &p_message, const Dictionary &p_data) {
	Dictionary finding;
	finding["severity"] = p_severity;
	finding["code"] = p_code;
	finding["message"] = p_message;
	if (!p_data.is_empty()) {
		finding["data"] = p_data;
	}
	r_findings.push_back(finding);
}

static bool has_errors(const Array &p_findings) {
	for (int i = 0; i < p_findings.size(); i++) {
		Dictionary f = p_findings[i];
		if (String(f["severity"]) == "error") {
			return true;
		}
	}
	return false;
}

/* -------------------------------------------------------------------------- */
/*  Script compilation                                                         */
/* -------------------------------------------------------------------------- */

static String parse_extends_line(const String &p_source) {
	PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = String(lines[i]).strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("@") || line.begins_with("class_name")) {
			continue;
		}
		if (line.begins_with("extends ")) {
			return line.substr(8).strip_edges();
		}
		return String();
	}
	return String();
}

static String parse_class_name_line(const String &p_source) {
	PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = String(lines[i]).strip_edges();
		if (line.begins_with("class_name ")) {
			String rest = line.substr(11).strip_edges();
			// `class_name Foo extends Bar` is legal; take the identifier only.
			const int space = rest.find(" ");
			return space >= 0 ? rest.substr(0, space) : rest;
		}
	}
	return String();
}

Dictionary AIOSValidator::validate_script_source(const String &p_source, const String &p_target_class) {
	Array findings;

	if (p_source.strip_edges().is_empty()) {
		_add(findings, "error", "empty_source", "The script source is empty.");
		Dictionary result;
		result["findings"] = findings;
		result["valid"] = false;
		return AIOSJson::ok(result);
	}

	const String extends_class = parse_extends_line(p_source);
	const String class_name_decl = parse_class_name_line(p_source);

	// --- compile in memory --------------------------------------------------
	Variant instance = ClassDBSingleton::get_singleton()->instantiate("GDScript");
	Ref<Script> script = instance;
	if (script.is_null()) {
		_add(findings, "warning", "compiler_unavailable",
				"Could not instantiate the GDScript compiler, so syntax was not checked.");
	} else {
		script->set_source_code(p_source);
		const Error err = script->reload(false);
		if (err != OK) {
			// The parser writes its diagnostic to the editor's Output panel; we
			// can only see the error code, so point the user at where the detail
			// actually is rather than pretending we have it.
			Dictionary data;
			data["error_code"] = (int)err;
			_add(findings, "error", "parse_error",
					"The script does not compile. The parser's message (with line number) is in Godot's Output "
					"panel. Nothing was written to disk.",
					data);
		}
	}

	// --- extends compatibility ---------------------------------------------
	if (extends_class.is_empty()) {
		_add(findings, "error", "no_extends",
				"The script has no `extends` line, so Godot treats it as extending RefCounted. It cannot be "
				"attached to a node.");
	} else if (!p_target_class.is_empty() && !extends_class.begins_with("\"") && !extends_class.begins_with("res://")) {
		ClassDBSingleton *db = ClassDBSingleton::get_singleton();
		if (db->class_exists(extends_class)) {
			if (!db->is_parent_class(p_target_class, extends_class)) {
				Dictionary data;
				data["extends"] = extends_class;
				data["target_class"] = p_target_class;
				_add(findings, "error", "incompatible_base",
						"The script extends " + extends_class + " but the target node is a " + p_target_class +
								", which does not inherit from it.",
						data);
			}
		} else {
			_add(findings, "warning", "unresolved_base",
					"`extends " + extends_class + "` is not an engine class. If it is a class_name from another "
					"script, make sure that script exists and compiles.");
		}
	}

	Dictionary result;
	result["findings"] = findings;
	result["valid"] = !has_errors(findings);
	result["extends"] = extends_class;
	if (!class_name_decl.is_empty()) {
		result["class_name"] = class_name_decl;
	}
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Node reference extraction                                                  */
/* -------------------------------------------------------------------------- */

Array AIOSValidator::_extract_node_references(const String &p_source) {
	Array refs;
	PackedStringArray lines = p_source.split("\n");

	for (int line_no = 0; line_no < lines.size(); line_no++) {
		const String line = lines[line_no];
		// Comments cannot contain live node lookups; skipping them removes the
		// most common source of false positives (commented-out code).
		const int comment = line.find("#");
		const String code = comment >= 0 ? line.substr(0, comment) : line;

		for (int i = 0; i < code.length(); i++) {
			const String ch = code.substr(i, 1);
			String path;
			bool unique = false;

			if (ch == "$" || ch == "%") {
				unique = ch == "%";
				int j = i + 1;
				if (j < code.length() && code.substr(j, 1) == "\"") {
					const int close = code.find("\"", j + 1);
					if (close < 0) {
						break;
					}
					path = code.substr(j + 1, close - j - 1);
					i = close;
				} else {
					// Bare form: consume identifier characters plus / for paths.
					int end = j;
					while (end < code.length()) {
						const String c = code.substr(end, 1);
						const bool ok = (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") ||
								(c >= "0" && c <= "9") || c == "_" || c == "/";
						if (!ok) {
							break;
						}
						end++;
					}
					path = code.substr(j, end - j);
					i = end - 1;
				}
			} else if (code.substr(i, 9) == "get_node(" || code.substr(i, 17) == "get_node_or_null(") {
				const int open = code.find("(", i);
				const int quote = code.find("\"", open);
				if (quote < 0) {
					continue;
				}
				const int close = code.find("\"", quote + 1);
				if (close < 0) {
					continue;
				}
				path = code.substr(quote + 1, close - quote - 1);
				i = close;
			} else {
				continue;
			}

			if (path.strip_edges().is_empty()) {
				continue;
			}
			Dictionary ref;
			ref["path"] = path;
			ref["line"] = line_no + 1;
			ref["unique_name"] = unique;
			refs.push_back(ref);
		}
	}
	return refs;
}

Dictionary AIOSValidator::validate_node_references(const String &p_source, const String &p_attach_path) {
	Array findings;
	Array refs = AIOSValidator::_extract_node_references(p_source);

	Node *root = AIOSSceneTools::get_edited_root();
	if (root == nullptr) {
		_add(findings, "info", "no_open_scene",
				"No scene is open, so node references could not be checked.");
		Dictionary result;
		result["findings"] = findings;
		result["references"] = refs;
		result["valid"] = true;
		return AIOSJson::ok(result);
	}

	// Paths in a script resolve relative to the node the script is on, not the
	// scene root. Getting this wrong would flag every correct relative path.
	Node *base = p_attach_path.is_empty() ? root : AIOSWorldModel::resolve_node(root, p_attach_path);
	if (base == nullptr) {
		base = root;
	}

	// Collect unique names once so %Name lookups can be checked.
	std::vector<Node *> all_nodes;
	AIOSSceneTools::collect_owned_nodes(root, root, all_nodes);

	for (int i = 0; i < refs.size(); i++) {
		Dictionary ref = refs[i];
		const String path = String(ref["path"]);
		const bool unique = (bool)ref["unique_name"];

		bool found = false;
		if (unique) {
			for (size_t n = 0; n < all_nodes.size(); n++) {
				if (all_nodes[n]->is_unique_name_in_owner() && String(all_nodes[n]->get_name()) == path) {
					found = true;
					break;
				}
			}
		} else {
			found = base->get_node_or_null(NodePath(path)) != nullptr;
			if (!found) {
				// A script attached to a node that is not in the scene yet still
				// resolves against the root for absolute-looking paths.
				found = root->get_node_or_null(NodePath(path)) != nullptr;
			}
		}

		if (!found) {
			Dictionary data;
			data["path"] = path;
			data["line"] = ref["line"];
			data["resolved_from"] = AIOSWorldModel::node_path_in_scene(root, base);
			// A warning rather than an error: the node may be created later in
			// the same plan, or built at runtime. Blocking here would make the
			// validator refuse legitimate work.
			_add(findings, "warning", "unresolved_node_reference",
					String(unique ? "%" : "$") + path + " (line " + String::num_int64((int64_t)ref["line"]) +
							") does not resolve in the current scene. If the node is created later in this plan, "
							"this is fine; otherwise the lookup will return null at runtime.",
					data);
		}
	}

	Dictionary result;
	result["findings"] = findings;
	result["references"] = refs;
	result["valid"] = !has_errors(findings);
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Property validation                                                        */
/* -------------------------------------------------------------------------- */

Dictionary AIOSValidator::validate_properties(const String &p_class, const Dictionary &p_properties) {
	Array findings;
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();

	if (!db->class_exists(p_class)) {
		_add(findings, "error", "unknown_type", "'" + p_class + "' is not a class in this Godot build.");
		Dictionary result;
		result["findings"] = findings;
		result["valid"] = false;
		return AIOSJson::ok(result);
	}

	Dictionary known;
	TypedArray<Dictionary> props = db->class_get_property_list(p_class, false);
	for (int i = 0; i < props.size(); i++) {
		Dictionary p = props[i];
		const int64_t usage = p.has("usage") ? (int64_t)p["usage"] : 0;
		if (usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		known[String(p["name"])] = p.has("type") ? (int)(int64_t)p["type"] : 0;
	}

	Array keys = p_properties.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		if (!known.has(key)) {
			Dictionary data;
			data["property"] = key;
			_add(findings, "error", "unknown_property",
					"'" + p_class + "' has no property named '" + key + "'.", data);
			continue;
		}

		Variant coerced;
		String coerce_error;
		if (!AIOSJson::coerce(p_properties[key], (int)(int64_t)known[key], coerced, coerce_error)) {
			Dictionary data;
			data["property"] = key;
			data["problem"] = coerce_error;
			_add(findings, "error", "invalid_property_type",
					"Property '" + key + "': " + coerce_error + ".", data);
		}
	}

	Dictionary result;
	result["findings"] = findings;
	result["valid"] = !has_errors(findings);
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Whole-scene sweep                                                          */
/* -------------------------------------------------------------------------- */

Dictionary AIOSValidator::validate_scene() {
	Array findings;

	Node *root = AIOSSceneTools::get_edited_root();
	if (root == nullptr) {
		_add(findings, "info", "no_open_scene", "No scene is open in the editor.");
		Dictionary result;
		result["findings"] = findings;
		result["valid"] = true;
		return AIOSJson::ok(result);
	}

	std::vector<Node *> nodes;
	AIOSSceneTools::collect_owned_nodes(root, root, nodes);

	for (size_t i = 0; i < nodes.size(); i++) {
		Node *node = nodes[i];
		const String node_path = AIOSWorldModel::node_path_in_scene(root, node);

		// A node with no owner is invisible to the .tscn writer: it exists in
		// the running tree and silently disappears the moment the scene is
		// saved and reloaded. This is the single most confusing way an
		// agent-built scene "loses" work, so it is an error, not a warning.
		if (node != root && node->get_owner() == nullptr) {
			Dictionary data;
			data["node"] = node_path;
			_add(findings, "error", "node_without_owner",
					"'" + node_path + "' has no owner, so it will not be saved into the scene file.", data);
		}

		// Scripts that failed to load leave the property set but with a null
		// value — the node looks scripted in the inspector and does nothing.
		Variant script_value = node->get_script();
		if (script_value.get_type() == Variant::OBJECT) {
			Ref<Script> script = script_value;
			if (script.is_valid() && !script->get_path().is_empty() && !FileAccess::file_exists(script->get_path())) {
				Dictionary data;
				data["node"] = node_path;
				data["script"] = script->get_path();
				_add(findings, "error", "missing_script_file",
						"'" + node_path + "' references " + script->get_path() + ", which is not on disk.", data);
			}
		}

		TypedArray<Dictionary> props = node->get_property_list();
		for (int p = 0; p < props.size(); p++) {
			Dictionary prop = props[p];
			const int64_t usage = prop.has("usage") ? (int64_t)prop["usage"] : 0;
			if (!(usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}
			const String prop_name = prop["name"];
			const int type = (int)(int64_t)prop["type"];

			if (type == Variant::NODE_PATH) {
				NodePath np = node->get(prop_name);
				if (np.is_empty()) {
					continue;
				}
				if (node->get_node_or_null(np) == nullptr) {
					Dictionary data;
					data["node"] = node_path;
					data["property"] = prop_name;
					data["path"] = String(np);
					_add(findings, "error", "dangling_node_path",
							"'" + node_path + "." + prop_name + "' points at '" + String(np) +
									"', which does not exist.",
							data);
				}
			} else if (type == Variant::OBJECT) {
				Variant value = node->get(prop_name);
				if (value.get_type() != Variant::OBJECT) {
					continue;
				}
				Ref<Resource> res = value;
				if (res.is_valid()) {
					const String res_path = res->get_path();
					// Built-in / sub-resources have no on-disk path and are fine.
					if (res_path.begins_with("res://") && !res_path.contains("::") && !FileAccess::file_exists(res_path)) {
						Dictionary data;
						data["node"] = node_path;
						data["property"] = prop_name;
						data["resource"] = res_path;
						_add(findings, "error", "missing_resource",
								"'" + node_path + "." + prop_name + "' references " + res_path +
										", which is not on disk.",
								data);
					}
				}
			}
		}
	}

	Dictionary result;
	result["findings"] = findings;
	result["valid"] = !has_errors(findings);
	result["nodes_checked"] = (int)nodes.size();
	result["scene"] = root->get_scene_file_path();
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Planned-call dispatch                                                      */
/* -------------------------------------------------------------------------- */

Dictionary AIOSValidator::validate_planned_call(const String &p_tool, const Dictionary &p_params) {
	Array findings;

	if (p_tool == "create_node_safe") {
		const String type = AIOSJson::get_string(p_params, "type", "");
		if (!type.is_empty()) {
			Dictionary props = AIOSValidator::validate_properties(type, AIOSJson::get_dict(p_params, "properties"));
			Array sub = Dictionary(props["result"])["findings"];
			findings.append_array(sub);
		}
	} else if (p_tool == "attach_script_safe") {
		const String source = AIOSJson::get_string(p_params, "source", "");
		if (!source.is_empty()) {
			// Resolve the target node's class so the extends check has something
			// concrete to compare against.
			String target_class;
			Node *root = AIOSSceneTools::get_edited_root();
			const String node_path = AIOSJson::get_string(p_params, "node", "");
			if (root != nullptr && !node_path.is_empty()) {
				Node *node = AIOSWorldModel::resolve_node(root, node_path);
				if (node != nullptr) {
					target_class = node->get_class();
				}
			}

			Dictionary script_check = AIOSValidator::validate_script_source(source, target_class);
			findings.append_array(Array(Dictionary(script_check["result"])["findings"]));

			Dictionary ref_check = AIOSValidator::validate_node_references(source, node_path);
			findings.append_array(Array(Dictionary(ref_check["result"])["findings"]));
		}
	} else if (p_tool == "set_node_properties") {
		// Resolve the node's real class, then run the same property checks
		// create_node_safe gets. Without this the Milestone 3 editing tools
		// would sit outside the Validate stage entirely.
		Node *root = AIOSSceneTools::get_edited_root();
		const String node_path = AIOSJson::get_string(p_params, "node", "");
		if (root != nullptr && !node_path.is_empty()) {
			Node *node = AIOSWorldModel::resolve_node(root, node_path);
			if (node == nullptr) {
				_add(findings, "error", "node_not_found",
						"No node at '" + node_path + "' in the open scene.");
			} else {
				Dictionary props = AIOSValidator::validate_properties(
						node->get_class(), AIOSJson::get_dict(p_params, "properties"));
				findings.append_array(Array(Dictionary(props["result"])["findings"]));
			}
		}
	} else if (p_tool == "patch_script") {
		// The tool compiles the patched result in memory before writing, but
		// checking here too means a doomed patch is refused before it is even
		// attempted, and the model gets the finding in the same shape as
		// everything else.
		const String path = AIOSJson::get_string(p_params, "path", "");
		const String operation = AIOSJson::get_string(p_params, "operation", "");
		if (path.is_empty()) {
			_add(findings, "error", "missing_parameter", "'path' is required.");
		} else if (!FileAccess::file_exists(path)) {
			_add(findings, "error", "missing_script_file",
					"No file at '" + path + "'. Use attach_script_safe to create a new script.");
		}
		if (operation != "replace_function" && operation != "append" && operation != "replace_text") {
			_add(findings, "error", "unknown_operation",
					"'operation' must be replace_function, append or replace_text. Got '" + operation + "'.");
		}
	} else if (p_tool == "connect_signal_safe") {
		Node *root = AIOSSceneTools::get_edited_root();
		if (root != nullptr) {
			const String from_path = AIOSJson::get_string(p_params, "from", "");
			const String to_path = AIOSJson::get_string(p_params, "to", "");
			const String signal_name = AIOSJson::get_string(p_params, "signal", "");
			const String method_name = AIOSJson::get_string(p_params, "method", "");

			Node *from = from_path.is_empty() ? nullptr : AIOSWorldModel::resolve_node(root, from_path);
			Node *to = to_path.is_empty() ? nullptr : AIOSWorldModel::resolve_node(root, to_path);

			if (from == nullptr) {
				_add(findings, "error", "node_not_found", "No node at '" + from_path + "' (the emitter).");
			} else if (!signal_name.is_empty() && !from->has_signal(StringName(signal_name))) {
				_add(findings, "error", "unknown_signal",
						"'" + from->get_class() + "' has no signal named '" + signal_name + "'.");
			}
			if (to == nullptr) {
				_add(findings, "error", "node_not_found", "No node at '" + to_path + "' (the receiver).");
			} else if (!method_name.is_empty() && !to->has_method(StringName(method_name))) {
				_add(findings, "error", "no_receiver_method",
						"'" + to_path + "' has no method '" + method_name + "'. Godot refuses connections to "
						"methods that do not exist, so add it before connecting.");
			}
		}
	} else if (p_tool == "create_scene") {
		const String path = AIOSJson::get_string(p_params, "path", "");
		const String root_type = AIOSJson::get_string(p_params, "root_type", "Node2D");
		if (!path.begins_with("res://") || path.get_extension().to_lower() != "tscn") {
			_add(findings, "error", "invalid_path",
					"'path' must be a res:// path ending in .tscn. Got '" + path + "'.");
		}
		if (!root_type.is_empty() && !ClassDBSingleton::get_singleton()->class_exists(root_type)) {
			_add(findings, "error", "unknown_type",
					"'" + root_type + "' is not a class known to this Godot build.");
		}
	} else if (p_tool == "safe_delete_node") {
		// Deletion has its own dependency audit inside the tool; running it in
		// dry-run mode here is both the cheapest and the most accurate check,
		// because it is literally the same code that will run for real.
		Dictionary params = p_params.duplicate();
		params["dry_run"] = true;
		Dictionary audit = AIOSSceneTools::safe_delete_node(params);
		if ((bool)audit["ok"]) {
			Dictionary report = audit["result"];
			Array blockers = AIOSJson::get_array(report, "blockers");
			for (int i = 0; i < blockers.size(); i++) {
				Dictionary b = blockers[i];
				_add(findings, "error", String(b["code"]), String(b["message"]));
			}
			Array warnings = AIOSJson::get_array(report, "warnings");
			for (int i = 0; i < warnings.size(); i++) {
				Dictionary w = warnings[i];
				_add(findings, "warning", String(w["code"]), String(w["message"]));
			}
		}
	}

	Dictionary result;
	result["tool"] = p_tool;
	result["findings"] = findings;
	result["valid"] = !has_errors(findings);
	return AIOSJson::ok(result);
}
