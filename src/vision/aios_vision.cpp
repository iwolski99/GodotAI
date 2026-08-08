/**************************************************************************/
/*  aios_vision.cpp                                                       */
/**************************************************************************/

#include "aios_vision.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>

#define AIOS_SHOT_DIR "user://godot_ai_os/screenshots"

// 1568px on the long edge is where Anthropic stops gaining accuracy and starts
// charging for pixels it downsamples anyway. 1024 is the default here because a
// viewport screenshot is being read for layout and composition, not fine detail,
// and it costs roughly half as many tokens.
#define AIOS_DEFAULT_MAX_EDGE 1024
#define AIOS_HARD_MAX_EDGE 1568

void AIOSVision::_bind_methods() {
	ClassDB::bind_static_method("AIOSVision",
			D_METHOD("capture_viewport_screenshot", "params"), &AIOSVision::capture_viewport_screenshot);
	ClassDB::bind_static_method("AIOSVision", D_METHOD("is_available"), &AIOSVision::is_available);
}

bool AIOSVision::is_available() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return false;
	}
	SubViewport *vp = ei->get_editor_viewport_3d(0);
	if (vp == nullptr) {
		return false;
	}
	Ref<Texture2D> tex = vp->get_texture();
	return tex.is_valid();
}

Dictionary AIOSVision::_encode(const Ref<Image> &p_image, const Dictionary &p_params, const String &p_source) {
	if (p_image.is_null() || p_image->is_empty()) {
		return AIOSJson::error("no_frame",
				"The editor viewport has not drawn a frame that can be read back. This is normal in "
				"--headless, where nothing is rendered at all.");
	}

	Ref<Image> image = p_image->duplicate();

	const int original_width = image->get_width();
	const int original_height = image->get_height();

	int64_t max_edge = AIOSJson::get_int(p_params, "max_edge", AIOS_DEFAULT_MAX_EDGE);
	if (max_edge <= 0) {
		max_edge = AIOS_DEFAULT_MAX_EDGE;
	}
	if (max_edge > AIOS_HARD_MAX_EDGE) {
		max_edge = AIOS_HARD_MAX_EDGE;
	}

	const int longest = original_width > original_height ? original_width : original_height;
	bool resized = false;
	if (longest > max_edge) {
		const double factor = (double)max_edge / (double)longest;
		image->resize((int)(original_width * factor), (int)(original_height * factor), Image::INTERPOLATE_BILINEAR);
		resized = true;
	}

	// The viewport is RGBA and the alpha is meaningless here — it is the
	// editor's clear colour, not transparency anyone asked about. Flattening to
	// RGB8 keeps a model from reading a checkerboard that is not in the game.
	if (image->get_format() != Image::FORMAT_RGB8) {
		image->convert(Image::FORMAT_RGB8);
	}

	const PackedByteArray png = image->save_png_to_buffer();
	if (png.is_empty()) {
		return AIOSJson::error("encode_failed", "The frame could not be encoded as PNG.");
	}

	Dictionary result;
	result["source"] = p_source;
	result["width"] = image->get_width();
	result["height"] = image->get_height();
	result["original_width"] = original_width;
	result["original_height"] = original_height;
	result["resized"] = resized;
	result["bytes"] = png.size();
	result["media_type"] = "image/png";

	// Always write the file. It costs a few milliseconds, and it means a human
	// reading the log can open the exact frame the model was looking at.
	String save_to = AIOSJson::get_string(p_params, "save_to", "");
	if (save_to.is_empty()) {
		if (!DirAccess::dir_exists_absolute(AIOS_SHOT_DIR)) {
			DirAccess::make_dir_recursive_absolute(AIOS_SHOT_DIR);
		}
		save_to = String(AIOS_SHOT_DIR) + "/" + p_source + "_" +
				String::num_uint64(Time::get_singleton()->get_ticks_msec()) + ".png";
	} else {
		const String dir = save_to.get_base_dir();
		if (!dir.is_empty() && !DirAccess::dir_exists_absolute(dir)) {
			DirAccess::make_dir_recursive_absolute(dir);
		}
	}

	Ref<FileAccess> file = FileAccess::open(save_to, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_buffer(png);
		file->close();
		result["path"] = save_to;
	} else {
		result["path"] = Variant();
		result["note"] = "The image could not be written to '" + save_to + "', but it was captured.";
	}

	if (AIOSJson::get_bool(p_params, "include_base64", true)) {
		result["image_base64"] = Marshalls::get_singleton()->raw_to_base64(png);
	}

	return AIOSJson::ok(result);
}

Dictionary AIOSVision::capture_viewport_screenshot(const Dictionary &p_params) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "This tool only works inside the Godot editor.");
	}

	const String which = AIOSJson::get_string(p_params, "viewport", "3d").to_lower();

	SubViewport *vp = nullptr;
	String source;
	if (which == "2d") {
		vp = ei->get_editor_viewport_2d();
		source = "viewport2d";
	} else if (which == "3d") {
		const int64_t index = AIOSJson::get_int(p_params, "index", 0);
		vp = ei->get_editor_viewport_3d((int32_t)(index >= 0 && index < 4 ? index : 0));
		source = "viewport3d";
	} else {
		return AIOSJson::error("unknown_viewport",
				"'viewport' must be \"3d\" or \"2d\". Got '" + which + "'.");
	}

	if (vp == nullptr) {
		return AIOSJson::error("viewport_unavailable",
				"The editor has no " + which + " viewport right now. Switch the editor to that mode first.");
	}

	Ref<Texture2D> tex = vp->get_texture();
	if (tex.is_null()) {
		return AIOSJson::error("no_frame",
				"The " + which + " viewport has no texture. In --headless nothing is rendered, so there is "
				"no frame to capture.");
	}

	return _encode(tex->get_image(), p_params, source);
}
