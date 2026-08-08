#!/usr/bin/env python3
"""
MCP server for Godot AI Agent OS.

Exposes every tool from the editor bridge as an MCP tool. Requires a running
Godot editor with the plugin enabled.

Usage:
    python3 server.py /path/to/godot-project
    # Or set GODOT_AI_OS_PROJECT to the project path.

Cursor MCP config (`.cursor/mcp.json`):
    {
      "mcpServers": {
        "godot-ai-os": {
          "command": "python3",
          "args": ["/path/to/GodotAI/clients/mcp/server.py", "/path/to/your-project"]
        }
      }
    }
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from typing import Any

# Reference client lives next door — no package install required for the bridge.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))

from godot_ai_client import GodotAIClient, ToolError  # noqa: E402

try:
    from mcp.server import Server
    from mcp.server.stdio import stdio_server
    from mcp import types
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "Install MCP dependencies: pip install -r clients/mcp/requirements.txt"
    ) from exc


def _project_path() -> str:
    if len(sys.argv) > 1:
        return os.path.abspath(sys.argv[1])
    env = os.environ.get("GODOT_AI_OS_PROJECT", "")
    if env:
        return os.path.abspath(env)
    raise SystemExit("Usage: server.py /path/to/godot-project (or set GODOT_AI_OS_PROJECT)")


server = Server("godot-ai-os")
_godot: GodotAIClient | None = None


@server.list_tools()
async def list_tools() -> list[types.Tool]:
    assert _godot is not None
    out: list[types.Tool] = []
    for entry in _godot.tools.values():
        schema = dict(entry.get("input_schema") or {})
        schema.pop("name", None)
        schema.pop("title", None)
        schema.pop("mutating", None)
        out.append(
            types.Tool(
                name=entry["name"],
                description=entry.get("summary", entry["name"]),
                inputSchema=schema,
            )
        )
    return out


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any] | None) -> list[types.TextContent]:
    assert _godot is not None
    params = arguments or {}
    try:
        result = _godot.call(name, params)
        text = json.dumps(result, indent=2)
        return [types.TextContent(type="text", text=text)]
    except ToolError as exc:
        payload = {"code": exc.code, "message": str(exc), "details": exc.details}
        return [types.TextContent(type="text", text=json.dumps(payload, indent=2))]


async def _run() -> None:
    global _godot
    project = _project_path()
    with GodotAIClient.from_project(project) as godot:
        _godot = godot
        async with stdio_server() as (read_stream, write_stream):
            await server.run(read_stream, write_stream, server.create_initialization_options())


def main() -> None:
    asyncio.run(_run())


if __name__ == "__main__":
    main()
