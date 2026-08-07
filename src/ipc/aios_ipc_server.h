/**************************************************************************/
/*  aios_ipc_server.h                                                     */
/*  Local transport layer between the Godot editor and external agents.   */
/**************************************************************************/

#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/classes/web_socket_peer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <vector>

using namespace godot;

// A single-threaded, loopback-only message bus.
//
// Design notes
// ------------
// * Everything is polled from the editor's main thread (see AIOSPlugin::_process).
//   Godot's editor API is *not* thread safe, and every message we receive ends
//   up mutating scene state, so a background reader thread would only buy us a
//   marshalling queue and a class of very hard bugs. At the message rates an
//   agent produces (a handful per second) polling is free.
// * Two wire formats are supported over the same TCP listener:
//     - MODE_WEBSOCKET  (default) RFC 6455 text frames, one JSON object each.
//     - MODE_TCP_JSONL  newline-delimited JSON, for clients that would rather
//                       not pull in a WebSocket library (see clients/python).
// * The listener binds 127.0.0.1 by default and requires a per-session token.
//   That token is a speed bump against other local processes and stray browser
//   tabs, not a security boundary: anything that can read the project directory
//   can read the token. Never bind this to 0.0.0.0 on a shared machine.
class AIOSIpcServer : public RefCounted {
	GDCLASS(AIOSIpcServer, RefCounted)

public:
	enum Mode {
		MODE_WEBSOCKET = 0,
		MODE_TCP_JSONL = 1,
	};

private:
	struct Client {
		int id = 0;
		Ref<WebSocketPeer> ws;
		Ref<StreamPeerTCP> tcp;
		PackedByteArray rx;
		bool announced = false;
		bool authenticated = false;
		String remote;
	};

	Ref<TCPServer> server;
	std::vector<Client> clients;

	int next_client_id = 1;
	int mode = MODE_WEBSOCKET;
	int port = 0;
	String bind_address = "127.0.0.1";
	String token;
	bool running = false;

	// Guards against a malformed or hostile peer buffering us out of memory.
	int64_t max_message_bytes = 8 * 1024 * 1024;

	void _accept_pending();
	void _service_client(Client &p_client, bool &r_drop);
	void _handle_payload(Client &p_client, const String &p_text);
	void _announce_if_ready(Client &p_client);
	void _send_raw(Client &p_client, const String &p_text);
	void _drop_client(Client &p_client, const String &p_reason);
	Client *_find_client(int p_id);

protected:
	static void _bind_methods();

public:
	AIOSIpcServer();
	~AIOSIpcServer();

	Error start(int p_port, const String &p_bind_address, int p_mode, const String &p_token);
	void stop();
	void poll();

	bool is_running() const { return running; }
	int get_port() const { return port; }
	int get_mode() const { return mode; }
	String get_bind_address() const { return bind_address; }
	int get_client_count() const { return (int)clients.size(); }
	Array get_clients() const;

	// Both take a plain Dictionary; JSON encoding happens here so callers never
	// have to think about the wire format.
	void send_to(int p_client_id, const Dictionary &p_message);
	void broadcast(const Dictionary &p_message);

	// Convenience wrapper used by the tool layer and the dock for one-way
	// notifications: {"type": "event", "event": <name>, "data": {...}}.
	void broadcast_event(const String &p_event, const Dictionary &p_data);
};
