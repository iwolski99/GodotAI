# Roadmap

Three milestones are planned. This is a living document — scope will move as the
thing gets used.

---

## Milestone 1 — Core plugin, transport, and the dock ✅ *shipped*

The foundation: a way in, a way to look around, a way to change things safely,
and a way to undo.

- [x] GDExtension + native `EditorPlugin` skeleton, editor-only registration
- [x] Local IPC transport — WebSocket and TCP-JSONL over one loopback listener,
      token handshake, session discovery file
- [x] In-memory world model with revision-tracked caching
- [x] `get_world_model`, `create_node_safe`, `attach_script_safe`,
      `safe_delete_node`
- [x] Auxiliary tools: `save_scene`, `open_scene`, `create_checkpoint`,
      `rollback_last`, `list_tools`, `ping`
- [x] JSON Schema for every tool, pushed to agents on connect
- [x] Git checkpoint after every mutation; revert-based rollback
- [x] Native chat dock — history, prompt input, mode selector, pipeline state
      indicator, Execute / Stop / Rollback
- [x] Playtest output forwarded into the dock via the runtime log bridge
- [x] Dependency-free Python reference client

---

## Milestone 2 — The autonomous loop *(next)*

Milestone 1 lets an agent make changes. Milestone 2 makes it responsible for
whether they work.

**Playtest harness**
- Launch a scene under editor control, with a time limit and a clean shutdown
- Capture runtime errors, script stack traces and warnings as structured events
  rather than log lines to be regexed
- Report crash / freeze / clean-exit as a first-class result an agent can branch on

**Validation gate**
- A pre-flight pass in front of every mutating tool: does the scene still make
  sense after this change?
- Orphaned `NodePath`s, missing resources, scripts referencing nodes that no
  longer exist, signal connections to methods that were deleted
- Runs on `dry_run` too, so a plan can be validated before any of it executes

**Repair loop**
- Feed a failed playtest back to the agent with the diff of what changed since
  the last known-good checkpoint
- Automatic rollback on repeated failure instead of letting an agent dig deeper
- The `PLANNING → VALIDATING → EXECUTING → PLAYTESTING → REPAIRING` state machine
  driven by the plugin rather than narrated by the agent

**More tools**
- `connect_signal_safe` / `disconnect_signal_safe`
- `set_node_properties` (edit, not just create)
- `move_node` / `reparent_node` with reference fixups
- `create_scene`, `instance_scene_safe`
- `read_script` / `patch_script` with structural edits rather than whole-file writes
- `run_project` / `stop_project`

---

## Milestone 3 — Multi-agent orchestration and polish

Making it something other people can rely on.

- Agent roles with distinct tool permissions (an Architect that cannot write
  files; a Debugger that cannot delete)
- Plan review: the agent proposes a sequence, the human approves it in the dock,
  and only then does it execute
- Diff preview in the dock before a batch of mutations lands
- MCP server so any MCP-capable client connects with no adapter code
- Asset pipeline tools — import, texture and audio handling
- Per-project agent memory: what was tried, what broke, what the conventions are
- Prebuilt binaries on GitHub Releases for Linux, macOS and Windows
- CI: build matrix, headless integration tests against a real editor

---

## Not planned

Things deliberately out of scope, so nobody builds them expecting a merge:

- **A bundled LLM client.** The plugin is a transport and a toolbox. Model
  choice, prompting and cost belong to whatever harness you already use.
- **API keys in the editor.** No credential should live in a `.gdextension`
  addon folder.
- **Remote / cloud access.** This binds loopback on purpose. Exposing an editor
  that can write arbitrary files to a network is not a feature.
- **C# / .NET tool equivalents.** GDScript first. C# support would need the tool
  layer to understand a second script backend, and that is a lot of surface for
  a small audience — reconsider if people ask.

---

## Have an opinion?

Open an issue. Concrete arguments about ordering, or about what is missing from
Milestone 2, are more useful than agreement.
