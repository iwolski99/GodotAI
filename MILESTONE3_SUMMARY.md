# Milestone 3 Implementation Summary

Branch: `cursor/milestone-3-tasks-5b9e`

This document summarizes the Milestone 3 work completed in this session against the roadmap in `docs/ROADMAP.md`.

---

## What was implemented

### 1. Agent roles with distinct tool permissions

**Problem:** Dock modes (Architect / Coder / Debugger / Playtester) only changed the system prompt — every mode could call all 22 tools.

**Solution:**
- `AIOSToolRegistry::is_allowed_for_role()` enforces per-mode allowlists.
- `_apply_tools_for_phase()` filters the tool manifest sent to the LLM.
- `_execute_call()` refuses disallowed tools with a `role_forbidden` error.
- IPC respects role when `registry->set_active_role(mode)` is set during a pipeline run.

| Mode | Can mutate? | Notes |
|------|-------------|-------|
| **coder** | Yes | Full tool access |
| **architect** | No | Planning tools only (`propose_plan`, `get_world_model`, `validate_*`, etc.) |
| **debugger** | Limited | No delete/create scene; can patch scripts and run playtests |
| **playtester** | No | Read + `run_playtest` only |

**Files:** `src/tools/aios_tool_registry.{h,cpp}`, `src/pipeline/aios_pipeline.cpp`

---

### 2. Plan review before execution

**Problem:** The dock's "Execute Plan" button only broadcast an IPC event — the built-in agent never paused for human approval.

**Solution:**
- New `propose_plan` tool with JSON schema.
- Project setting `ai_agent_os/agent/require_plan_approval` (default `false`).
- When enabled, mutating tools return `plan_approval_required` until the human presses **Approve Plan**.
- Pipeline stage `AWAITING_APPROVAL`; dock button becomes **Approve Plan**.
- `AIOSPipeline::approve_plan()` resumes the run and tells the model to proceed.

**Files:** `src/pipeline/aios_pipeline.{h,cpp}`, `src/editor/aios_chat_dock.{h,cpp}`, `src/editor/aios_plugin.cpp`, `project/addons/godot_ai_os/schemas/propose_plan.json`

---

### 3. Diff preview in the dock

**Problem:** No way to see what would change before approving a plan.

**Solution:**
- `AIOSGitCheckpoint::diff_working_tree()` runs `git diff --stat HEAD` + unified diff (truncated).
- On `propose_plan`, the pipeline emits `plan_proposed` with the diff text.
- Dock renders it via `append_diff_preview()` in a monospace block.

**Files:** `src/vcs/aios_git_checkpoint.{h,cpp}`, `src/editor/aios_chat_dock.cpp`, `src/editor/aios_plugin.cpp`

---

### 4. MCP server

**Problem:** Cursor and other MCP clients had no plug-and-play path — users needed a custom adapter.

**Solution:**
- `clients/mcp/server.py` — stdio MCP server using the official `mcp` Python package.
- Lists tools from `session_ready` and forwards `call_tool` to `GodotAIClient.call()`.
- Documented in `docs/AGENTS.md` with a Cursor `mcp.json` example.

**Files:** `clients/mcp/server.py`, `clients/mcp/requirements.txt`, `docs/AGENTS.md`

---

### 5. Asset pipeline tools

**Problem:** Agents were stuck with ColorRects — no way to import textures or audio.

**Solution:**
- New `import_asset` tool: copies png/jpg/jpeg/webp/svg/wav/ogg/mp3 from disk into `res://`, triggers `EditorFileSystem::update_file()`.
- Supports `dry_run`.
- `get_world_model` filesystem section now includes `assets` alongside scenes and scripts.

**Files:** `src/tools/aios_scene_tools.{h,cpp}`, `src/tools/aios_tool_registry.cpp`, `src/world/aios_world_model.cpp`, `project/addons/godot_ai_os/schemas/import_asset.json`

---

### 6. Per-project agent memory

**Problem:** Every run started with a blank conversation — no recall of conventions or past failures.

**Solution:**
- `AIOSAgentMemory` stores data in `res://.godot/ai_agent_os/memory.json`.
- Tracks conventions, failure history, session notes, and run summaries.
- `format_for_prompt()` injects a memory block into the system prompt on `start()`.
- Failures recorded on repair-budget abort and auto-playtest errors.

**Files:** `src/agent/aios_agent_memory.{h,cpp}`, `src/pipeline/aios_pipeline.{h,cpp}`, `src/editor/aios_plugin.cpp`

---

### 7. Prebuilt binaries + CI integration tests

**Problem:** Users had to compile the GDExtension manually; no automated verification.

**Solution:**
- `.github/workflows/ci.yml` — build matrix for Linux, macOS, Windows; uploads binaries as artifacts.
- `scripts/integration_smoke_test.sh` — starts headless editor, waits for `session.json`, runs `ping` and `get_world_model` via the Python client.

**Note:** Artifacts are uploaded per-workflow-run. Wiring them to GitHub Releases is a one-line follow-up (`softprops/action-gh-release` or similar).

**Files:** `.github/workflows/ci.yml`, `scripts/integration_smoke_test.sh`

---

### 8. Reliability quick wins

#### `auto_playtest` wired

The `ai_agent_os/agent/auto_playtest` setting was stored but never read. After a successful mutating batch and scene validation, the pipeline now launches an automatic smoke playtest (20s timeout) before returning results to the model.

#### Driver lock (partial)

While the built-in pipeline is active, IPC mutating tool calls return `driver_busy` instead of racing with the in-editor agent.

**Files:** `src/pipeline/aios_pipeline.cpp`, `src/editor/aios_plugin.cpp`

---

## Tool count

The registry now exposes **25 tools** (was 22):

| New tool | Purpose |
|----------|---------|
| `propose_plan` | Submit a plan for human review |
| `import_asset` | Copy image/audio into res:// |

(Plus clarify tools `ask_user` / `commit_brief` from the prior branch.)

---

## Configuration added

| Setting | Default | Purpose |
|---------|---------|---------|
| `ai_agent_os/agent/require_plan_approval` | `false` | Block mutating tools until plan is approved |

Existing settings used more correctly:

| Setting | Now actually used for |
|---------|----------------------|
| `ai_agent_os/agent/auto_playtest` | Auto smoke test after mutating batches |

---

## What is still open (deferred)

These were identified in `docs/REVIEW-AGENT-LOOPS.md` but not addressed in this pass:

| Item | Why deferred |
|------|--------------|
| Dock routing policy (Built-in / External / Ask) | Needs UX design beyond driver lock |
| Global brief lock for IPC | Requires shared brief state in registry |
| Crash-as-clean playtest heuristic | Needs portable exit-code detection |
| GitHub Releases auto-upload | CI artifacts exist; release wiring is ops |
| Full asset pipeline (reimport metadata, assign to nodes) | `import_asset` is copy + scan; assignment still via `set_node_properties` |

---

## How to test locally

```bash
# Build (requires godot-cpp submodule)
scons platform=linux target=editor -j8

# Editor + bridge
godot --editor --path project/

# MCP (separate terminal, editor must be running)
pip install -r clients/mcp/requirements.txt
python3 clients/mcp/server.py ./project

# Plan review: enable in Project Settings > AI Agent OS > require_plan_approval
# Then use architect mode — agent must call propose_plan before building.

# Smoke test (headless)
bash scripts/integration_smoke_test.sh
```

---

## File change index

| Area | Files touched |
|------|---------------|
| Pipeline | `src/pipeline/aios_pipeline.{h,cpp}` |
| Tools | `src/tools/aios_tool_registry.{h,cpp}`, `src/tools/aios_scene_tools.{h,cpp}` |
| VCS | `src/vcs/aios_git_checkpoint.{h,cpp}` |
| Memory | `src/agent/aios_agent_memory.{h,cpp}` (new) |
| Dock / Plugin | `src/editor/aios_chat_dock.{h,cpp}`, `src/editor/aios_plugin.{h,cpp}` |
| World model | `src/world/aios_world_model.cpp` |
| Schemas | `schemas/propose_plan.json`, `schemas/import_asset.json` (new) |
| MCP | `clients/mcp/server.py`, `clients/mcp/requirements.txt` (new) |
| CI | `.github/workflows/ci.yml`, `scripts/integration_smoke_test.sh` (new) |
| Docs | `docs/ROADMAP.md`, `docs/AGENTS.md`, this file |
