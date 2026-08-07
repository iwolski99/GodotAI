/**************************************************************************/
/*  aios_ipc_server.cpp                                                   */
/**************************************************************************/

#include "aios_ipc_server.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

AIOSIpcServer::AIOSIpcServer() {}

AIOSIpcServer::~AIOSIpcServer() {
	stop();
}

void AIOSIpcServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "port", "bind_address", "mode", "token"), &AIOSIpcServer::start);
	ClassDB::bind_method(D_METHOD("stop"), &AIOSIpcServer::stop);
	ClassDB::bind_method(D_METHOD("poll"), &AIOSIpcServer::poll);
	ClassDB::bind_method(D_METHOD("is_running"), &AIOSIpcServer::is_running);
	ClassDB::bind_method(D_METHOD("get_port"), &AIOSIpcServer::get_port);
	ClassDB::bind_method(D_METHOD("get_mode"), &AIOSIpcServer::get_mode);
	ClassDB::bind_method(D_METHOD("get_bind_address"), &AIOSIpcServer::get_bind_address);
	ClassDB::bind_method(D_METHOD("get_client_count"), &AIOSIpcServer::get_client_count);
	ClassDB::bind_method(D_METHOD("get_clients"), &AIOSIpcServer::get_clients);
	ClassDB::bind_method(D_METHOD("send_to", "client_id", "message"), &AIOSIpcServer::send_to);
	ClassDB::bind_method(D_METHOD("broadcast", "message"), &AIOSIpcServer::broadcast);
	ClassDB::bind_method(D_METHOD("broadcast_event", "event", "data"), &AIOSIpcServer::broadcast_event);

	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "client_id"), PropertyInfo(Variant::STRING, "remote")));
	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "client_id"), PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::INT, "client_id"), PropertyInfo(Variant::DICTIONARY, "message")));
	ADD_SIGNAL(MethodInfo("transport_log", PropertyInfo(Variant::STRING, "level"), PropertyInfo(Variant::STRING, "message")));

	BIND_CONSTANT(MODE_WEBSOCKET);
	BIND_CONSTANT(MODE_TCP_JSONL);
}

Error AIOSIpcServer::start(int p_port, const String &p_bind_address, int p_mode, const String &p_token) {
	stop();

	mode = (p_mode == MODE_TCP_JSONL) ? MODE_TCP_JSONL : MODE_WEBSOCKET;
	bind_address = p_bind_address.is_empty() ? String("127.0.0.1") : p_bind_address;
	token = p_token;

	server.instantiate();
	Error err = server->listen(p_port, bind_address);
	if (err != OK) {
		server.unref();
		emit_signal("transport_log", "error",
				vformat("Could not listen on %s:%d (error %d). Is another editor instance already running the AI Agent OS?",
						bind_address, p_port, (int)err));
		return err;
	}

	port = (int)server->get_local_port();
	running = true;

	if (bind_address != "127.0.0.1" && bind_address != "localhost" && bind_address != "::1") {
		emit_signal("transport_log", "warn",
				vformat("Listening on %s - the agent bridge is reachable from other machines. Use 127.0.0.1 unless you know why you need this.", bind_address));
	}
	emit_signal("transport_log", "info",
			vformat("Agent bridge listening on %s:%d (%s)", bind_address, port,
					mode == MODE_WEBSOCKET ? "websocket" : "tcp-jsonl"));
	return OK;
}

void AIOSIpcServer::stop() {
	for (size_t i = 0; i < clients.size(); i++) {
		Client &c = clients[i];
		if (c.ws.is_valid()) {
			c.ws->close(1001, "editor shutting down");
		}
		if (c.tcp.is_valid()) {
			c.tcp->disconnect_from_host();
		}
	}
	clients.clear();

	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	if (running) {
		emit_signal("transport_log", "info", "Agent bridge stopped.");
	}
	running = false;
	port = 0;
}

void AIOSIpcServer::poll() {
	if (!running || server.is_null()) {
		return;
	}

	_accept_pending();

	// Service clients by index and compact afterwards: _handle_payload can
	// re-enter this object through signal handlers, so we never hold iterators.
	std::vector<size_t> dropped;
	for (size_t i = 0; i < clients.size(); i++) {
		bool drop = false;
		_service_client(clients[i], drop);
		if (drop) {
			dropped.push_back(i);
		}
	}
	for (size_t i = dropped.size(); i > 0; i--) {
		clients.erase(clients.begin() + dropped[i - 1]);
	}
}

void AIOSIpcServer::_accept_pending() {
	while (server->is_connection_available()) {
		Ref<StreamPeerTCP> conn = server->take_connection();
		if (conn.is_null()) {
			break;
		}
		conn->set_no_delay(true);

		Client c;
		c.id = next_client_id++;
		c.remote = conn->get_connected_host() + ":" + String::num_int64(conn->get_connected_port());
		c.authenticated = token.is_empty();

		if (mode == MODE_WEBSOCKET) {
			Ref<WebSocketPeer> ws;
			ws.instantiate();
			ws->set_inbound_buffer_size((int)max_message_bytes);
			ws->set_outbound_buffer_size((int)max_message_bytes);
			Ref<StreamPeer> stream = conn;
			if (ws->accept_stream(stream) != OK) {
				emit_signal("transport_log", "warn", "Rejected an inbound connection: WebSocket handshake could not start.");
				continue;
			}
			c.ws = ws;
		} else {
			c.tcp = conn;
		}

		clients.push_back(c);
		// With no token configured there is nothing to wait for, so a JSONL peer
		// is live the moment it connects. Otherwise the announcement waits for a
		// valid handshake — an unauthenticated peer must not receive the tool
		// manifest the plugin sends on connect.
		_announce_if_ready(clients.back());
	}
}

void AIOSIpcServer::_service_client(Client &p_client, bool &r_drop) {
	if (p_client.ws.is_valid()) {
		p_client.ws->poll();
		const WebSocketPeer::State state = p_client.ws->get_ready_state();

		if (state == WebSocketPeer::STATE_OPEN) {
			_announce_if_ready(p_client);
			while (p_client.ws->get_available_packet_count() > 0) {
				PackedByteArray packet = p_client.ws->get_packet();
				if (packet.size() > max_message_bytes) {
					_drop_client(p_client, "message too large");
					r_drop = true;
					return;
				}
				_handle_payload(p_client, packet.get_string_from_utf8());
			}
		} else if (state == WebSocketPeer::STATE_CLOSED) {
			if (p_client.announced) {
				emit_signal("client_disconnected", p_client.id, p_client.ws->get_close_reason());
			}
			r_drop = true;
		}
		return;
	}

	if (p_client.tcp.is_valid()) {
		p_client.tcp->poll();
		if (p_client.tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			emit_signal("client_disconnected", p_client.id, "socket closed");
			r_drop = true;
			return;
		}

		const int64_t available = p_client.tcp->get_available_bytes();
		if (available > 0) {
			Array chunk = p_client.tcp->get_partial_data((int)available);
			if ((Error)(int)chunk[0] == OK) {
				PackedByteArray bytes = chunk[1];
				p_client.rx.append_array(bytes);
			}
		}

		if (p_client.rx.size() > max_message_bytes) {
			_drop_client(p_client, "message too large");
			r_drop = true;
			return;
		}

		// Split the buffer on newlines; the tail stays buffered for next poll.
		while (true) {
			int newline = -1;
			for (int i = 0; i < p_client.rx.size(); i++) {
				if (p_client.rx[i] == '\n') {
					newline = i;
					break;
				}
			}
			if (newline < 0) {
				break;
			}
			PackedByteArray line = p_client.rx.slice(0, newline);
			p_client.rx = p_client.rx.slice(newline + 1);
			String text = line.get_string_from_utf8().strip_edges();
			if (!text.is_empty()) {
				_handle_payload(p_client, text);
			}
		}
	}
}

void AIOSIpcServer::_handle_payload(Client &p_client, const String &p_text) {
	Variant parsed = JSON::parse_string(p_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		Dictionary err;
		err["type"] = "error";
		err["error"] = "malformed_json";
		err["message"] = "Every frame must be a single JSON object.";
		_send_raw(p_client, JSON::stringify(err));
		return;
	}

	Dictionary message = parsed;

	if (!p_client.authenticated) {
		// Until the token is presented, the only message we will look at is the
		// handshake. Everything else is refused without hinting at internals.
		const String type = message.has("type") ? String(message["type"]) : String();
		const String presented = message.has("token") ? String(message["token"]) : String();
		if (type != "hello" || presented != token) {
			Dictionary err;
			err["type"] = "error";
			err["error"] = "unauthorized";
			err["message"] = "Send {\"type\":\"hello\",\"token\":\"<session token>\"} first. The token is shown in the AI Agent dock.";
			_send_raw(p_client, JSON::stringify(err));
			_drop_client(p_client, "failed authentication");
			return;
		}
		p_client.authenticated = true;

		Dictionary welcome;
		welcome["type"] = "welcome";
		welcome["protocol"] = "godot-ai-os/1";
		welcome["client_id"] = p_client.id;
		_send_raw(p_client, JSON::stringify(welcome));

		// Only now is this peer real. Announcing here — after the welcome is on
		// the wire — guarantees a client sees welcome before session_ready.
		_announce_if_ready(p_client);
		return;
	}

	if (message.has("type") && String(message["type"]) == "hello") {
		Dictionary welcome;
		welcome["type"] = "welcome";
		welcome["protocol"] = "godot-ai-os/1";
		welcome["client_id"] = p_client.id;
		_send_raw(p_client, JSON::stringify(welcome));
		return;
	}

	emit_signal("message_received", p_client.id, message);
}

void AIOSIpcServer::_announce_if_ready(Client &p_client) {
	if (p_client.announced || !p_client.authenticated) {
		return;
	}
	if (p_client.ws.is_valid() && p_client.ws->get_ready_state() != WebSocketPeer::STATE_OPEN) {
		return;
	}
	p_client.announced = true;
	emit_signal("client_connected", p_client.id, p_client.remote);
}

void AIOSIpcServer::_send_raw(Client &p_client, const String &p_text) {
	if (p_client.ws.is_valid()) {
		if (p_client.ws->get_ready_state() == WebSocketPeer::STATE_OPEN) {
			p_client.ws->send_text(p_text);
		}
		return;
	}
	if (p_client.tcp.is_valid() && p_client.tcp->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
		p_client.tcp->put_data((p_text + String("\n")).to_utf8_buffer());
	}
}

void AIOSIpcServer::_drop_client(Client &p_client, const String &p_reason) {
	if (p_client.ws.is_valid()) {
		p_client.ws->close(1008, p_reason);
	}
	if (p_client.tcp.is_valid()) {
		p_client.tcp->disconnect_from_host();
	}
	if (p_client.announced) {
		emit_signal("client_disconnected", p_client.id, p_reason);
	}
	emit_signal("transport_log", "warn", vformat("Dropped client %d: %s", p_client.id, p_reason));
}

AIOSIpcServer::Client *AIOSIpcServer::_find_client(int p_id) {
	for (size_t i = 0; i < clients.size(); i++) {
		if (clients[i].id == p_id) {
			return &clients[i];
		}
	}
	return nullptr;
}

Array AIOSIpcServer::get_clients() const {
	Array out;
	for (size_t i = 0; i < clients.size(); i++) {
		Dictionary d;
		d["id"] = clients[i].id;
		d["remote"] = clients[i].remote;
		d["authenticated"] = clients[i].authenticated;
		out.push_back(d);
	}
	return out;
}

void AIOSIpcServer::send_to(int p_client_id, const Dictionary &p_message) {
	Client *c = _find_client(p_client_id);
	if (c == nullptr) {
		return;
	}
	_send_raw(*c, JSON::stringify(p_message));
}

void AIOSIpcServer::broadcast(const Dictionary &p_message) {
	if (clients.empty()) {
		return;
	}
	// Encode once; the payload is identical for every peer.
	const String text = JSON::stringify(p_message);
	for (size_t i = 0; i < clients.size(); i++) {
		if (clients[i].authenticated) {
			_send_raw(clients[i], text);
		}
	}
}

void AIOSIpcServer::broadcast_event(const String &p_event, const Dictionary &p_data) {
	Dictionary msg;
	msg["type"] = "event";
	msg["event"] = p_event;
	msg["data"] = p_data;
	broadcast(msg);
}
