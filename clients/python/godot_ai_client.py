#!/usr/bin/env python3
"""
Reference client for the Godot AI Agent OS bridge.

This is the smallest complete implementation of the protocol: it reads the
session file the plugin writes, connects, authenticates, and exposes a
request/response call plus an event stream. It deliberately has no third-party
dependencies — a WebSocket client is ~80 lines of stdlib socket code, and an
agent harness should not need a package install to talk to your editor.

Command line:

    python3 godot_ai_client.py /path/to/project ping
    python3 godot_ai_client.py /path/to/project get_world_model '{"max_depth": 2}'
    python3 godot_ai_client.py /path/to/project create_node_safe \\
        '{"type": "Sprite2D", "name": "Player", "properties": {"position": [64, 32]}}'
    python3 godot_ai_client.py /path/to/project --listen

Library:

    from godot_ai_client import GodotAIClient

    with GodotAIClient.from_project("/path/to/project") as godot:
        world = godot.call("get_world_model", {"max_depth": 3})
        godot.call("create_node_safe", {"type": "Timer", "name": "Spawner"})
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
from typing import Any, Callable, Dict, Iterator, Optional

SESSION_RELATIVE_PATH = os.path.join(".godot", "ai_agent_os", "session.json")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class BridgeError(RuntimeError):
    """Raised when the editor rejects a request or the link breaks."""


class ToolError(RuntimeError):
    """Raised when a tool returns {"ok": false}. Carries the structured error."""

    def __init__(self, tool: str, error: Dict[str, Any]):
        self.tool = tool
        self.code = error.get("code", "unknown")
        self.details = error.get("details")
        super().__init__(f"{tool} failed [{self.code}]: {error.get('message', '')}")


# --------------------------------------------------------------------------- #
#  Minimal RFC 6455 client framing                                             #
# --------------------------------------------------------------------------- #


class _WebSocket:
    """Text-frame-only WebSocket client. Enough for this protocol, no more."""

    def __init__(self, sock: socket.socket, host: str, port: int):
        self._sock = sock
        self._buffer = b""
        self._handshake(host, port)

    def _handshake(self, host: str, port: int) -> None:
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET / HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self._sock.sendall(request.encode())

        while b"\r\n\r\n" not in self._buffer:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise BridgeError("The editor closed the connection during the handshake.")
            self._buffer += chunk

        head, self._buffer = self._buffer.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if "101" not in status:
            raise BridgeError(f"WebSocket upgrade refused: {status}")

        expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        for line in head.decode(errors="replace").split("\r\n")[1:]:
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-accept" and value.strip() != expected:
                raise BridgeError("WebSocket handshake failed its accept-key check.")

    def send_text(self, text: str) -> None:
        payload = text.encode()
        header = bytearray([0x81])  # FIN + text opcode
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < (1 << 16):
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        # Clients must mask; the server unmasks.
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(bytes(header) + masked)

    def _fill(self, n: int) -> None:
        while len(self._buffer) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise BridgeError("The editor closed the connection.")
            self._buffer += chunk

    def recv_text(self, timeout: Optional[float] = None) -> Optional[str]:
        self._sock.settimeout(timeout)
        try:
            self._fill(2)
        except (socket.timeout, TimeoutError):
            return None
        finally:
            self._sock.settimeout(None)

        opcode = self._buffer[0] & 0x0F
        length = self._buffer[1] & 0x7F
        offset = 2
        if length == 126:
            self._fill(4)
            length = struct.unpack(">H", self._buffer[2:4])[0]
            offset = 4
        elif length == 127:
            self._fill(10)
            length = struct.unpack(">Q", self._buffer[2:10])[0]
            offset = 10

        self._fill(offset + length)
        payload = self._buffer[offset : offset + length]
        self._buffer = self._buffer[offset + length :]

        if opcode == 0x8:  # close
            raise BridgeError("The editor closed the connection.")
        if opcode == 0x9:  # ping -> pong
            self._sock.sendall(b"\x8a\x80" + os.urandom(4))
            return self.recv_text(timeout)
        if opcode not in (0x1, 0x0):
            return self.recv_text(timeout)
        return payload.decode(errors="replace")

    def close(self) -> None:
        try:
            self._sock.sendall(b"\x88\x80" + os.urandom(4))
        except OSError:
            pass
        self._sock.close()


class _JsonLines:
    """The no-WebSocket transport: newline-delimited JSON over raw TCP."""

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._buffer = b""

    def send_text(self, text: str) -> None:
        self._sock.sendall(text.encode() + b"\n")

    def recv_text(self, timeout: Optional[float] = None) -> Optional[str]:
        self._sock.settimeout(timeout)
        try:
            while b"\n" not in self._buffer:
                chunk = self._sock.recv(65536)
                if not chunk:
                    raise BridgeError("The editor closed the connection.")
                self._buffer += chunk
        except (socket.timeout, TimeoutError):
            return None
        finally:
            self._sock.settimeout(None)

        line, self._buffer = self._buffer.split(b"\n", 1)
        return line.decode(errors="replace")

    def close(self) -> None:
        self._sock.close()


# --------------------------------------------------------------------------- #
#  Client                                                                      #
# --------------------------------------------------------------------------- #


class GodotAIClient:
    def __init__(self, host: str, port: int, token: str = "", transport: str = "websocket"):
        sock = socket.create_connection((host, port), timeout=10)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._link = _WebSocket(sock, host, port) if transport == "websocket" else _JsonLines(sock)
        self._next_id = 1
        self.tools: Dict[str, Dict[str, Any]] = {}

        self._link.send_text(json.dumps({"type": "hello", "token": token}))
        welcome = self._await(lambda m: m.get("type") in ("welcome", "error"), timeout=10)
        if welcome.get("type") == "error":
            raise BridgeError(welcome.get("message", "handshake refused"))
        self.client_id = welcome.get("client_id")

        # The editor pushes the tool manifest right after the handshake. It may
        # already have arrived while we were waiting for the welcome, in which
        # case _stash_event has it — so only block if we have not seen it yet.
        if not self.tools:
            self._await(
                lambda m: m.get("type") == "event" and m.get("event") == "session_ready",
                timeout=5,
                required=False,
            )

    # -- construction -------------------------------------------------------

    @classmethod
    def from_project(cls, project_path: str) -> "GodotAIClient":
        """Reads <project>/.godot/ai_agent_os/session.json, written by the plugin."""
        session_path = os.path.join(project_path, SESSION_RELATIVE_PATH)
        if not os.path.exists(session_path):
            raise BridgeError(
                f"No session file at {session_path}. Open the project in the Godot editor "
                "with the AI Agent OS extension installed."
            )
        with open(session_path, "r", encoding="utf-8") as handle:
            session = json.load(handle)
        return cls(
            session.get("host", "127.0.0.1"),
            int(session["port"]),
            session.get("token", ""),
            session.get("transport", "websocket"),
        )

    def __enter__(self) -> "GodotAIClient":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    def close(self) -> None:
        self._link.close()

    # -- protocol -----------------------------------------------------------

    def _await(
        self,
        predicate: Callable[[Dict[str, Any]], bool],
        timeout: float,
        required: bool = True,
    ) -> Dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = self._link.recv_text(timeout=deadline - time.monotonic())
            if raw is None:
                break
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                continue
            self._stash_event(message)
            if predicate(message):
                return message
            self.on_event(message)
        if required:
            raise BridgeError("Timed out waiting for the editor to respond.")
        return {}

    def _stash_event(self, message: Dict[str, Any]) -> None:
        """Picks up state the editor pushes at us, whenever it happens to arrive."""
        if message.get("type") == "event" and message.get("event") == "session_ready":
            self.tools = {t["name"]: t for t in message.get("data", {}).get("tools", [])}

    def on_event(self, message: Dict[str, Any]) -> None:
        """Override to handle unsolicited events (logs, world_changed, prompts)."""

    def call(self, tool: str, params: Optional[Dict[str, Any]] = None, timeout: float = 30.0) -> Dict[str, Any]:
        """Invokes a tool and returns its result. Raises ToolError on failure."""
        request_id = str(self._next_id)
        self._next_id += 1
        self._link.send_text(
            json.dumps({"id": request_id, "type": "request", "tool": tool, "params": params or {}})
        )
        response = self._await(
            lambda m: m.get("type") == "response" and m.get("id") == request_id, timeout=timeout
        )
        if not response.get("ok"):
            raise ToolError(tool, response.get("error", {}))
        return response.get("result", {})

    def events(self) -> Iterator[Dict[str, Any]]:
        """Blocking iterator over every event the editor sends."""
        while True:
            raw = self._link.recv_text(timeout=None)
            if raw is None:
                continue
            try:
                message = json.loads(raw)
            except json.JSONDecodeError:
                continue
            self._stash_event(message)
            if message.get("type") == "event":
                yield message

    # -- convenience --------------------------------------------------------

    def say(self, text: str) -> None:
        """Writes a chat message into the editor dock."""
        self._link.send_text(json.dumps({"type": "event", "event": "chat", "data": {"text": text}}))

    def set_state(self, state: str, detail: str = "") -> None:
        """Drives the dock's pipeline indicator: PLANNING, EXECUTING, ..."""
        self._link.send_text(
            json.dumps({"type": "event", "event": "status", "data": {"state": state, "detail": detail}})
        )

    def log(self, text: str, level: str = "info") -> None:
        self._link.send_text(
            json.dumps({"type": "event", "event": "log", "data": {"level": level, "text": text}})
        )

    def wait_playtest(self, timeout: float = 120.0) -> Dict[str, Any]:
        """Block until the editor emits `playtest_finished`, then return its report.

        Completes the Observe half of the loop for harnesses that call
        `run_playtest` (which only acknowledges launch). Raises BridgeError on
        timeout.
        """
        message = self._await(
            lambda m: m.get("type") == "event" and m.get("event") == "playtest_finished",
            timeout=timeout,
        )
        data = message.get("data") or {}
        # Prefer the structured report when the plugin nests it; otherwise the
        # event payload *is* the report.
        report = data.get("report") if isinstance(data.get("report"), dict) else data
        return report


# --------------------------------------------------------------------------- #
#  CLI                                                                         #
# --------------------------------------------------------------------------- #


def _main(argv: list) -> int:
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    project = argv[1]
    try:
        client = GodotAIClient.from_project(project)
    except BridgeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    with client:
        if len(argv) < 3 or argv[2] == "--listen":
            print(f"connected as client #{client.client_id}; {len(client.tools)} tools available")
            print("listening for events, Ctrl-C to stop\n")
            try:
                for event in client.events():
                    print(json.dumps(event))
            except KeyboardInterrupt:
                return 0
            return 0

        tool = argv[2]
        params = json.loads(argv[3]) if len(argv) > 3 else {}
        try:
            print(json.dumps(client.call(tool, params), indent=2))
        except ToolError as exc:
            print(f"error: {exc}", file=sys.stderr)
            if exc.details:
                print(json.dumps(exc.details, indent=2), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
