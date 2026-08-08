# Milestone 3 progress — orchestration, MCP, and reliability

Summary of the first Milestone 3 implementation slice on branch
`cursor/milestone-3-orchestration-bfb0`.

## Shipped in this change

### 1. MCP server (`clients/mcp/`)

- Stdio MCP server with **Content-Length** framing (Cursor / Claude Desktop).
- No third-party deps — wraps `clients/python/godot_ai_client.py`.
- `tools/list` from `session_ready`; `tools/call` → bridge.
- `run_playtest` waits for `playtest_finished` via the new client helper.
- Docs: [`clients/mcp/README.md`](../clients/mcp/README.md),
  [`docs/AGENTS.md`](AGENTS.md#model-context-protocol).

### 2. Wire `auto_playtest`

- After a successful scene sweep, if the batch called `save_scene` and
  `agent/auto_playtest` is on, the pipeline launches a 60-frame headless smoke
  test and feeds the report back to the model.
- If the model already ended its turn (no further tool calls) with an unsaved
  playtest pending, the same smoke test runs before `run_finished`.

### 3. Driver lock + dock routing

| Setting | Default | Behaviour |
| --- | --- | --- |
| `agent/driver_lock` | on | Built-in pipeline holds an exclusive driver while active; IPC mutations / playtests get `busy`. IPC playtests hold the lock only until `playtest_finished`. |
| `agent/dock_routing` | Auto | **Auto** = built-in when a key exists, else IPC. **Built-in** / **External** force one path. |

Fixes the dual-control-plane conflict called out in
[`REVIEW-AGENT-LOOPS.md`](REVIEW-AGENT-LOOPS.md).

### 4. Agent role permissions

Built-in mode selector now **filters tools** (and refuses forbidden calls):

| Role | Withheld |
| --- | --- |
| Architect | `attach_script_safe`, `patch_script`, `safe_delete_node`, `rollback_last` |
| Debugger | `safe_delete_node`, `rollback_last` |
| Playtester | everything except read / validate / playtest / memory / ask |
| Coder | (full set) |

### 5. Per-project agent memory

- New tools: `remember`, `recall_memory`
- Store: `res://.godot/ai_agent_os/memory.json`
- Built-in system prompt injects recent notes at run start

### 6. Playtest crash heuristic

Silent / immediate process death without `quit_after_frames` is reported as
`crashed` instead of a false `clean` (Godot still has no portable exit-code API).

### 7. Python `wait_playtest`

`GodotAIClient.wait_playtest(timeout=…)` blocks on the `playtest_finished` event
so external harnesses can complete Observe without reimplementing the event loop.

## Still open (Milestone 3)

- Human plan review before execution
- Diff preview in the dock
- Asset pipeline tools (import / texture / audio)
- Prebuilt binaries + CI integration tests
- Server-side pipeline mode over IPC (Validate → Observe → Repair as events)
- Global brief lock for IPC (clarify gates still pipeline-local)

## Files touched (high level)

| Area | Paths |
| --- | --- |
| MCP | `clients/mcp/server.py`, `clients/mcp/README.md` |
| Python client | `clients/python/godot_ai_client.py` |
| Pipeline | `src/pipeline/aios_pipeline.{h,cpp}` |
| Plugin / settings | `src/editor/aios_plugin.{h,cpp}` |
| Tools / memory | `src/tools/aios_tool_registry.{h,cpp}`, `schemas/remember.json`, `schemas/recall_memory.json` |
| Playtest | `src/playtest/aios_playtest.{h,cpp}` |
| Docs | `ROADMAP.md`, `AGENTS.md`, `SETUP.md`, `PIPELINE.md`, `REVIEW-AGENT-LOOPS.md`, `CONTRIBUTING.md` |

## How to try it

1. Rebuild the GDExtension: `scons target=editor -j$(nproc)`
2. Open the project in Godot 4.4+
3. **MCP:** add the server to Cursor MCP config (see `clients/mcp/README.md`)
4. **External dock prompts:** Project Settings → AI Agent OS → `dock_routing` = External
5. **Memory:** ask the built-in agent to `remember` a convention; start a new run and confirm it appears in the prompt / via `recall_memory`
