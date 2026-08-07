extends Node
## Forwards a running game's console output back to the AI Agent dock.
##
## Why this exists: when the editor launches a playtest, the game is a separate
## OS process. Its stdout/stderr go to the editor's Output panel, and there is no
## GDExtension API to read that panel. So instead of scraping the editor, the
## game reports its own output — it is the only process that can.
##
## The trick is that Godot already writes everything (print(), push_error(),
## engine errors, script stack traces) to `user://logs/godot.log` when file
## logging is enabled. We tail that file and forward new lines over the same IPC
## link the editor's agent bridge listens on.
##
## Setup (one line in Project Settings > Autoload):
##     res://addons/godot_ai_os/runtime/agent_log_bridge.gd   as   AgentLogBridge
##
## It is a no-op in exported builds and whenever no session file is present, so
## it is safe to leave enabled.

const SESSION_FILE := "res://.godot/ai_agent_os/session.json"
const POLL_INTERVAL := 0.25
## Guard against a runaway error loop flooding the dock.
const MAX_LINES_PER_POLL := 40

var _socket: WebSocketPeer = null
var _tcp: StreamPeerTCP = null
var _token := ""
var _connected := false
var _log_path := ""
var _read_offset := 0
var _timer := 0.0
var _rx_buffer := ""


func _ready() -> void:
	if OS.has_feature("template"):
		# Exported build: there is no editor to talk to.
		queue_free()
		return

	var session := _read_session()
	if session.is_empty():
		queue_free()
		return

	_log_path = ProjectSettings.get_setting("debug/file_logging/log_path", "user://logs/godot.log")
	if not ProjectSettings.get_setting("debug/file_logging/enable_file_logging", false):
		push_warning("AgentLogBridge: enable debug/file_logging/enable_file_logging to forward console output.")

	# Start at the end of the existing log so we only forward this run's output.
	_read_offset = _file_size(_log_path)
	_token = str(session.get("token", ""))
	_connect_to_bridge(session)
	set_process(true)


func _read_session() -> Dictionary:
	if not FileAccess.file_exists(SESSION_FILE):
		return {}
	var text := FileAccess.get_file_as_string(SESSION_FILE)
	var parsed: Variant = JSON.parse_string(text)
	return parsed if parsed is Dictionary else {}


func _connect_to_bridge(session: Dictionary) -> void:
	var host := str(session.get("host", "127.0.0.1"))
	var port := int(session.get("port", 45857))

	if str(session.get("transport", "websocket")) == "websocket":
		_socket = WebSocketPeer.new()
		var err := _socket.connect_to_url("ws://%s:%d" % [host, port])
		if err != OK:
			push_warning("AgentLogBridge: could not reach the agent bridge (%s)." % error_string(err))
			_socket = null
	else:
		_tcp = StreamPeerTCP.new()
		if _tcp.connect_to_host(host, port) != OK:
			_tcp = null


func _process(delta: float) -> void:
	_pump_socket()
	if not _connected:
		return

	_timer += delta
	if _timer < POLL_INTERVAL:
		return
	_timer = 0.0
	_forward_new_log_lines()


func _pump_socket() -> void:
	if _socket != null:
		_socket.poll()
		var state := _socket.get_ready_state()
		if state == WebSocketPeer.STATE_OPEN and not _connected:
			_connected = true
			_send({"type": "hello", "token": _token})
			_send_event("runtime_log", {"stream": "stdout", "text": "playtest started"})
		elif state == WebSocketPeer.STATE_CLOSED:
			_connected = false
		return

	if _tcp != null:
		_tcp.poll()
		if _tcp.get_status() == StreamPeerTCP.STATUS_CONNECTED and not _connected:
			_connected = true
			_send({"type": "hello", "token": _token})
			_send_event("runtime_log", {"stream": "stdout", "text": "playtest started"})
		elif _tcp.get_status() == StreamPeerTCP.STATUS_ERROR:
			_connected = false


func _file_size(path: String) -> int:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return 0
	var size := f.get_length()
	f.close()
	return size


func _forward_new_log_lines() -> void:
	var f := FileAccess.open(_log_path, FileAccess.READ)
	if f == null:
		return

	var size := f.get_length()
	if size <= _read_offset:
		# Log rotated or truncated: start over from the beginning.
		if size < _read_offset:
			_read_offset = 0
		else:
			f.close()
			return

	f.seek(_read_offset)
	_rx_buffer += f.get_buffer(size - _read_offset).get_string_from_utf8()
	_read_offset = size
	f.close()

	var lines := _rx_buffer.split("\n")
	# The final element is an incomplete line; keep it for the next poll.
	_rx_buffer = lines[lines.size() - 1]

	var sent := 0
	for i in range(lines.size() - 1):
		var line: String = lines[i].strip_edges()
		if line.is_empty():
			continue
		if sent >= MAX_LINES_PER_POLL:
			_send_event("runtime_log", {"stream": "stderr", "text": "… output throttled, see the Output panel"})
			break
		_send_event("runtime_log", {"stream": _classify(line), "text": line})
		sent += 1


func _classify(line: String) -> String:
	for marker in ["ERROR:", "SCRIPT ERROR:", "USER ERROR:", "WARNING:", "USER WARNING:"]:
		if line.begins_with(marker) or line.contains(" " + marker):
			return "stderr"
	return "stdout"


func _send_event(event: String, data: Dictionary) -> void:
	_send({"type": "event", "event": event, "data": data})


func _send(message: Dictionary) -> void:
	var text := JSON.stringify(message)
	if _socket != null and _socket.get_ready_state() == WebSocketPeer.STATE_OPEN:
		_socket.send_text(text)
	elif _tcp != null and _tcp.get_status() == StreamPeerTCP.STATUS_CONNECTED:
		_tcp.put_data((text + "\n").to_utf8_buffer())


func _exit_tree() -> void:
	if _connected:
		_send_event("runtime_log", {"stream": "stdout", "text": "playtest ended"})
	if _socket != null:
		_socket.close()
	if _tcp != null:
		_tcp.disconnect_from_host()
