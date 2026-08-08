/**************************************************************************/
/*  aios_chat_session.cpp                                                 */
/**************************************************************************/

#include "aios_chat_session.h"

#include "../editor/aios_chat_dock.h"
#include "../pipeline/aios_pipeline.h"
#include "aios_llm_client.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>

Dictionary AIOSChatSession::capture(const AIOSChatDock *p_dock, const AIOSLlmClient *p_llm,
		const AIOSPipeline *p_pipeline) const {
	Dictionary data;
	data["protocol"] = "godot-ai-os/chat-session/1";
	data["saved_at"] = Time::get_singleton()->get_unix_time_from_system();

	if (p_dock != nullptr) {
		data["dock_text"] = p_dock->export_history_text();
		data["message_count"] = p_dock->get_message_count();
		data["mode"] = p_dock->get_selected_mode();
	}

	if (p_llm != nullptr) {
		data["llm_history"] = p_llm->get_history();
		data["turn_count"] = p_llm->get_turn_count();
		data["system_prompt"] = p_llm->get_system_prompt();
	}

	if (p_pipeline != nullptr) {
		data["pipeline"] = p_pipeline->export_session_state();
	}

	return data;
}

bool AIOSChatSession::save(const Dictionary &p_data) const {
	const String dir = String(FILE_PATH).get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}

	Ref<FileAccess> file = FileAccess::open(FILE_PATH, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_string(JSON::stringify(p_data, "  "));
	file->close();
	return true;
}

Dictionary AIOSChatSession::load() const {
	if (!FileAccess::file_exists(FILE_PATH)) {
		return Dictionary();
	}

	Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(FILE_PATH));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	return parsed;
}

void AIOSChatSession::clear() const {
	if (FileAccess::file_exists(FILE_PATH)) {
		DirAccess::remove_absolute(FILE_PATH);
	}
}

bool AIOSChatSession::exists() const {
	return FileAccess::file_exists(FILE_PATH);
}

bool AIOSChatSession::apply(const Dictionary &p_data, AIOSChatDock *p_dock, AIOSLlmClient *p_llm,
		AIOSPipeline *p_pipeline) const {
	if (p_data.is_empty()) {
		return false;
	}

	if (p_dock != nullptr) {
		p_dock->restore_history_text(String(p_data.get("dock_text", "")), (int)(int64_t)p_data.get("message_count", 0));
		const String mode = String(p_data.get("mode", ""));
		if (!mode.is_empty()) {
			p_dock->set_selected_mode(mode);
		}
	}

	if (p_pipeline != nullptr && p_data.has("pipeline")) {
		p_pipeline->import_session_state(p_data["pipeline"]);
	}

	if (p_llm != nullptr && p_data.has("llm_history")) {
		const Array history = p_data.get("llm_history", Array());
		const int turn_count = (int)(int64_t)p_data.get("turn_count", 0);
		const String system_prompt = String(p_data.get("system_prompt", ""));
		p_llm->restore_conversation(history, turn_count, system_prompt);
	}

	if (p_pipeline != nullptr) {
		p_pipeline->sync_session_tools();
	}

	return true;
}
