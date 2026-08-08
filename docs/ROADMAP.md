# Roadmap

All three planned milestones have shipped. This is a living document — scope
moved as the thing got used, and the record of where it moved is at the bottom.

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

## Milestone 3 — Editing, sight, and generated assets ✅ *shipped*

Milestone 1 let an agent read and add. Milestone 2 made it accountable for
whether its changes worked. Milestone 3 lets it **edit** what already exists,
**see** what it built, and **acquire** the assets it needs.

**Editing tools** — the actual blocker on "build me a game"
- [x] `set_node_properties` — move, rotate, scale, retint, set exported vars
- [x] `create_scene` — reusable prefabs exist at all
- [x] `reparent_node` — restructure, or reorder for draw/layout order
- [x] `connect_signal_safe` / `disconnect_signal_safe` — gameplay wiring
- [x] `read_script` with a function index
- [x] `patch_script` — replace one function instead of rewriting the file
- [x] Spatial awareness in the world model: world-space position, rotation in
      degrees, scale, and transformed AABB bounds

**Sight**
- [x] `capture_viewport_screenshot` — the editor viewport as a PNG
- [x] Image content blocks in both providers, including the workaround for
      OpenAI's refusal to carry images inside a tool result

**Generated assets**
- [x] Meshy and Tripo3D text-to-3D, downloaded and imported
- [x] `import_asset_from_url` — works with any service, including ones this
      plugin has never heard of
- [x] Asset keys in the encrypted credential store, never in `project.godot`
- [x] Per-session paid-call ceiling, and a dock warning on every paid call

**Blender**
- [x] Headless bridge with cross-platform discovery
- [x] `cleanup_mesh` — decimate to a triangle budget, normalise scale, weld
      seams, drop the origin to the footprint
- [x] Collision via Godot's own import-time mesh-name suffixes
- [x] `run_blender_script` for procedural geometry
- [ ] **Not verified end to end** — Blender is not installed in the development
      environment. The script is syntax- and argument-tested only.

**UI**
- [x] Settings moved out of the dock into a dialog

---

## How the three milestones interlock

Each milestone is useless without the one before it, which is worth making
explicit because the dependency is not the obvious one.

```
  M1  TOOLS            read the project, change it, undo the change
       │               get_world_model / create_node_safe / attach_script_safe
       │               safe_delete_node + git checkpoints + the IPC bridge
       │
       │  ── without M1, an agent is guessing at your scene tree ──
       ▼
  M2  ACCOUNTABILITY   was the change correct?
       │               Validate before executing. Run the game. Read the stack
       │               trace. Repair, or reset --hard to the snapshot taken
       │               before the step.
       │
       │  ── without M2, M1's tools let an agent break things faster ──
       ▼
  M3  REACH            edit what exists, see what you built, get what you need
                       set_node_properties / connect_signal_safe / patch_script
                       capture_viewport_screenshot
                       generate_3d_asset -> cleanup_mesh
```

The loop a real session runs through touches all three:

| Step | Milestone | Tool |
| --- | --- | --- |
| "Build a corridor with a locked door" | — | the prompt |
| Read the scene | M1 | `get_world_model` (now with transforms, M3) |
| Snapshot before touching anything | M2 | `AIOSGitCheckpoint::create_snapshot` |
| Check the plan before running it | M2 | `validate_change` |
| Make the prefab | M3 | `create_scene` |
| Build it | M1 | `create_node_safe` |
| Place it in the world | M3 | `set_node_properties` |
| Get a door model | M3 | `generate_3d_asset` → `cleanup_mesh` |
| Wire the lock | M3 | `connect_signal_safe` |
| Fix one function of the script | M3 | `patch_script` |
| Check it still holds together | M2 | `validate_scene` |
| Run it | M2 | `run_playtest` |
| Look at it | M3 | `capture_viewport_screenshot` |
| Fix what broke, or roll back | M2 | the repair budget |
| Commit the working state | M1 | `create_checkpoint` |

Read bottom-up, the design rule is: **M3 tools are only safe because M2 gates
them, and M2 can only gate them because M1 made the project legible.**

---

## Milestone 4 — the ideas that did not fit

Not scheduled. Listed because they came up while building M3 and are the
obvious next things.

**Making long sessions work**
- Plan review: the agent proposes a sequence, the human approves it in the
  dock, and only then does it execute
- Diff preview before a batch of mutations lands
- Per-project agent memory: what was tried, what broke, the conventions
- Agent roles with distinct tool permissions (an Architect that cannot write
  files; a Debugger that cannot delete)

**Assets**
- Textures and audio through the same gateway (ElevenLabs and OpenAI keys are
  already in the credential store; the download path is already generic)
- A material pass — generated meshes arrive with flat albedo and no roughness
- LOD generation, which is the same Blender decimate at three budgets
- Animation retargeting

**Reach**
- MCP server so any MCP-capable client connects with no adapter code
- Prebuilt binaries on GitHub Releases for Linux, macOS and Windows
- CI: build matrix, headless integration tests against a real editor

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
