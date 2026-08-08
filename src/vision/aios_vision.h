/**************************************************************************/
/*  aios_vision.h                                                         */
/*  Editor viewport capture: the Observe stage with eyes.                 */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// run_playtest tells an agent whether the game *errored*. It cannot tell it
// whether the game looks right — whether the level is lit, whether the player is
// inside a wall, whether the UI is stacked in the corner. Those are the failures
// that produce no diagnostic at all, and they are most of what goes wrong when a
// model builds a 3D scene it cannot see.
//
// So this grabs the editor's own viewport texture and hands it back as a PNG the
// model can actually look at. It closes the loop that text alone leaves open.
//
// Two things worth knowing about the implementation:
//
//   * The capture is the last frame the editor drew. It is not re-rendered on
//     demand — forcing a draw from inside a tool call re-enters the renderer at
//     a moment it does not expect. In practice this is what you want anyway: it
//     is exactly what the human is looking at.
//
//   * Images are downscaled before they are encoded. A 4K viewport is ~8 MB of
//     base64, which is thousands of tokens and a slower request for no extra
//     information — every vision model downsamples aggressively anyway.
class AIOSVision : public RefCounted {
	GDCLASS(AIOSVision, RefCounted)

private:
	static Dictionary _encode(const Ref<Image> &p_image, const Dictionary &p_params, const String &p_source);

protected:
	static void _bind_methods();

public:
	// p_params: {viewport: "3d"|"2d", max_edge: 1024, save_to: "res://...",
	//            include_base64: true}
	static Dictionary capture_viewport_screenshot(const Dictionary &p_params);

	// True when the editor is running with a renderer that can produce a frame.
	// Headless has no drawn viewport, so capture fails cleanly rather than
	// returning a black rectangle the model would try to interpret.
	static bool is_available();
};
