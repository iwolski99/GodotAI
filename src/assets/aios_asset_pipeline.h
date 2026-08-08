/**************************************************************************/
/*  aios_asset_pipeline.h                                                 */
/*  Getting a generated asset from a URL into a usable Godot resource.    */
/**************************************************************************/

#pragma once

#include "../agent/aios_credentials.h"

#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// The unglamorous half of generative assets.
//
// Every text-to-3D service hands back a URL. Between that URL and something a
// game can actually use sit a series of steps that each fail quietly:
//
//   download -> write into res:// -> wait for Godot to import it -> generate
//   collision -> hand back a path that resolves
//
// Skipping any of them produces a mesh that is in the project folder and
// invisible to the editor, or visible but with no collision, or with collision
// the player falls through because the importer defaulted to a convex hull on
// concave geometry.
//
// Two facts drive the design:
//
//   * Godot generates collision from *mesh name suffixes* at import time.
//     A mesh named `Wall-col` gets a concave trimesh body; `-convcol` gets a
//     convex one; `-colonly` becomes collision with no visual. This is the
//     supported path, and it is why the Blender post-process step renames
//     meshes rather than trying to build CollisionShape3D nodes afterwards.
//
//   * The editor imports asynchronously. A .glb written to res:// is not
//     loadable until the filesystem scan that follows notices it, so the
//     pipeline waits for that rather than racing it.
//
// Everything here is asynchronous for the same reason the LLM client is: a
// 30 MB mesh download must not freeze the editor.
class AIOSAssetPipeline : public Node {
	GDCLASS(AIOSAssetPipeline, Node)

public:
	enum Stage {
		STAGE_IDLE,
		STAGE_REQUESTING, // asking the provider to generate
		STAGE_POLLING, // waiting for the job to finish
		STAGE_DOWNLOADING,
		STAGE_IMPORTING,
		STAGE_POSTPROCESSING, // Blender cleanup, if enabled
	};

private:
	HTTPRequest *http = nullptr;
	HTTPRequest *download = nullptr;
	Ref<AIOSCredentials> credentials;

	Stage stage = STAGE_IDLE;
	String provider;
	String prompt;
	String target_dir = "res://assets/generated";
	String task_id;
	String asset_name;
	String pending_url;
	String downloaded_path;

	double poll_accumulator = 0.0;
	double poll_interval = 5.0;
	double elapsed = 0.0;
	double timeout_sec = 600.0;
	int poll_count = 0;

	// Post-processing request carried through the async chain.
	bool want_cleanup = false;
	int64_t target_triangles = 0;
	String collision_mode = "convex";
	String art_style = "realistic";

	void _on_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_download_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);

	void _set_stage(Stage p_stage, const String &p_detail);
	void _fail(const String &p_code, const String &p_message);
	void _finish(const Dictionary &p_result);

	Error _begin_generation();
	Error _poll_task();
	Error _begin_download(const String &p_url);
	Dictionary _write_and_import(const PackedByteArray &p_bytes, const String &p_url);

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void setup(const Ref<AIOSCredentials> &p_credentials);

	// Kicks off a text-to-3D generation. p_params:
	//   {provider, prompt, name, target_dir, cleanup, target_triangles,
	//    collision, timeout_sec}
	Dictionary generate_3d_asset(const Dictionary &p_params);

	// Downloads a URL straight into the project and imports it. This is the half
	// of the pipeline that works with any provider, including one this plugin
	// has never heard of — an agent can call a service itself and hand the URL
	// here.
	Dictionary import_from_url(const Dictionary &p_params);

	// Called every frame by the plugin; drives polling and the timeout.
	void poll(double p_delta);

	void cancel();
	bool is_busy() const { return stage != STAGE_IDLE; }
	String get_stage_name() const;
	Dictionary get_status() const;

	// Asks the editor to (re)import a file that was written to res:// outside
	// the editor's knowledge, and waits for it to become loadable.
	static Dictionary import_file(const String &p_res_path);
};
