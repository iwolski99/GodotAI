# MCP server for Godot AI Agent OS

Exposes every bridge tool to MCP-capable clients (Cursor, Claude Desktop, etc.)
with no adapter code. The server is a thin stdio wrapper around
[`clients/python/godot_ai_client.py`](../python/godot_ai_client.py): it reads the
tool manifest from `session_ready` and forwards `tools/call` to the live editor.

**Requirements**

- Godot 4.4+ with this plugin loaded and the project open
- Python 3.10+ (stdlib only — no `pip install`)

## Cursor

Add to your MCP config (Cursor Settings → MCP, or `.cursor/mcp.json`):

```json
{
  "mcpServers": {
    "godot-ai-os": {
      "command": "python3",
      "args": [
        "/absolute/path/to/GodotAI/clients/mcp/server.py",
        "--project",
        "/absolute/path/to/your-godot-project"
      ]
    }
  }
}
```

Or with an environment variable:

```json
{
  "mcpServers": {
    "godot-ai-os": {
      "command": "python3",
      "args": ["/absolute/path/to/GodotAI/clients/mcp/server.py"],
      "env": {
        "GODOT_AI_PROJECT": "/absolute/path/to/your-godot-project"
      }
    }
  }
}
```

Open the project in Godot first. The plugin writes
`<project>/.godot/ai_agent_os/session.json`; without that file the server exits
with a clear error.

## Smoke test

With the editor running:

```bash
# JSON-RPC initialize + tools/list over stdin (newline-delimited test mode)
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | python3 clients/mcp/server.py --jsonl --project /path/to/project
```

Cursor and other MCP hosts use Content-Length framing by default (no `--jsonl`).

## Notes

- `run_playtest` waits for the `playtest_finished` event (via
  `GodotAIClient.wait_playtest`) so MCP callers get the full report, not only
  the launch ack.
- The server reconnects once if the editor drops the socket (token rotates on
  every editor restart — re-read `session.json`).
- This path exposes **tools only**. The built-in Clarify → Plan → Validate →
  Repair loop stays in the editor dock unless you enable server-side pipeline
  mode later. Prefer `dry_run`, `validate_change`, and `run_playtest` in your
  harness.
- If a built-in agent run holds the **driver lock**, mutating tools return
  `busy` until that run finishes or is stopped.
