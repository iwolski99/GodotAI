#!/usr/bin/env python3
"""
MCP server for Godot AI Agent OS.

Thin stdio JSON-RPC wrapper over the existing bridge: tool schemas come from
`session_ready`, and every `tools/call` is forwarded to `GodotAIClient.call`.

Requires a live Godot editor with the AI Agent OS plugin loaded. No third-party
packages — only the stdlib plus the reference client in `clients/python/`.

Cursor / Claude Desktop example:

    {
      "mcpServers": {
        "godot-ai-os": {
          "command": "python3",
          "args": [
            "/absolute/path/to/GodotAI/clients/mcp/server.py",
            "--project", "/absolute/path/to/your-godot-project"
          ]
        }
      }
    }

CLI:

    python3 clients/mcp/server.py --project /path/to/project
    GODOT_AI_PROJECT=/path/to/project python3 clients/mcp/server.py
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import traceback
from typing import Any, Dict, List, Optional

# Allow importing the dependency-free reference client next door.
_CLIENTS_PYTHON = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "python"))
if _CLIENTS_PYTHON not in sys.path:
    sys.path.insert(0, _CLIENTS_PYTHON)

from godot_ai_client import BridgeError, GodotAIClient, ToolError  # noqa: E402

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "godot-ai-os"
SERVER_VERSION = "0.3.0"


class McpServer:
    """Minimal MCP server over newline-delimited JSON-RPC 2.0 on stdio."""

    def __init__(self, project: str, reconnect: bool = True):
        self.project = os.path.abspath(project)
        self.reconnect = reconnect
        self.client: Optional[GodotAIClient] = None
        self._initialized = False

    # -- bridge -------------------------------------------------------------

    def _connect(self) -> GodotAIClient:
        if self.client is not None:
            return self.client
        self.client = GodotAIClient.from_project(self.project)
        return self.client

    def _reset_client(self) -> None:
        if self.client is not None:
            try:
                self.client.close()
            except Exception:
                pass
        self.client = None

    def _with_client(self, fn):
        try:
            return fn(self._connect())
        except BridgeError:
            if not self.reconnect:
                raise
            self._reset_client()
            return fn(self._connect())

    # -- MCP handlers -------------------------------------------------------

    def handle(self, message: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        msg_id = message.get("id")
        method = message.get("method")
        params = message.get("params") or {}

        # Notifications have no id and must not get a response.
        is_notification = "id" not in message

        try:
            if method == "initialize":
                result = self._initialize(params)
            elif method == "notifications/initialized":
                self._initialized = True
                return None
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = self._tools_list()
            elif method == "tools/call":
                result = self._tools_call(params)
            elif method == "resources/list":
                result = {"resources": []}
            elif method == "prompts/list":
                result = {"prompts": []}
            elif method == "logging/setLevel":
                result = {}
            else:
                if is_notification:
                    return None
                return self._error(msg_id, -32601, f"Method not found: {method}")
        except BridgeError as exc:
            if is_notification:
                return None
            return self._error(msg_id, -32000, f"Godot bridge error: {exc}")
        except ToolError as exc:
            # Tool failures are successful MCP calls with isError=true content.
            if is_notification:
                return None
            payload = {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps(
                            {"ok": False, "code": exc.code, "message": str(exc), "details": exc.details},
                            indent=2,
                        ),
                    }
                ],
                "isError": True,
            }
            return {"jsonrpc": "2.0", "id": msg_id, "result": payload}
        except Exception as exc:
            if is_notification:
                return None
            return self._error(msg_id, -32603, f"Internal error: {exc}", data=traceback.format_exc())

        if is_notification:
            return None
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}

    def _initialize(self, params: Dict[str, Any]) -> Dict[str, Any]:
        # Connect early so Cursor surfaces a clear failure if Godot is offline.
        client = self._connect()
        return {
            "protocolVersion": params.get("protocolVersion") or PROTOCOL_VERSION,
            "capabilities": {
                "tools": {"listChanged": False},
            },
            "serverInfo": {
                "name": SERVER_NAME,
                "version": SERVER_VERSION,
            },
            "instructions": (
                "Godot AI Agent OS tools for a live editor session. "
                f"Connected to project {self.project!r} as client #{client.client_id} "
                f"with {len(client.tools)} tools. Call get_world_model before editing; "
                "call save_scene after a coherent batch of scene edits; use run_playtest "
                "to observe runtime errors. Prefer dry_run when uncertain."
            ),
        }

    def _tools_list(self) -> Dict[str, Any]:
        def list_tools(client: GodotAIClient) -> List[Dict[str, Any]]:
            tools = []
            for name, meta in client.tools.items():
                schema = meta.get("input_schema") or {"type": "object", "properties": {}}
                # MCP wants a JSON Schema object; some manifests nest under "properties".
                if "type" not in schema:
                    schema = {"type": "object", **schema}
                tools.append(
                    {
                        "name": name,
                        "description": meta.get("summary") or meta.get("description") or name,
                        "inputSchema": schema,
                    }
                )
            tools.sort(key=lambda t: t["name"])
            return tools

        return {"tools": self._with_client(list_tools)}

    def _tools_call(self, params: Dict[str, Any]) -> Dict[str, Any]:
        name = params.get("name")
        if not name:
            raise BridgeError("tools/call requires 'name'")
        arguments = params.get("arguments") or {}

        def invoke(client: GodotAIClient) -> Dict[str, Any]:
            # Playtests are async over the bridge; give them a longer budget and
            # wait for playtest_finished when the ack succeeds.
            if name == "run_playtest":
                timeout = float(arguments.get("timeout_sec", 60)) + 30.0
                ack = client.call(name, arguments, timeout=min(timeout, 15.0))
                report = client.wait_playtest(timeout=timeout)
                return {"ack": ack, "report": report}
            return client.call(name, arguments, timeout=60.0)

        result = self._with_client(invoke)
        return {
            "content": [{"type": "text", "text": json.dumps(result, indent=2, default=str)}],
            "structuredContent": result if isinstance(result, dict) else {"result": result},
            "isError": False,
        }

    @staticmethod
    def _error(msg_id: Any, code: int, message: str, data: Any = None) -> Dict[str, Any]:
        err: Dict[str, Any] = {"code": code, "message": message}
        if data is not None:
            err["data"] = data
        return {"jsonrpc": "2.0", "id": msg_id, "error": err}

    # -- stdio loop ---------------------------------------------------------

    def serve_stdio(self) -> int:
        # Line-buffered stdout so MCP hosts see each JSON-RPC message promptly.
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(line_buffering=True)
        if hasattr(sys.stdin, "reconfigure"):
            sys.stdin.reconfigure(line_buffering=True)

        for raw in sys.stdin:
            line = raw.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                sys.stdout.write(
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": None,
                            "error": {"code": -32700, "message": "Parse error"},
                        }
                    )
                    + "\n"
                )
                sys.stdout.flush()
                continue

            # MCP also allows Content-Length framed messages; support both.
            response = self.handle(message)
            if response is not None:
                sys.stdout.write(json.dumps(response, default=str) + "\n")
                sys.stdout.flush()

        self._reset_client()
        return 0


def _read_content_length_frames(server: McpServer) -> int:
    """Optional Content-Length framing (used by some MCP hosts)."""
    stdin = sys.stdin.buffer
    while True:
        headers = b""
        while b"\r\n\r\n" not in headers:
            chunk = stdin.readline()
            if not chunk:
                server._reset_client()
                return 0
            headers += chunk
            if headers in (b"\n", b"\r\n"):
                # Blank line between JSONL messages — ignore.
                headers = b""
                continue

        header_text = headers.decode("utf-8", errors="replace")
        length = None
        for line in header_text.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        if length is None:
            # Not framed — try parsing headers as a JSON line (JSONL host).
            try:
                message = json.loads(header_text.strip())
            except json.JSONDecodeError:
                continue
            response = server.handle(message)
            if response is not None:
                body = json.dumps(response, default=str).encode("utf-8")
                sys.stdout.buffer.write(
                    f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
                )
                sys.stdout.buffer.flush()
            continue

        body = stdin.read(length)
        if not body:
            server._reset_client()
            return 0
        try:
            message = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            err = json.dumps(
                {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
            ).encode("utf-8")
            sys.stdout.buffer.write(f"Content-Length: {len(err)}\r\n\r\n".encode("ascii") + err)
            sys.stdout.buffer.flush()
            continue

        response = server.handle(message)
        if response is not None:
            out = json.dumps(response, default=str).encode("utf-8")
            sys.stdout.buffer.write(f"Content-Length: {len(out)}\r\n\r\n".encode("ascii") + out)
            sys.stdout.buffer.flush()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="MCP server for Godot AI Agent OS")
    parser.add_argument(
        "--project",
        default=os.environ.get("GODOT_AI_PROJECT", ""),
        help="Path to the Godot project (or set GODOT_AI_PROJECT)",
    )
    parser.add_argument(
        "--jsonl",
        action="store_true",
        help="Use newline-delimited JSON instead of MCP Content-Length framing (for manual tests)",
    )
    parser.add_argument(
        "--no-reconnect",
        action="store_true",
        help="Do not reconnect after a bridge drop",
    )
    args = parser.parse_args(argv)

    if not args.project:
        print(
            "error: pass --project /path/to/godot-project or set GODOT_AI_PROJECT",
            file=sys.stderr,
        )
        return 2

    server = McpServer(args.project, reconnect=not args.no_reconnect)
    try:
        # MCP stdio hosts (Cursor, Claude Desktop) speak Content-Length framing.
        # --jsonl is for the printf smoke test in the README.
        if args.jsonl:
            return server.serve_stdio()
        return _read_content_length_frames(server)
    except KeyboardInterrupt:
        server._reset_client()
        return 0
    except BridgeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
