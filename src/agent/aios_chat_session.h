/**************************************************************************/
/*  aios_chat_session.h                                                   */
/*  Persists dock chat + LLM history across editor restarts.              */
/**************************************************************************/

#pragma once

#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

class AIOSChatDock;
class AIOSLlmClient;
class AIOSPipeline;

// Project-local session file under res://.godot/ai_agent_os/. Survives editor
// restarts so architect interviews and plans are not lost before any scene
// edits land on disk.
class AIOSChatSession {
public:
	static constexpr const char *FILE_PATH = "res://.godot/ai_agent_os/chat_session.json";

	Dictionary capture(const AIOSChatDock *p_dock, const AIOSLlmClient *p_llm, const AIOSPipeline *p_pipeline) const;
	bool save(const Dictionary &p_data) const;
	Dictionary load() const;
	void clear() const;
	bool exists() const;

	bool apply(const Dictionary &p_data, AIOSChatDock *p_dock, AIOSLlmClient *p_llm, AIOSPipeline *p_pipeline) const;
};
