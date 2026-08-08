/**************************************************************************/
/*  aios_credentials.cpp                                                  */
/**************************************************************************/

#include "aios_credentials.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/hashing_context.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#define AIOS_CRED_DIR "user://godot_ai_os"
#define AIOS_CRED_FILE AIOS_CRED_DIR "/credentials.enc"

void AIOSCredentials::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_key", "provider"), &AIOSCredentials::get_key);
	ClassDB::bind_method(D_METHOD("set_key", "provider", "key"), &AIOSCredentials::set_key);
	ClassDB::bind_method(D_METHOD("clear_key", "provider"), &AIOSCredentials::clear_key);
	ClassDB::bind_method(D_METHOD("has_key", "provider"), &AIOSCredentials::has_key);
	ClassDB::bind_method(D_METHOD("is_from_environment", "provider"), &AIOSCredentials::is_from_environment);
	ClassDB::bind_method(D_METHOD("describe"), &AIOSCredentials::describe);

	// Exposed so a GDScript extension can render key status the same way the
	// dock does — in particular, so nobody reimplements redaction badly.
	ClassDB::bind_static_method("AIOSCredentials", D_METHOD("redact", "key"), &AIOSCredentials::redact);
	ClassDB::bind_static_method("AIOSCredentials", D_METHOD("env_var_for", "provider"), &AIOSCredentials::env_var_for);

	ADD_SIGNAL(MethodInfo("credentials_changed", PropertyInfo(Variant::STRING, "provider")));
}

// The provider table. Model providers and asset providers share one store on
// purpose: they are the same kind of secret with the same failure mode, and a
// second half-implemented credential path is how one of them ends up in
// project.godot.
struct ProviderInfo {
	const char *id;
	const char *env_var;
	const char *kind; // "model" | "asset"
	const char *label;
};

static const ProviderInfo AIOS_PROVIDERS[] = {
	{ "anthropic", "ANTHROPIC_API_KEY", "model", "Anthropic (Claude)" },
	{ "openrouter", "OPENROUTER_API_KEY", "model", "OpenRouter" },
	{ "meshy", "MESHY_API_KEY", "asset", "Meshy (text/image to 3D)" },
	{ "tripo", "TRIPO_API_KEY", "asset", "Tripo3D (text/image to 3D)" },
	{ "elevenlabs", "ELEVENLABS_API_KEY", "asset", "ElevenLabs (voice, SFX)" },
	{ "openai", "OPENAI_API_KEY", "asset", "OpenAI (images, audio)" },
};

static const int AIOS_PROVIDER_COUNT = sizeof(AIOS_PROVIDERS) / sizeof(ProviderInfo);

String AIOSCredentials::env_var_for(const String &p_provider) {
	for (int i = 0; i < AIOS_PROVIDER_COUNT; i++) {
		if (p_provider == AIOS_PROVIDERS[i].id) {
			return AIOS_PROVIDERS[i].env_var;
		}
	}
	return String();
}

PackedStringArray AIOSCredentials::known_providers(const String &p_kind) {
	PackedStringArray out;
	for (int i = 0; i < AIOS_PROVIDER_COUNT; i++) {
		if (p_kind.is_empty() || p_kind == AIOS_PROVIDERS[i].kind) {
			out.push_back(AIOS_PROVIDERS[i].id);
		}
	}
	return out;
}

String AIOSCredentials::_store_path() {
	return AIOS_CRED_FILE;
}

String AIOSCredentials::_passphrase() {
	// Bind the ciphertext to this machine and this OS user. Copying the file to
	// another computer, or handing it to a different account, yields a file that
	// will not decrypt — which is the property worth having when the risk is a
	// backup or a synced folder walking off with it.
	OS *os = OS::get_singleton();
	String seed = "godot-ai-os/credentials/v1";
	seed += "|" + os->get_unique_id();
	seed += "|" + os->get_data_dir();

	// Hash rather than using the seed directly: get_unique_id() can be a machine
	// identifier a user would not want written into a file verbatim.
	Ref<HashingContext> ctx;
	ctx.instantiate();
	if (ctx->start(HashingContext::HASH_SHA256) != OK) {
		return seed;
	}
	ctx->update(seed.to_utf8_buffer());
	return ctx->finish().hex_encode();
}

void AIOSCredentials::_ensure_loaded() {
	if (loaded) {
		return;
	}
	loaded = true;

	if (!FileAccess::file_exists(_store_path())) {
		return;
	}

	Ref<FileAccess> file = FileAccess::open_encrypted_with_pass(_store_path(), FileAccess::READ, _passphrase());
	if (file.is_null()) {
		// Almost always means the file was written on another machine or by
		// another user account. Say so plainly instead of leaving the user
		// wondering why their saved key vanished.
		UtilityFunctions::push_warning(
				"AI Agent OS: could not decrypt " AIOS_CRED_FILE ". It was most likely created on a different "
				"machine or user account. Re-enter your API key in the AI Agent dock to replace it.");
		return;
	}

	Variant parsed = JSON::parse_string(file->get_as_text());
	file->close();
	if (parsed.get_type() == Variant::DICTIONARY) {
		stored = parsed;
	}
}

String AIOSCredentials::get_key(const String &p_provider) {
	const String env_name = env_var_for(p_provider);
	if (!env_name.is_empty()) {
		const String from_env = OS::get_singleton()->get_environment(env_name);
		if (!from_env.strip_edges().is_empty()) {
			return from_env.strip_edges();
		}
	}

	_ensure_loaded();
	if (stored.has(p_provider)) {
		return String(stored[p_provider]).strip_edges();
	}
	return String();
}

Error AIOSCredentials::set_key(const String &p_provider, const String &p_key) {
	_ensure_loaded();

	const String key = p_key.strip_edges();
	if (key.is_empty()) {
		stored.erase(p_provider);
	} else {
		stored[p_provider] = key;
	}

	if (!DirAccess::dir_exists_absolute(AIOS_CRED_DIR)) {
		DirAccess::make_dir_recursive_absolute(AIOS_CRED_DIR);
	}

	Ref<FileAccess> file = FileAccess::open_encrypted_with_pass(_store_path(), FileAccess::WRITE, _passphrase());
	if (file.is_null()) {
		UtilityFunctions::push_error("AI Agent OS: could not write " AIOS_CRED_FILE);
		return FAILED;
	}
	file->store_string(JSON::stringify(stored));
	file->close();

	emit_signal("credentials_changed", p_provider);
	return OK;
}

Error AIOSCredentials::clear_key(const String &p_provider) {
	return set_key(p_provider, String());
}

bool AIOSCredentials::has_key(const String &p_provider) {
	return !get_key(p_provider).is_empty();
}

bool AIOSCredentials::is_from_environment(const String &p_provider) const {
	const String env_name = env_var_for(p_provider);
	if (env_name.is_empty()) {
		return false;
	}
	return !OS::get_singleton()->get_environment(env_name).strip_edges().is_empty();
}

String AIOSCredentials::redact(const String &p_key) {
	const String key = p_key.strip_edges();
	if (key.is_empty()) {
		return String();
	}
	if (key.length() <= 12) {
		return String("****");
	}
	// Keep the provider prefix (it identifies the key type) and the last four
	// characters (enough to distinguish two keys), hide everything between.
	// Plain ASCII on purpose: a C string literal is decoded as Latin-1, so a
	// literal ellipsis here renders as mojibake in the dock and in describe().
	return key.substr(0, 7) + "..." + key.substr(key.length() - 4);
}

Dictionary AIOSCredentials::describe() {
	Dictionary out;

	for (int i = 0; i < AIOS_PROVIDER_COUNT; i++) {
		const String provider = AIOS_PROVIDERS[i].id;
		const String key = get_key(provider);

		Dictionary info;
		info["configured"] = !key.is_empty();
		info["kind"] = AIOS_PROVIDERS[i].kind;
		info["label"] = AIOS_PROVIDERS[i].label;
		info["source"] = key.is_empty() ? String("none") : (is_from_environment(provider) ? String("environment") : String("encrypted store"));
		info["env_var"] = env_var_for(provider);
		if (!key.is_empty()) {
			info["hint"] = redact(key);
		}
		out[provider] = info;
	}
	return out;
}
