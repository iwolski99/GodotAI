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

## Milestone 2 — The autonomous loop ✅ *shipped*

Milestone 1 let an agent make changes. Milestone 2 makes it responsible for
whether they work. Full write-up: **[docs/PIPELINE.md](PIPELINE.md)**.

**The pipeline**
- [x] `Plan → Validate → Execute → Observe → Repair → Snapshot → Continue` as a
      strict linear state machine, driven by the plugin rather than narrated by
      the agent
- [x] Every C++ hook, validation routine and runtime interceptor serves exactly
      one named stage
- [x] Repair budget — after N consecutive failures the pipeline rolls back and
      hands control to the human instead of letting the model dig
- [x] Prompt-wrapper builders that turn machine findings into text a model can
      act on: validation feedback, playtest feedback, rollback notice

**Transactional git engine**
- [x] Snapshot before every batch of changes; `git reset --hard` back to it when
      the batch breaks the project
- [x] `git merge-base --is-ancestor` guard so a stray sha can never discard
      unrelated history
- [x] Cross-platform git discovery, including Windows install locations and a
      `GODOT_AI_OS_GIT` override
- [x] The human's Rollback button stays on the safe `git revert` path

**Playtest harness and error interception**
- [x] `run_playtest` launches the game in a child process with a time limit and
      an optional deterministic frame-count exit
- [x] Non-blocking stdout/stderr capture via Godot's `--log-file`, tailed from
      `_process` — no autoload required in the game
- [x] Engine errors, script errors and GDScript stack traces parsed into
      structured diagnostics with file, line and function
- [x] `clean` / `errors` / `crashed` / `timeout` as a first-class result an agent
      can branch on

**Pre-execution validation**
- [x] GDScript compiled in memory — never written to disk — for real parser
      diagnostics
- [x] `extends` checked against the target node's actual class
- [x] NodePath references resolved against the open scene
- [x] Property bags type-checked against ClassDB before anything is instantiated
- [x] Whole-scene sweep after execution: dangling NodePaths, missing resources,
      broken scripts, nodes with no owner
- [x] `validate_change` exposes the same checks as a dry-run tool

**Built-in agent** *(added at a user's request, and a reversal — see below)*
- [x] Anthropic and OpenRouter support behind one canonical conversation format
- [x] Model selector, populated live from the provider's model list
- [x] Thinking on/off, reasoning effort `low` → `max`, output token budget
- [x] API keys stored encrypted in `user://`, with environment variables taking
      priority and never being written anywhere

**Windows**
- [x] `.gitattributes` so a Windows checkout does not rewrite every file to CRLF
      and make the first checkpoint look like a whole-project rewrite
- [x] No console windows flashing up from git invocations
- [x] Documented MSVC build path
- [ ] **Not yet verified on a Windows machine.** The code paths are written and
      reviewed; the development environment is Linux. Reports welcome.

**Deferred to Milestone 3**
- `connect_signal_safe` / `disconnect_signal_safe`
- `set_node_properties` (edit, not just create)
- `move_node` / `reparent_node` with reference fixups
- `create_scene`, `instance_scene_safe`
- `read_script` / `patch_script` with structural edits rather than whole-file
  writes

---

## Milestone 3 — Multi-agent orchestration and polish

Making it something other people can rely on.

- [x] **Clarify-before-build interview** — architect/coder modes ask focused
  questions (including 2D vs 3D) via `ask_user` until `commit_brief` locks a
  design brief; mutating tools stay withheld until then. Dock shows
  `CLARIFYING`, routes answers into the open interview, and offers
  **Skip & Build**.
- [x] **Agent roles with distinct tool permissions** — Architect cannot write
  scripts or delete; Debugger cannot delete or roll back; Playtester is
  observe-only. Enforced in the built-in tool list and at execute time.
- [x] **MCP server** — `clients/mcp/server.py` exposes the live tool manifest to
  Cursor / Claude Desktop over stdio (Content-Length framing). See
  [clients/mcp/README.md](../clients/mcp/README.md).
- [x] **Driver lock + dock routing** — `agent/driver_lock` (default on) stops IPC
  mutations while the built-in pipeline runs; `agent/dock_routing`
  (`Auto` / `Built-in` / `External`) controls whether dock prompts go to the
  built-in agent or an external harness.
- [x] **Wire `auto_playtest`** — after a mutating batch that called `save_scene`,
  the pipeline launches a short `quit_after_frames` smoke test when the setting
  is on.
- [x] **Per-project agent memory** — `remember` / `recall_memory` tools; notes
  live in `.godot/ai_agent_os/memory.json` and are injected into the built-in
  system prompt on the next run.
- [x] **Playtest crash heuristic** — silent immediate process death without
  `quit_after_frames` is reported as `crashed`, not `clean`.
- [x] **Python `wait_playtest`** — reference client helper for the Observe half
  of external harnesses.
- Plan review: the agent proposes a sequence, the human approves it in the dock,
  and only then does it execute
- Diff preview in the dock before a batch of mutations lands
- Asset pipeline tools — import, texture and audio handling
- Prebuilt binaries on GitHub Releases for Linux, macOS and Windows
- CI: build matrix, headless integration tests against a real editor
- Server-side pipeline mode over IPC (Validate → Observe → Repair as events)

---

## Changed our minds

Milestone 1's roadmap listed two things as "not planned" that Milestone 2 then
shipped. Leaving that unmarked would be quietly rewriting history, so:

- **~~A bundled LLM client.~~** Originally argued the plugin should be a transport
  and a toolbox, with model choice belonging to whatever harness you already use.
  That reasoning holds for people who *have* a harness. It is a wall for everyone
  else, and it made the pipeline impossible to demonstrate without writing a
  second program first. The bridge is unchanged and still first-class — the
  built-in agent is an addition, not a replacement.

- **~~API keys in the editor.~~** The original objection was specifically to a
  credential sitting in an addon folder under `res://`, and that objection was
  right and still stands: nothing is ever written there. Keys live in `user://`,
  encrypted, outside the project — or, better, in an environment variable that is
  read and never stored. The rule was too broad, not wrong.

---

## Not planned

Things deliberately out of scope, so nobody builds them expecting a merge:

- **Remote / cloud access.** This binds loopback on purpose. Exposing an editor
  that can write arbitrary files to a network is not a feature.
- **Sending your project to a model wholesale.** Tools answer specific questions
  about specific parts of the project. There is no "upload the repo" call and
  there will not be one.
- **C# / .NET tool equivalents.** GDScript first. C# support would need the tool
  layer to understand a second script backend, and that is a lot of surface for
  a small audience — reconsider if people ask.

---

## Have an opinion?

Open an issue. Concrete arguments about ordering, or about what is missing from
Milestone 3, are more useful than agreement.
