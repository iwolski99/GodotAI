/**************************************************************************/
/*  aios_json.cpp                                                         */
/**************************************************************************/

#include "aios_json.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/vector4.hpp>

#define AIOS_MAX_JSON_DEPTH 24

static Dictionary tagged(const String &p_type) {
	Dictionary d;
	d["__type"] = p_type;
	return d;
}

Variant AIOSJson::to_json(const Variant &p_value, int p_depth) {
	if (p_depth > AIOS_MAX_JSON_DEPTH) {
		return String("<max-depth-exceeded>");
	}

	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_value;

		case Variant::STRING_NAME:
			return String(p_value);

		case Variant::NODE_PATH:
			return String(NodePath(p_value));

		case Variant::VECTOR2: {
			Vector2 v = p_value;
			Dictionary d = tagged("Vector2");
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR2I: {
			Vector2i v = p_value;
			Dictionary d = tagged("Vector2i");
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR3: {
			Vector3 v = p_value;
			Dictionary d = tagged("Vector3");
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::VECTOR3I: {
			Vector3i v = p_value;
			Dictionary d = tagged("Vector3i");
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::VECTOR4: {
			Vector4 v = p_value;
			Dictionary d = tagged("Vector4");
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			d["w"] = v.w;
			return d;
		}
		case Variant::RECT2: {
			Rect2 r = p_value;
			Dictionary d = tagged("Rect2");
			d["x"] = r.position.x;
			d["y"] = r.position.y;
			d["w"] = r.size.x;
			d["h"] = r.size.y;
			return d;
		}
		case Variant::RECT2I: {
			Rect2i r = p_value;
			Dictionary d = tagged("Rect2i");
			d["x"] = r.position.x;
			d["y"] = r.position.y;
			d["w"] = r.size.x;
			d["h"] = r.size.y;
			return d;
		}
		case Variant::COLOR: {
			Color c = p_value;
			Dictionary d = tagged("Color");
			d["r"] = c.r;
			d["g"] = c.g;
			d["b"] = c.b;
			d["a"] = c.a;
			d["hex"] = String("#") + c.to_html(true);
			return d;
		}
		case Variant::OBJECT: {
			Object *obj = p_value;
			if (obj == nullptr) {
				return Variant();
			}
			Dictionary d = tagged("Object");
			d["class"] = obj->get_class();
			Resource *res = Object::cast_to<Resource>(obj);
			if (res != nullptr) {
				d["__type"] = "Resource";
				d["path"] = res->get_path();
			}
			Node *node = Object::cast_to<Node>(obj);
			if (node != nullptr) {
				d["__type"] = "Node";
				d["name"] = String(node->get_name());
			}
			return d;
		}
		case Variant::ARRAY:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY: {
			Array src = p_value;
			Array out;
			for (int i = 0; i < src.size(); i++) {
				out.push_back(to_json(src[i], p_depth + 1));
			}
			return out;
		}
		case Variant::DICTIONARY: {
			Dictionary src = p_value;
			Dictionary out;
			Array keys = src.keys();
			for (int i = 0; i < keys.size(); i++) {
				out[String(keys[i])] = to_json(src[keys[i]], p_depth + 1);
			}
			return out;
		}
		default: {
			// Transforms, Basis, RIDs, Callables, ... Not worth a bespoke
			// encoding yet; the string form is at least inspectable.
			Dictionary d = tagged(type_name(p_value.get_type()));
			d["value"] = String(p_value);
			return d;
		}
	}
}

// Reads x/y/z/w (or index 0..3) out of an Array or Dictionary into a float
// buffer. Returns the number of components found, or -1 if the shape is wrong.
static int read_components(const Variant &p_input, double *r_out, int p_max) {
	static const char *keys[4] = { "x", "y", "z", "w" };

	if (p_input.get_type() == Variant::ARRAY) {
		Array a = p_input;
		int n = a.size() < p_max ? (int)a.size() : p_max;
		for (int i = 0; i < n; i++) {
			Variant v = a[i];
			if (v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) {
				return -1;
			}
			r_out[i] = (double)v;
		}
		return n;
	}

	if (p_input.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_input;
		int n = 0;
		for (int i = 0; i < p_max; i++) {
			if (!d.has(keys[i])) {
				break;
			}
			Variant v = d[keys[i]];
			if (v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) {
				return -1;
			}
			r_out[i] = (double)v;
			n++;
		}
		return n;
	}

	return -1;
}

bool AIOSJson::coerce(const Variant &p_input, int p_target_type, Variant &r_output, String &r_error) {
	const Variant::Type target = (Variant::Type)p_target_type;
	const Variant::Type src = p_input.get_type();

	// Anything goes where anything is accepted.
	if (target == Variant::NIL) {
		r_output = p_input;
		return true;
	}

	if (src == target) {
		r_output = p_input;
		return true;
	}

	double comp[4] = { 0.0, 0.0, 0.0, 0.0 };

	switch (target) {
		case Variant::BOOL: {
			if (src == Variant::INT || src == Variant::FLOAT) {
				r_output = (bool)((double)p_input != 0.0);
				return true;
			}
		} break;

		case Variant::INT: {
			if (src == Variant::FLOAT) {
				double d = p_input;
				if (d != (double)(int64_t)d) {
					r_error = "expected an integer, got the fractional number " + String::num(d);
					return false;
				}
				r_output = (int64_t)d;
				return true;
			}
			if (src == Variant::BOOL) {
				r_output = (int64_t)((bool)p_input ? 1 : 0);
				return true;
			}
			if (src == Variant::STRING) {
				String s = p_input;
				if (s.is_valid_int()) {
					r_output = s.to_int();
					return true;
				}
			}
		} break;

		case Variant::FLOAT: {
			if (src == Variant::INT) {
				r_output = (double)(int64_t)p_input;
				return true;
			}
			if (src == Variant::STRING) {
				String s = p_input;
				if (s.is_valid_float()) {
					r_output = s.to_float();
					return true;
				}
			}
		} break;

		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			if (src == Variant::STRING || src == Variant::STRING_NAME || src == Variant::NODE_PATH) {
				String s = String(p_input);
				if (target == Variant::STRING) {
					r_output = s;
				} else if (target == Variant::STRING_NAME) {
					r_output = StringName(s);
				} else {
					r_output = NodePath(s);
				}
				return true;
			}
		} break;

		case Variant::VECTOR2:
		case Variant::VECTOR2I: {
			if (read_components(p_input, comp, 2) == 2) {
				if (target == Variant::VECTOR2) {
					r_output = Vector2((float)comp[0], (float)comp[1]);
				} else {
					r_output = Vector2i((int32_t)comp[0], (int32_t)comp[1]);
				}
				return true;
			}
		} break;

		case Variant::VECTOR3:
		case Variant::VECTOR3I: {
			if (read_components(p_input, comp, 3) == 3) {
				if (target == Variant::VECTOR3) {
					r_output = Vector3((float)comp[0], (float)comp[1], (float)comp[2]);
				} else {
					r_output = Vector3i((int32_t)comp[0], (int32_t)comp[1], (int32_t)comp[2]);
				}
				return true;
			}
		} break;

		case Variant::VECTOR4: {
			if (read_components(p_input, comp, 4) == 4) {
				r_output = Vector4((float)comp[0], (float)comp[1], (float)comp[2], (float)comp[3]);
				return true;
			}
		} break;

		case Variant::RECT2:
		case Variant::RECT2I: {
			if (p_input.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_input;
				if (d.has("x") && d.has("y") && d.has("w") && d.has("h")) {
					double x = d["x"], y = d["y"], w = d["w"], h = d["h"];
					if (target == Variant::RECT2) {
						r_output = Rect2((float)x, (float)y, (float)w, (float)h);
					} else {
						r_output = Rect2i((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
					}
					return true;
				}
			}
			if (p_input.get_type() == Variant::ARRAY) {
				Array a = p_input;
				if (a.size() == 4) {
					double x = a[0], y = a[1], w = a[2], h = a[3];
					if (target == Variant::RECT2) {
						r_output = Rect2((float)x, (float)y, (float)w, (float)h);
					} else {
						r_output = Rect2i((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
					}
					return true;
				}
			}
		} break;

		case Variant::COLOR: {
			if (src == Variant::STRING) {
				String s = String(p_input).strip_edges();
				if (s.begins_with("#")) {
					r_output = Color::html(s.substr(1));
					return true;
				}
				r_output = Color::html(s);
				return true;
			}
			if (p_input.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_input;
				if (d.has("hex")) {
					String s = String(d["hex"]);
					r_output = Color::html(s.begins_with("#") ? s.substr(1) : s);
					return true;
				}
				Color c;
				c.r = d.has("r") ? (float)(double)d["r"] : 0.0f;
				c.g = d.has("g") ? (float)(double)d["g"] : 0.0f;
				c.b = d.has("b") ? (float)(double)d["b"] : 0.0f;
				c.a = d.has("a") ? (float)(double)d["a"] : 1.0f;
				r_output = c;
				return true;
			}
			int n = read_components(p_input, comp, 4);
			if (n >= 3) {
				r_output = Color((float)comp[0], (float)comp[1], (float)comp[2], n == 4 ? (float)comp[3] : 1.0f);
				return true;
			}
		} break;

		case Variant::OBJECT: {
			// Resources are addressed by res:// path; null clears the slot.
			if (src == Variant::NIL) {
				r_output = Variant();
				return true;
			}
			String path;
			if (src == Variant::STRING) {
				path = p_input;
			} else if (src == Variant::DICTIONARY) {
				Dictionary d = p_input;
				path = d.has("path") ? String(d["path"]) : String();
			}
			if (!path.is_empty()) {
				if (!ResourceLoader::get_singleton()->exists(path)) {
					r_error = "resource not found: " + path;
					return false;
				}
				Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
				if (res.is_null()) {
					r_error = "failed to load resource: " + path;
					return false;
				}
				r_output = res;
				return true;
			}
		} break;

		case Variant::ARRAY: {
			if (src == Variant::ARRAY) {
				r_output = p_input;
				return true;
			}
		} break;

		case Variant::DICTIONARY: {
			if (src == Variant::DICTIONARY) {
				r_output = p_input;
				return true;
			}
		} break;

		case Variant::PACKED_STRING_ARRAY: {
			if (src == Variant::ARRAY) {
				Array a = p_input;
				PackedStringArray out;
				for (int i = 0; i < a.size(); i++) {
					out.push_back(String(a[i]));
				}
				r_output = out;
				return true;
			}
		} break;

		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY: {
			if (src == Variant::ARRAY) {
				Array a = p_input;
				if (target == Variant::PACKED_INT32_ARRAY) {
					PackedInt32Array out;
					for (int i = 0; i < a.size(); i++) {
						out.push_back((int32_t)(double)a[i]);
					}
					r_output = out;
				} else {
					PackedInt64Array out;
					for (int i = 0; i < a.size(); i++) {
						out.push_back((int64_t)(double)a[i]);
					}
					r_output = out;
				}
				return true;
			}
		} break;

		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY: {
			if (src == Variant::ARRAY) {
				Array a = p_input;
				if (target == Variant::PACKED_FLOAT32_ARRAY) {
					PackedFloat32Array out;
					for (int i = 0; i < a.size(); i++) {
						out.push_back((float)(double)a[i]);
					}
					r_output = out;
				} else {
					PackedFloat64Array out;
					for (int i = 0; i < a.size(); i++) {
						out.push_back((double)a[i]);
					}
					r_output = out;
				}
				return true;
			}
		} break;

		default:
			break;
	}

	r_error = "expected " + type_name(p_target_type) + ", got " + type_name(src);
	return false;
}

String AIOSJson::type_name(int p_type) {
	switch ((Variant::Type)p_type) {
		case Variant::NIL:
			return "null";
		case Variant::BOOL:
			return "bool";
		case Variant::INT:
			return "int";
		case Variant::FLOAT:
			return "float";
		case Variant::STRING:
			return "String";
		case Variant::VECTOR2:
			return "Vector2";
		case Variant::VECTOR2I:
			return "Vector2i";
		case Variant::RECT2:
			return "Rect2";
		case Variant::RECT2I:
			return "Rect2i";
		case Variant::VECTOR3:
			return "Vector3";
		case Variant::VECTOR3I:
			return "Vector3i";
		case Variant::TRANSFORM2D:
			return "Transform2D";
		case Variant::VECTOR4:
			return "Vector4";
		case Variant::VECTOR4I:
			return "Vector4i";
		case Variant::PLANE:
			return "Plane";
		case Variant::QUATERNION:
			return "Quaternion";
		case Variant::AABB:
			return "AABB";
		case Variant::BASIS:
			return "Basis";
		case Variant::TRANSFORM3D:
			return "Transform3D";
		case Variant::PROJECTION:
			return "Projection";
		case Variant::COLOR:
			return "Color";
		case Variant::STRING_NAME:
			return "StringName";
		case Variant::NODE_PATH:
			return "NodePath";
		case Variant::RID:
			return "RID";
		case Variant::OBJECT:
			return "Object";
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Signal";
		case Variant::DICTIONARY:
			return "Dictionary";
		case Variant::ARRAY:
			return "Array";
		case Variant::PACKED_BYTE_ARRAY:
			return "PackedByteArray";
		case Variant::PACKED_INT32_ARRAY:
			return "PackedInt32Array";
		case Variant::PACKED_INT64_ARRAY:
			return "PackedInt64Array";
		case Variant::PACKED_FLOAT32_ARRAY:
			return "PackedFloat32Array";
		case Variant::PACKED_FLOAT64_ARRAY:
			return "PackedFloat64Array";
		case Variant::PACKED_STRING_ARRAY:
			return "PackedStringArray";
		case Variant::PACKED_VECTOR2_ARRAY:
			return "PackedVector2Array";
		case Variant::PACKED_VECTOR3_ARRAY:
			return "PackedVector3Array";
		case Variant::PACKED_COLOR_ARRAY:
			return "PackedColorArray";
		default:
			return "Variant";
	}
}

bool AIOSJson::get_bool(const Dictionary &p_dict, const String &p_key, bool p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	Variant v = p_dict[p_key];
	if (v.get_type() == Variant::BOOL) {
		return (bool)v;
	}
	if (v.get_type() == Variant::INT || v.get_type() == Variant::FLOAT) {
		return (double)v != 0.0;
	}
	return p_default;
}

int64_t AIOSJson::get_int(const Dictionary &p_dict, const String &p_key, int64_t p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	Variant v = p_dict[p_key];
	if (v.get_type() == Variant::INT || v.get_type() == Variant::FLOAT) {
		return (int64_t)(double)v;
	}
	return p_default;
}

String AIOSJson::get_string(const Dictionary &p_dict, const String &p_key, const String &p_default) {
	if (!p_dict.has(p_key)) {
		return p_default;
	}
	Variant v = p_dict[p_key];
	if (v.get_type() == Variant::NIL) {
		return p_default;
	}
	return String(v);
}

Dictionary AIOSJson::get_dict(const Dictionary &p_dict, const String &p_key) {
	if (p_dict.has(p_key) && Variant(p_dict[p_key]).get_type() == Variant::DICTIONARY) {
		return p_dict[p_key];
	}
	return Dictionary();
}

Array AIOSJson::get_array(const Dictionary &p_dict, const String &p_key) {
	if (p_dict.has(p_key) && Variant(p_dict[p_key]).get_type() == Variant::ARRAY) {
		return p_dict[p_key];
	}
	return Array();
}

Dictionary AIOSJson::ok(const Dictionary &p_result) {
	Dictionary d;
	d["ok"] = true;
	d["result"] = p_result;
	return d;
}

Dictionary AIOSJson::error(const String &p_code, const String &p_message, const Dictionary &p_details) {
	Dictionary err;
	err["code"] = p_code;
	err["message"] = p_message;
	if (!p_details.is_empty()) {
		err["details"] = p_details;
	}
	Dictionary d;
	d["ok"] = false;
	d["error"] = err;
	return d;
}
