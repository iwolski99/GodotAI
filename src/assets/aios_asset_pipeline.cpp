/**************************************************************************/
/*  aios_asset_pipeline.cpp                                               */
/**************************************************************************/

#include "aios_asset_pipeline.h"

#include "../util/aios_json.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// Provider endpoints, kept in one table rather than scattered through the code.
//
// IMPORTANT, and stated plainly because it affects whether this works for you:
// these request and response shapes were written from the providers' published
// API documentation but have NOT been executed against the live services — the
// development environment for this plugin has no outbound access to them and no
// API keys. Treat this table as the thing to check first if a generation call
// fails with a parse error rather than a network error.
//
// The shapes are deliberately isolated here so fixing one is a five-line edit
// and not an archaeology expedition.
struct AssetProviderSpec {
	const char *id;
	const char *create_url;
	const char *status_url_prefix; // task id is appended
	const char *auth_scheme; // "Bearer" for both current providers
};

static const AssetProviderSpec ASSET_PROVIDERS[] = {
	{ "meshy",
			"https://api.meshy.ai/openapi/v2/text-to-3d",
			"https://api.meshy.ai/openapi/v2/text-to-3d/",
			"Bearer" },
	{ "tripo",
			"https://api.tripo3d.ai/v2/openapi/task",
			"https://api.tripo3d.ai/v2/openapi/task/",
			"Bearer" },
};

static const int ASSET_PROVIDER_COUNT = sizeof(ASSET_PROVIDERS) / sizeof(AssetProviderSpec);

static const AssetProviderSpec *find_provider(const String &p_id) {
	for (int i = 0; i < ASSET_PROVIDER_COUNT; i++) {
		if (p_id == ASSET_PROVIDERS[i].id) {
			return &ASSET_PROVIDERS[i];
		}
	}
	return nullptr;
}

void AIOSAssetPipeline::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_request_completed", "result", "code", "headers", "body"),
			&AIOSAssetPipeline::_on_request_completed);
	ClassDB::bind_method(D_METHOD("_on_download_completed", "result", "code", "headers", "body"),
			&AIOSAssetPipeline::_on_download_completed);

	ClassDB::bind_method(D_METHOD("generate_3d_asset", "params"), &AIOSAssetPipeline::generate_3d_asset);
	ClassDB::bind_method(D_METHOD("import_from_url", "params"), &AIOSAssetPipeline::import_from_url);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &AIOSAssetPipeline::poll);
	ClassDB::bind_method(D_METHOD("cancel"), &AIOSAssetPipeline::cancel);
	ClassDB::bind_method(D_METHOD("is_busy"), &AIOSAssetPipeline::is_busy);
	ClassDB::bind_method(D_METHOD("get_status"), &AIOSAssetPipeline::get_status);
	ClassDB::bind_static_method("AIOSAssetPipeline", D_METHOD("import_file", "res_path"), &AIOSAssetPipeline::import_file);

	ADD_SIGNAL(MethodInfo("asset_stage_changed", PropertyInfo(Variant::STRING, "stage"), PropertyInfo(Variant::STRING, "detail")));
	ADD_SIGNAL(MethodInfo("asset_ready", PropertyInfo(Variant::DICTIONARY, "result")));
	ADD_SIGNAL(MethodInfo("asset_failed", PropertyInfo(Variant::DICTIONARY, "error")));
	ADD_SIGNAL(MethodInfo("asset_log", PropertyInfo(Variant::STRING, "level"), PropertyInfo(Variant::STRING, "message")));
}

void AIOSAssetPipeline::_ready() {
	http = memnew(HTTPRequest);
	http->set_name("AssetApiRequest");
	http->set_timeout(120.0);
	http->set_use_threads(true);
	add_child(http);
	http->connect("request_completed", Callable(this, "_on_request_completed"));

	// A separate HTTPRequest for the payload: a mesh download can take minutes
	// and must not block the status polling that is happening alongside it.
	download = memnew(HTTPRequest);
	download->set_name("AssetDownload");
	download->set_timeout(600.0);
	download->set_use_threads(true);
	add_child(download);
	download->connect("request_completed", Callable(this, "_on_download_completed"));
}

void AIOSAssetPipeline::setup(const Ref<AIOSCredentials> &p_credentials) {
	credentials = p_credentials;
}

String AIOSAssetPipeline::get_stage_name() const {
	switch (stage) {
		case STAGE_REQUESTING:
			return "REQUESTING";
		case STAGE_POLLING:
			return "POLLING";
		case STAGE_DOWNLOADING:
			return "DOWNLOADING";
		case STAGE_IMPORTING:
			return "IMPORTING";
		case STAGE_POSTPROCESSING:
			return "POSTPROCESSING";
		default:
			return "IDLE";
	}
}

Dictionary AIOSAssetPipeline::get_status() const {
	Dictionary d;
	d["stage"] = get_stage_name();
	d["provider"] = provider;
	d["prompt"] = prompt;
	d["task_id"] = task_id;
	d["elapsed_sec"] = elapsed;
	d["polls"] = poll_count;
	return d;
}

void AIOSAssetPipeline::_set_stage(Stage p_stage, const String &p_detail) {
	stage = p_stage;
	emit_signal("asset_stage_changed", get_stage_name(), p_detail);
}

void AIOSAssetPipeline::_fail(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["provider"] = provider;
	error["task_id"] = task_id;
	_set_stage(STAGE_IDLE, p_code);
	emit_signal("asset_failed", error);
}

void AIOSAssetPipeline::_finish(const Dictionary &p_result) {
	_set_stage(STAGE_IDLE, "done");
	emit_signal("asset_ready", p_result);
}

/* -------------------------------------------------------------------------- */
/*  Starting a generation                                                      */
/* -------------------------------------------------------------------------- */

Dictionary AIOSAssetPipeline::generate_3d_asset(const Dictionary &p_params) {
	if (stage != STAGE_IDLE) {
		return AIOSJson::error("already_running",
				"An asset job is already in flight (" + get_stage_name() + "). Wait for it or cancel it.");
	}

	provider = AIOSJson::get_string(p_params, "provider", "meshy").to_lower();
	const AssetProviderSpec *spec = find_provider(provider);
	if (spec == nullptr) {
		Dictionary details;
		Array known;
		for (int i = 0; i < ASSET_PROVIDER_COUNT; i++) {
			known.push_back(ASSET_PROVIDERS[i].id);
		}
		details["available"] = known;
		return AIOSJson::error("unknown_provider",
				"No generation provider called '" + provider + "'.", details);
	}

	prompt = AIOSJson::get_string(p_params, "prompt", "");
	if (prompt.strip_edges().is_empty()) {
		return AIOSJson::error("missing_parameter", "'prompt' is required — describe the model you want.");
	}

	if (credentials.is_null() || !credentials->has_key(provider)) {
		return AIOSJson::error("no_api_key",
				"No API key for " + provider + ". Add one in the AI Agent dock's Settings, or set " +
						AIOSCredentials::env_var_for(provider) + ".");
	}

	asset_name = AIOSJson::get_string(p_params, "name", "");
	if (asset_name.strip_edges().is_empty()) {
		// Derive a filename from the prompt: first few words, safe characters.
		String slug = prompt.to_lower();
		String cleaned;
		for (int i = 0; i < slug.length() && cleaned.length() < 40; i++) {
			const char32_t ch = slug[i];
			if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
				cleaned += String::chr(ch);
			} else if (!cleaned.is_empty() && !cleaned.ends_with("_")) {
				cleaned += "_";
			}
		}
		asset_name = cleaned.is_empty() ? String("generated_asset") : cleaned.trim_suffix("_");
	}

	target_dir = AIOSJson::get_string(p_params, "target_dir", "res://assets/generated");
	want_cleanup = AIOSJson::get_bool(p_params, "cleanup", true);
	target_triangles = AIOSJson::get_int(p_params, "target_triangles", 0);
	collision_mode = AIOSJson::get_string(p_params, "collision", "convex");
	art_style = AIOSJson::get_string(p_params, "art_style", "realistic");
	timeout_sec = (double)AIOSJson::get_int(p_params, "timeout_sec", 600);
	if (timeout_sec <= 0.0) {
		timeout_sec = 600.0;
	}

	task_id = "";
	pending_url = "";
	downloaded_path = "";
	elapsed = 0.0;
	poll_accumulator = 0.0;
	poll_count = 0;

	const Error err = _begin_generation();
	if (err != OK) {
		_set_stage(STAGE_IDLE, "request failed");
		return AIOSJson::error("request_failed", "Could not send the generation request to " + provider + ".");
	}

	Dictionary result;
	result["started"] = true;
	result["provider"] = provider;
	result["prompt"] = prompt;
	result["name"] = asset_name;
	result["target_dir"] = target_dir;
	result["note"] = "Generation is asynchronous and typically takes 30-120 seconds. The finished asset "
					 "arrives as an 'asset_ready' event with the res:// path of the imported scene.";
	return AIOSJson::ok(result);
}

Error AIOSAssetPipeline::_begin_generation() {
	const AssetProviderSpec *spec = find_provider(provider);
	if (spec == nullptr || http == nullptr) {
		return FAILED;
	}

	const String key = credentials->get_key(provider);

	PackedStringArray headers;
	headers.push_back("content-type: application/json");
	headers.push_back("authorization: " + String(spec->auth_scheme) + " " + key);

	Dictionary body;
	if (provider == "meshy") {
		body["mode"] = "preview";
		body["prompt"] = prompt;
		body["art_style"] = art_style;
		body["should_remesh"] = true;
		if (target_triangles > 0) {
			// Asking the provider for a budget beats decimating afterwards: the
			// remesher knows the topology and preserves silhouette better than a
			// blind decimate ever will.
			body["target_polycount"] = target_triangles;
		}
	} else if (provider == "tripo") {
		body["type"] = "text_to_model";
		body["prompt"] = prompt;
		if (target_triangles > 0) {
			body["face_limit"] = target_triangles;
		}
	}

	_set_stage(STAGE_REQUESTING, provider + ": " + prompt.substr(0, 60));
	emit_signal("asset_log", "info", "Asking " + provider + " to generate: " + prompt);

	return http->request(spec->create_url, headers, HTTPClient::METHOD_POST, JSON::stringify(body));
}

Error AIOSAssetPipeline::_poll_task() {
	const AssetProviderSpec *spec = find_provider(provider);
	if (spec == nullptr || http == nullptr || task_id.is_empty()) {
		return FAILED;
	}
	PackedStringArray headers;
	headers.push_back("authorization: " + String(spec->auth_scheme) + " " + credentials->get_key(provider));
	poll_count++;
	return http->request(String(spec->status_url_prefix) + task_id, headers, HTTPClient::METHOD_GET);
}

/* -------------------------------------------------------------------------- */
/*  Responses                                                                  */
/* -------------------------------------------------------------------------- */

void AIOSAssetPipeline::_on_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	(void)p_headers;

	if (p_result != HTTPRequest::RESULT_SUCCESS) {
		_fail("network_error",
				"Could not reach " + provider + " (result " + String::num_int64(p_result) +
						"). Check your connection and any proxy settings.");
		return;
	}

	const String text = p_body.get_string_from_utf8();
	Variant parsed = JSON::parse_string(text);
	Dictionary body = parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();

	if (p_code < 200 || p_code >= 300) {
		String message = "HTTP " + String::num_int64(p_code) + " from " + provider;
		if (body.has("message")) {
			message += ": " + String(body["message"]);
		} else if (body.has("error")) {
			message += ": " + JSON::stringify(body["error"]);
		} else if (!text.strip_edges().is_empty()) {
			message += ": " + text.substr(0, 300);
		}
		if (p_code == 401 || p_code == 403) {
			message += " (the API key was rejected)";
		}
		_fail("api_error", message);
		return;
	}

	// --- the create call: pull out a task id -------------------------------
	if (stage == STAGE_REQUESTING) {
		String id;
		if (provider == "meshy") {
			// Documented as {"result": "<task id>"}.
			id = String(body.get("result", ""));
			if (id.is_empty() && body.has("id")) {
				id = String(body["id"]);
			}
		} else if (provider == "tripo") {
			Dictionary data = body.get("data", Dictionary());
			id = String(data.get("task_id", ""));
		}

		if (id.is_empty()) {
			_fail("unexpected_response",
					"The generation request succeeded but no task id could be found in the reply. The provider's "
					"response shape has probably changed; see ASSET_PROVIDERS in aios_asset_pipeline.cpp. "
					"Raw reply: " + text.substr(0, 300));
			return;
		}

		task_id = id;
		poll_accumulator = 0.0;
		_set_stage(STAGE_POLLING, "task " + task_id);
		emit_signal("asset_log", "info", "Generation queued as task " + task_id + ".");
		return;
	}

	// --- a status poll -----------------------------------------------------
	if (stage != STAGE_POLLING) {
		return;
	}

	String status;
	String model_url;
	double progress = -1.0;

	if (provider == "meshy") {
		status = String(body.get("status", "")).to_upper();
		if (body.has("progress")) {
			progress = (double)body["progress"];
		}
		Dictionary urls = body.get("model_urls", Dictionary());
		model_url = String(urls.get("glb", ""));
	} else if (provider == "tripo") {
		Dictionary data = body.get("data", Dictionary());
		status = String(data.get("status", "")).to_upper();
		if (data.has("progress")) {
			progress = (double)data["progress"];
		}
		Dictionary output = data.get("output", Dictionary());
		model_url = String(output.get("pbr_model", ""));
		if (model_url.is_empty()) {
			model_url = String(output.get("model", ""));
		}
	}

	if (status == "FAILED" || status == "FAILURE" || status == "ERROR" || status == "BANNED") {
		String reason = String(body.get("task_error", ""));
		if (reason.is_empty()) {
			reason = text.substr(0, 300);
		}
		_fail("generation_failed", provider + " could not generate that model: " + reason);
		return;
	}

	if (status == "SUCCEEDED" || status == "SUCCESS" || status == "COMPLETED") {
		if (model_url.is_empty()) {
			_fail("no_model_url",
					"The job finished but the reply carried no downloadable model URL. Raw reply: " +
							text.substr(0, 300));
			return;
		}
		emit_signal("asset_log", "success", "Generation finished; downloading the mesh.");
		if (_begin_download(model_url) != OK) {
			_fail("download_failed", "Could not start downloading '" + model_url + "'.");
		}
		return;
	}

	// Still working. Report progress so the human is not staring at nothing.
	if (progress >= 0.0) {
		_set_stage(STAGE_POLLING, "generating, " + String::num(progress, 0) + "%");
	}
}

Error AIOSAssetPipeline::_begin_download(const String &p_url) {
	if (download == nullptr) {
		return FAILED;
	}
	pending_url = p_url;
	_set_stage(STAGE_DOWNLOADING, p_url.get_file());
	return download->request(p_url, PackedStringArray(), HTTPClient::METHOD_GET);
}

void AIOSAssetPipeline::_on_download_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	(void)p_headers;

	if (p_result != HTTPRequest::RESULT_SUCCESS) {
		_fail("download_failed",
				"The model URL could not be downloaded (result " + String::num_int64(p_result) + ").");
		return;
	}
	if (p_code < 200 || p_code >= 300) {
		_fail("download_failed", "HTTP " + String::num_int64(p_code) + " downloading the model.");
		return;
	}
	if (p_body.is_empty()) {
		_fail("download_empty", "The download completed but returned no data.");
		return;
	}

	Dictionary written = _write_and_import(p_body, pending_url);
	if (!(bool)written["ok"]) {
		Dictionary error = written["error"];
		_fail(String(error["code"]), String(error["message"]));
		return;
	}

	Dictionary result = written["result"];
	result["provider"] = provider;
	result["prompt"] = prompt;
	result["task_id"] = task_id;
	result["elapsed_sec"] = elapsed;

	// Post-processing is a separate, optional step run through the Blender
	// bridge. It is reported rather than performed here because Blender is an
	// external process the plugin does not own.
	if (want_cleanup) {
		result["cleanup_requested"] = true;
		result["cleanup_hint"] = "Run blender_cleanup on this file to reduce triangles and tag collision meshes.";
		result["target_triangles"] = target_triangles;
		result["collision"] = collision_mode;
	}

	_finish(result);
}

/* -------------------------------------------------------------------------- */
/*  Writing into the project                                                   */
/* -------------------------------------------------------------------------- */

Dictionary AIOSAssetPipeline::_write_and_import(const PackedByteArray &p_bytes, const String &p_url) {
	_set_stage(STAGE_IMPORTING, asset_name);

	if (!target_dir.begins_with("res://")) {
		return AIOSJson::error("invalid_target", "'target_dir' must be under res://. Got '" + target_dir + "'.");
	}
	if (!DirAccess::dir_exists_absolute(target_dir)) {
		if (DirAccess::make_dir_recursive_absolute(target_dir) != OK) {
			return AIOSJson::error("directory_failed", "Could not create '" + target_dir + "'.");
		}
	}

	// Trust the URL's extension only if it is one Godot can import; a signed
	// download URL often ends in a query string rather than a filename.
	String extension = p_url.get_file().get_extension().to_lower();
	if (extension.contains("?")) {
		extension = extension.substr(0, extension.find("?"));
	}
	if (extension != "glb" && extension != "gltf" && extension != "fbx" && extension != "obj") {
		extension = "glb";
	}

	const String path = target_dir + "/" + asset_name + "." + extension;

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return AIOSJson::error("write_failed", "Could not write '" + path + "'.");
	}
	file->store_buffer(p_bytes);
	file->close();

	downloaded_path = path;
	emit_signal("asset_log", "info",
			"Wrote " + String::num_int64(p_bytes.size() / 1024) + " KB to " + path + "; importing.");

	Dictionary imported = import_file(path);

	Dictionary result;
	result["path"] = path;
	result["bytes"] = p_bytes.size();
	result["format"] = extension;
	result["imported"] = (bool)imported["ok"];
	if (!(bool)imported["ok"]) {
		Dictionary error = imported["error"];
		result["import_note"] = error["message"];
	}
	result["source_url"] = p_url;
	return AIOSJson::ok(result);
}

Dictionary AIOSAssetPipeline::import_file(const String &p_res_path) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei == nullptr) {
		return AIOSJson::error("editor_unavailable", "Importing requires the editor.");
	}
	EditorFileSystem *efs = ei->get_resource_filesystem();
	if (efs == nullptr) {
		return AIOSJson::error("filesystem_unavailable", "The editor filesystem is not available.");
	}

	if (!FileAccess::file_exists(p_res_path)) {
		return AIOSJson::error("file_not_found", "No file at '" + p_res_path + "'.");
	}

	// update_file registers the new file; the scan is what actually runs the
	// importer. Without both, the mesh sits in the project folder invisible to
	// the editor and unloadable by path.
	efs->update_file(p_res_path);
	efs->scan();

	Dictionary result;
	result["path"] = p_res_path;
	result["queued"] = true;
	result["note"] = "The editor imports asynchronously. The resource is usually loadable within a second or "
					 "two; if a load fails immediately after this call, retry once before treating it as an error.";
	return AIOSJson::ok(result);
}

Dictionary AIOSAssetPipeline::import_from_url(const Dictionary &p_params) {
	if (stage != STAGE_IDLE) {
		return AIOSJson::error("already_running", "An asset job is already in flight.");
	}

	const String url = AIOSJson::get_string(p_params, "url", "");
	if (url.is_empty()) {
		return AIOSJson::error("missing_parameter", "'url' is required.");
	}
	if (!url.begins_with("http://") && !url.begins_with("https://")) {
		return AIOSJson::error("invalid_url", "'url' must be an http(s) URL.");
	}

	provider = "direct";
	prompt = "";
	task_id = "";
	asset_name = AIOSJson::get_string(p_params, "name", "");
	if (asset_name.strip_edges().is_empty()) {
		asset_name = url.get_file().get_basename();
		if (asset_name.is_empty()) {
			asset_name = "downloaded_asset";
		}
	}
	target_dir = AIOSJson::get_string(p_params, "target_dir", "res://assets/generated");
	want_cleanup = AIOSJson::get_bool(p_params, "cleanup", false);
	target_triangles = AIOSJson::get_int(p_params, "target_triangles", 0);
	collision_mode = AIOSJson::get_string(p_params, "collision", "convex");
	elapsed = 0.0;
	timeout_sec = (double)AIOSJson::get_int(p_params, "timeout_sec", 600);

	if (_begin_download(url) != OK) {
		_set_stage(STAGE_IDLE, "download failed");
		return AIOSJson::error("download_failed", "Could not start the download.");
	}

	Dictionary result;
	result["started"] = true;
	result["url"] = url;
	result["name"] = asset_name;
	result["note"] = "The imported path arrives as an 'asset_ready' event.";
	return AIOSJson::ok(result);
}

/* -------------------------------------------------------------------------- */
/*  Driving                                                                    */
/* -------------------------------------------------------------------------- */

void AIOSAssetPipeline::poll(double p_delta) {
	if (stage == STAGE_IDLE) {
		return;
	}
	elapsed += p_delta;

	if (elapsed > timeout_sec) {
		cancel();
		_fail("timeout",
				"The asset job did not finish within " + String::num(timeout_sec, 0) +
						"s. Generation services queue under load; try again, or raise timeout_sec.");
		return;
	}

	if (stage == STAGE_POLLING) {
		poll_accumulator += p_delta;
		if (poll_accumulator >= poll_interval) {
			poll_accumulator = 0.0;
			_poll_task();
		}
	}
}

void AIOSAssetPipeline::cancel() {
	if (http != nullptr) {
		http->cancel_request();
	}
	if (download != nullptr) {
		download->cancel_request();
	}
	if (stage != STAGE_IDLE) {
		_set_stage(STAGE_IDLE, "cancelled");
	}
}
