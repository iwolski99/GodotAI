/**************************************************************************/
/*  aios_credentials.h                                                    */
/*  API key storage for the built-in LLM client.                          */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

// Where an API key must never live: anywhere under res://. A key in the project
// folder gets committed, pushed, and scraped — this plugin is for public repos,
// so that failure is a matter of when, not if.
//
// So keys live in user://, which the editor maps outside the project entirely
// (%APPDATA%\Godot\app_userdata\<project> on Windows, ~/.local/share/godot/...
// on Linux, ~/Library/Application Support/Godot/... on macOS). The file is
// written through FileAccess::open_encrypted_with_pass so a key is not sitting
// in plaintext in a directory backup software sweeps up.
//
// Be clear about what that encryption is and is not. The passphrase is derived
// from the machine, so the file is useless if copied to another computer — but
// anything running as you on this computer can derive the same passphrase and
// read it. It is encryption at rest against casual disclosure, not a keychain.
// The genuinely safe option is an environment variable, which is checked first
// and never written anywhere.
class AIOSCredentials : public RefCounted {
	GDCLASS(AIOSCredentials, RefCounted)

private:
	Dictionary stored; // provider id -> key, decrypted in memory.
	bool loaded = false;

	static String _store_path();
	static String _passphrase();
	void _ensure_loaded();

protected:
	static void _bind_methods();

public:
	// Environment variable consulted for each provider, checked before the
	// stored file. Returns "" for an unknown provider.
	static String env_var_for(const String &p_provider);

	// Every provider this store knows about. p_kind filters to "model" (the LLM
	// providers) or "asset" (the generation APIs); empty returns all of them.
	static PackedStringArray known_providers(const String &p_kind = String());

	// The key to use for a provider: environment variable first, then the
	// encrypted store. Empty when neither has one.
	String get_key(const String &p_provider);

	// Writes a key to the encrypted store. An empty value clears it.
	Error set_key(const String &p_provider, const String &p_key);
	Error clear_key(const String &p_provider);

	bool has_key(const String &p_provider);

	// True when the active key comes from the environment — the UI shows this
	// so a user editing the stored key understands why it has no effect.
	bool is_from_environment(const String &p_provider) const;

	// Never returns key material: just which providers are configured and how.
	// Safe to log, safe to send over the IPC link.
	Dictionary describe();

	// "sk-ant-…4f2a" — enough to tell two keys apart, useless if leaked.
	static String redact(const String &p_key);
};
