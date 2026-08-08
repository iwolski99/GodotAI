# Architecture

How the pieces fit, and why they are shaped the way they are.

---

## The shape of the problem

An agent wants to do four things to a Godot project: **read** its structure,
**change** it, **run** it, and **undo** the change when running it goes badly.
Milestone 1 built the first, second and fourth, plus the transport they travel
over. Milestone 2 added the third — and then the loop that makes the four add up
to something more useful than four separate buttons.

The hard part is not any individual operation — Godot's editor API can create a
node in three lines. The hard part is that an agent operates without the context
a human editor has. It cannot see that a node is referenced by a script three
folders away. It does not know that a `.tscn` on disk is two minutes behind what
is on screen. It will confidently pass a string where a `Vector2` belongs. Every
design decision below follows from assuming a caller with no situational
awareness and infinite patience for being told "no".

---

## Layers

```
 ┌──────────────────────────────────┐   ┌───────────────────────────────────┐
 │  External agent                  │   │  Built-in agent                   │
 │  Claude Code, Cursor, a script   │   │  AIOSPipeline + AIOSLlmClient     │
 └────────────────┬─────────────────┘   └────────────────┬──────────────────┘
                  │ JSON over WebSocket / TCP-JSONL      │ in-process
                  │ loopback, token handshake            │ signals
 ┌────────────────▼─────────────────┐                    │
 │  AIOSIpcServer      src/ipc/     │                    │
 │  Frames, authenticates, emits on │                    │
 │  the editor's main thread.       │                    │
 └────────────────┬─────────────────┘                    │
                  │                                      │
 ┌────────────────▼──────────────────────────────────────▼──────────────────┐
 │  AIOSPlugin                                            src/editor/       │
 │  EditorPlugin. Owns everything, routes messages, polls the transport and │
 │  the pipeline from _process, wires the dock to the tools.                │
 └───┬──────────────────────┬──────────────────────────┬───────────────┬────┘
     │                      │                          │               │
 ┌───▼────────────┐  ┌──────▼───────────┐  ┌───────────▼────┐  ┌───────▼────┐
 │ AIOSChatDock   │  │ AIOSToolRegistry │  │ AIOSValidator  │  │ AIOSGit-   │
 │ src/editor/    │  │ src/tools/       │  │ src/validate/  │  │ Checkpoint │
 │ Renders state, │  │ Dispatch +       │  │ Static checks  │  │ src/vcs/   │
 │ emits intent.  │  │ policy: check-   │  │ before a call  │  │ Snapshot,  │
 │ Settings panel │  │ point, cache     │  │ ever runs.     │  │ reset,     │
 │ for the model. │  │ invalidation.    │  │ Nothing is     │  │ revert.    │
 │                │  │                  │  │ written.       │  │            │
 └────────────────┘  └──────┬───────────┘  └────────────────┘  └────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
 ┌──────▼──────────┐ ┌──────▼──────────┐ ┌──────▼──────────────┐
 │ AIOSWorldModel  │ │ AIOSSceneTools  │ │ AIOSPlaytest        │
 │ src/world/      │ │ src/tools/      │ │ src/playtest/       │
 │ The read path.  │ │ The write path. │ │ The run path.       │
 │ Cached JSON of  │ │ create/attach/  │ │ Child process +     │
 │ the live tree.  │ │ delete/save.    │ │ --log-file tailing. │
 └─────────────────┘ └─────────────────┘ └─────────────────────┘
```

Both agents reach the project through the same `AIOSToolRegistry`, and both get
the same manifest from `list_tools()`. That is deliberate: the moment the
built-in agent has a private path to the tools, the two drift apart and one of
them starts silently rotting.

---

## Decisions worth explaining

### Everything runs on the main thread

The transport is polled from `AIOSPlugin::_process`. There is no reader thread,
no work queue, no lock.

Godot's editor API is not thread-safe, and *every* message an agent sends ends up
mutating the scene tree or reading it. A background thread would have to marshal
every single operation back to the main thread anyway — buying nothing but a
class of race conditions that only appear under load, in someone else's project,
once. At the rate an LLM produces tool calls (single digits per second, each
followed by a network round trip to a model), polling a socket sixty times a
second is not a cost worth optimising.

The one thing this costs: a tool that blocked for seconds would freeze the
editor. None of them do — the slowest, `safe_delete_node` with script scanning,
is ~20 ms on a project with a few hundred scripts.

### The world model reads the live tree, not `.tscn` files

The obvious implementation of `get_world_model` is to parse the scene file. It is
also wrong: the file on disk is whatever was last saved, and the thing an agent
is editing is the editor's in-memory tree. An agent that adds a node, then reads
the file, sees nothing and adds it again.

So the model walks `EditorInterface::get_edited_scene_root()` and caches the
resulting JSON. Invalidation is coarse on purpose — any structural editor event
bumps a revision counter and marks the cache dirty. Rebuilding a few hundred
nodes takes well under a millisecond, so "rebuild when in doubt" beats
fine-grained delta tracking that is subtly wrong in the one case nobody tested.

The `revision` counter is exposed to agents so they can tell whether their view
went stale — including when *another* agent or a human made the change.

### Properties are validated before anything is instantiated

`create_node_safe` builds the full set of coerced property values, and only if
every one succeeds does it call `instantiate`. The alternative — create, then set
properties, then fail halfway — leaves an orphan node in the scene and a
confused agent.

The coercion layer ([`src/util/aios_json.cpp`](../src/util/aios_json.cpp)) exists
because JSON has six types and Godot has thirty-eight. An agent writing
`"position": [64, 32]` is being reasonable and should not have to know about
`Vector2`. So `[64, 32]`, `{"x": 64, "y": 32}` and `{"__type": "Vector2", ...}`
all work, `"#ff8800"` becomes a `Color`, and a `res://` path becomes a loaded
`Resource`. When coercion genuinely cannot work, the error names the expected
type — and a misspelled property name comes back with `did_you_mean`.

### `set_owner` is not optional

Every node `create_node_safe` adds gets `set_owner(scene_root)`. Without it the
node exists in the running tree but is never written to the `.tscn` — it silently
vanishes on save. This is the single most common way agent-built scenes evaporate,
and it is one line.

### Deletion is an audit, not a delete

`safe_delete_node` produces two lists:

- **Blockers** — deleting the scene root; deleting into an instanced sub-scene;
  other nodes holding `NodePath` properties that point into the subtree. These
  stop the call unless `force: true`.
- **Warnings** — descendants that go too, orphaned scripts, authored signal
  connections, group membership, and textual `$Name` / `%Name` references found
  in project scripts. Reported, not blocking.

The signal audit only reports connections flagged `CONNECT_PERSIST` — the ones
Godot writes into the `.tscn` because a human made them in the Node dock. Without
that filter the report drowns in engine plumbing (`SceneTreeEditor::…`,
`Viewport::…`) that is rebuilt on every scene load and means nothing.

The script scan is textual and therefore a heuristic: GDScript can build node
paths at runtime and no static analysis catches that. It is reported as a warning
for exactly that reason — it is evidence, not proof.

### Undo is git, not an undo stack

`EditorUndoRedoManager` exists, and using it was the first plan. It has two
problems for this use case: it does not cover files written outside the scene
(the generated `.gd`), and it does not survive an editor restart or a crash —
which is precisely when you most want to undo what an agent did.

So every mutating tool call ends with a commit:
`[ai-checkpoint] create_node_safe`. Rollback is `git revert`, never
`git reset --hard`. Reverting is additive: it cannot destroy uncommitted work
(and any that exists is parked in its own checkpoint first). The cost is a
noisier history. That is the correct trade for an undo button an autonomous
process is allowed to press.

The honest limitation: scene edits live in editor memory until saved, so a
checkpoint only captures what is on disk. Agents should call `save_scene` at the
end of a batch. The plugin will not save on your behalf — silently overwriting a
file a human has open is worse than losing an agent's work.

### The dock renders, it does not decide

`AIOSChatDock` emits four signals — `prompt_submitted`, `execute_plan_requested`,
`stop_requested`, `rollback_requested` — and exposes methods to render state. It
holds no transport, no tool registry, no policy. All of that lives in
`AIOSPlugin`.

This is what makes the "Stop" button trustworthy: it is not a request to the
agent's own loop to please stop. It broadcasts a `stop` event, sets the state
indicator, and — if a playtest is running — kills it directly through
`EditorInterface`. The human's override does not depend on the agent cooperating.

Agent text is BBCode-escaped before rendering. A model that emits `[img]` in its
output gets the literal characters, not a fetch.

### The plugin is editor-only

Classes are registered at `MODULE_INITIALIZATION_LEVEL_EDITOR` and nowhere else.
In an exported game the library loads and registers nothing. There is no runtime
component, no shipped attack surface, and nothing to strip before release.

The one runtime piece — `agent_log_bridge.gd`, which forwards playtest output —
is plain GDScript and deletes itself when it detects an export template.

---

## Milestone 2: the pipeline

Milestone 1's tools let an agent make changes. Milestone 2 wraps them in a loop
that is accountable for whether those changes worked:

```
Plan ──▶ Validate ──▶ Execute ──▶ Observe ──▶ Repair ──▶ Snapshot ──▶ Continue
```

The full treatment — stage-by-stage pseudocode, the process-interception code,
and the exact text fed back to the model — is in
**[PIPELINE.md](PIPELINE.md)**. The three decisions that shape everything else:

**Validation runs before execution, and twice.** Once per planned call
(`validate_planned_call`), and once over the whole scene after the batch
(`validate_scene`). The second pass exists because individually valid edits
combine badly: a delete that orphans a `NodePath` another step just set. Scripts
are compiled **in memory** — `set_source_code()` + `reload()` on a `GDScript`
instance — so a broken script never touches disk. That matters more than it
sounds: a bad `.gd` written to `res://` poisons the editor's script cache and
shows up in the FileSystem dock even if you delete it a moment later.

**Snapshot before, reset after.** A git commit is taken *before* each batch runs,
which is the only reason `git reset --hard` is a safe rollback here — there is
nothing uncommitted left for it to destroy. The reset is additionally guarded by
`git merge-base --is-ancestor`, so a stale sha is refused rather than obeyed. The
human's Rollback button deliberately does *not* use this path; outside the
pipeline the "everything was just committed" guarantee does not hold, so it stays
on `git revert`.

**Observation is a child process, not an API call.** `EditorInterface` can play a
scene, but it cannot hand you that scene's stderr. So `run_playtest` spawns Godot
with `--log-file` and tails the file from `_process`. `OS::execute()` would block
the editor for the whole playtest; `OS::execute_with_pipe()` blocks whenever the
pipe is empty, which is worse because it is intermittent. Tailing a file is
non-blocking, cross-platform, and captures GDScript stack traces with file, line
and function — which is the difference between an agent that fixes a runtime error
and one that guesses at it.

### Where the model configuration lives

`AIOSLlmClient` is a `Node` rather than a `RefCounted`, because `HTTPRequest` is:
it needs a scene tree to drive its own polling. Every request is asynchronous for
the same reason everything else is single-threaded — a blocking HTTP call would
freeze the editor, *including the Stop button whose entire job is to interrupt a
run that has gone wrong*.

The conversation history is stored in Anthropic's shape regardless of provider.
That is not favouritism: thinking blocks carry signatures the API requires to be
echoed back verbatim, and they have no OpenAI-shaped equivalent. Storing
OpenAI-native and converting up would discard data. So Anthropic requests are
near-passthrough and OpenRouter requests are translated on the way out and back.

---

## Milestone 3: editing, sight and assets

Milestone 2 could build a scene and tell you whether it ran. It could not
*change* one: an agent could create a node and never move it, attach a whole
script but never edit one, and had no idea where anything was in space. That
gap — not asset generation — was what actually blocked building a game.

**Editing is a superset of creating, and it needs different guarantees.**
`set_node_properties` shares its coercion path with `create_node_safe` because
two implementations disagreeing about whether `[64, 32]` is a `Vector2` would be
a maddening bug. It reads every value back after writing, because a Godot setter
can clamp or ignore what you gave it and reporting "applied" for a rejected value
sends the agent hunting in the wrong place.

`connect_signal_safe` uses `CONNECT_PERSIST`. Without that flag the connection
works until the scene reloads and then silently disappears — the kind of bug that
looks correct in the editor right up until it isn't.

`patch_script` exists because `attach_script_safe` rewrites the whole file from
whatever the model remembered. That is fine at 20 lines and destructive at 300.
Patches are compiled in memory before anything is written, so a patch that would
not parse leaves the file untouched.

**Spatial awareness is what makes 3D possible at all.** The property dump reports
`position` as a *local* offset and omits it entirely when it equals the default,
so an agent placing walls was working blind. The world model now carries
world-space position, rotation in degrees (radians are a reliable source of
silent factor-of-57 bugs), scale, and transformed AABB bounds — which is what
makes "does this overlap that" answerable.

**Sight closes the loop text leaves open.** `run_playtest` reports whether the
game *errored*. It cannot report that the level is unlit, the rifle is 40 metres
long, or the player spawned inside the floor — failures that produce no
diagnostic whatsoever. `capture_viewport_screenshot` hands the model the frame.

That required provider work: Anthropic accepts an image inside a `tool_result`,
and the OpenAI chat-completions schema rejects images there outright. So
OpenRouter requests keep the text in the `tool` message and re-send the image as
a following `user` message — the workaround that ecosystem settled on.

**Assets: the generation is the easy half.** Every service hands back a URL. The
part that fails quietly is everything after: a `.glb` written into `res://` is
invisible to the editor until `update_file` + `scan` runs the importer, and a
mesh arrives at arbitrary scale with no collision and 200k triangles.

Collision uses Godot's own supported mechanism rather than post-hoc scene
surgery: the importer builds a collision body from *mesh name suffixes*
(`-col`, `-convcol`, `-colonly`), so the Blender pass duplicates the mesh,
decimates the copy hard, and renames it. The engine does the rest.

Blender's role is deliberately narrow. An LLM writing `bpy` produces decent
parametric geometry and poor organic geometry, so procedural generation is
available but the valuable path is post-processing what a generation service
returned.

**Paid calls get their own gate.** `generate_3d_asset` costs money at a real
provider, and nothing else in the pipeline would stop a repair loop from calling
it repeatedly — the call *succeeds* every time, so there is no error to back off
from. Hence a per-session ceiling and a dock warning on every billed call.

---

## File map

| Path | What lives there |
| --- | --- |
| `src/register_types.cpp` | GDExtension entry point; registers classes at the editor init level. |
| `src/ipc/aios_ipc_server.*` | TCP listener, WebSocket and JSONL framing, token handshake, client bookkeeping. |
| `src/world/aios_world_model.*` | Scene-tree → JSON, caching, revision counter, node path resolution. |
| `src/tools/aios_scene_tools.*` | `create_node_safe`, `attach_script_safe`, `safe_delete_node`, `save_scene`, `open_scene`. |
| `src/tools/aios_tool_registry.*` | Tool table, dispatch, schema loading, checkpoint and cache policy. |
| `src/vcs/aios_git_checkpoint.*` | `git` subprocess wrapper; cross-platform git discovery; snapshots, ancestor-guarded hard reset, revert-based rollback. |
| `src/validate/aios_validator.*` | The Validate stage. In-memory GDScript compilation, NodePath resolution, property type checks, whole-scene sweep. |
| `src/playtest/aios_playtest.*` | The Observe stage. Child-process launch, `--log-file` tailing, diagnostic parsing. |
| `src/pipeline/aios_pipeline.*` | The loop itself: clarification interview, then Plan→Validate→Execute→Observe→Repair, plus the prompt-wrapper builders that feed failures back to the model. |
| `src/agent/aios_provider.*` | Request building and response normalisation for Anthropic and OpenRouter. |
| `src/agent/aios_llm_client.*` | Async HTTP transport, conversation history, model listing. |
| `src/agent/aios_credentials.*` | Encrypted API key store in `user://` for both model and asset providers; environment variables take priority. |
| `src/vision/aios_vision.*` | Editor viewport capture, downscaling and PNG encoding for multimodal feedback. |
| `src/assets/aios_asset_pipeline.*` | Text-to-3D generation, download, and getting the result imported. |
| `src/assets/aios_blender_bridge.*` | Headless Blender invocation: mesh cleanup and arbitrary bpy scripts. |
| `tools/blender/` | The Blender-side Python pipeline (also shipped inside the addon). |
| `src/editor/aios_plugin.*` | `EditorPlugin`; owns everything; message routing; project settings; session file. |
| `src/editor/aios_chat_dock.*` | The dock UI. |
| `src/util/aios_json.*` | Variant ⇄ JSON marshalling and type coercion. |
| `project/addons/godot_ai_os/schemas/` | JSON Schema for every tool — the source of truth agents receive. |
| `project/addons/godot_ai_os/runtime/` | The GDScript playtest log bridge. |
| `clients/python/` | Dependency-free reference client. |
