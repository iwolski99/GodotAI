# Architecture

How the pieces fit, and why they are shaped the way they are.

---

## The shape of the problem

An external agent wants to do four things to a Godot project: **read** its
structure, **change** it, **run** it, and **undo** the change when running it
goes badly. Milestone 1 builds the first, second and fourth, plus the transport
they all travel over.

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
 ┌──────────────────────────────────────────────────────────────────┐
 │  External agent  (Claude, Cursor, a script, another editor)      │
 └───────────────────────────────┬──────────────────────────────────┘
                                 │  JSON over WebSocket / TCP-JSONL
                                 │  loopback, token handshake
 ┌───────────────────────────────▼──────────────────────────────────┐
 │  AIOSIpcServer            src/ipc/                               │
 │  Accepts connections, frames messages, authenticates, emits      │
 │  message_received on the editor's main thread.                   │
 └───────────────────────────────┬──────────────────────────────────┘
                                 │
 ┌───────────────────────────────▼──────────────────────────────────┐
 │  AIOSPlugin               src/editor/                            │
 │  EditorPlugin. Owns everything, routes messages, polls the       │
 │  transport from _process, wires the dock to the tools.           │
 └───┬───────────────────────────┬───────────────────────────┬──────┘
     │                           │                           │
 ┌───▼──────────────┐  ┌─────────▼──────────┐  ┌─────────────▼─────┐
 │ AIOSChatDock     │  │ AIOSToolRegistry   │  │ AIOSGitCheckpoint │
 │ src/editor/      │  │ src/tools/         │  │ src/vcs/          │
 │ Renders state,   │  │ Dispatch + policy: │  │ Commits after     │
 │ emits intent.    │  │ checkpoint, cache  │  │ each mutation,    │
 │                  │  │ invalidation, log. │  │ reverts on undo.  │
 └──────────────────┘  └─────────┬──────────┘  └───────────────────┘
                                 │
                 ┌───────────────┴───────────────┐
                 │                               │
      ┌──────────▼─────────┐        ┌────────────▼──────────┐
      │ AIOSWorldModel     │        │ AIOSSceneTools        │
      │ src/world/         │        │ src/tools/            │
      │ The read path.     │        │ The write path.       │
      │ Cached JSON view   │        │ create / attach /     │
      │ of the live tree.  │        │ delete / save / open  │
      └────────────────────┘        └───────────────────────┘
```

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

## Where Milestone 2 plugs in

The seams are already cut:

- **Playtesting** — `EditorInterface::play_custom_scene` plus the existing
  `runtime_log` event path. The transport already handles the running game as
  just another client.
- **Validation** — `AIOSToolRegistry::call_tool` is the single funnel every
  mutation passes through. A validation pass goes there and applies to every
  tool at once, including ones not written yet.
- **Repair loops** — `world_changed` events plus the checkpoint stack give an
  agent everything it needs to detect a regression and walk back to a known-good
  state.
- **The state machine** — the dock already renders `PLANNING → VALIDATING →
  EXECUTING → PLAYTESTING → REPAIRING`. Milestone 1 lets an agent drive those
  labels manually; Milestone 2 makes the plugin drive them itself.

---

## File map

| Path | What lives there |
| --- | --- |
| `src/register_types.cpp` | GDExtension entry point; registers classes at the editor init level. |
| `src/ipc/aios_ipc_server.*` | TCP listener, WebSocket and JSONL framing, token handshake, client bookkeeping. |
| `src/world/aios_world_model.*` | Scene-tree → JSON, caching, revision counter, node path resolution. |
| `src/tools/aios_scene_tools.*` | `create_node_safe`, `attach_script_safe`, `safe_delete_node`, `save_scene`, `open_scene`. |
| `src/tools/aios_tool_registry.*` | Tool table, dispatch, schema loading, checkpoint and cache policy. |
| `src/vcs/aios_git_checkpoint.*` | `git` subprocess wrapper; checkpoint stack; revert-based rollback. |
| `src/editor/aios_plugin.*` | `EditorPlugin`; owns everything; message routing; project settings; session file. |
| `src/editor/aios_chat_dock.*` | The dock UI. |
| `src/util/aios_json.*` | Variant ⇄ JSON marshalling and type coercion. |
| `project/addons/godot_ai_os/schemas/` | JSON Schema for every tool — the source of truth agents receive. |
| `project/addons/godot_ai_os/runtime/` | The GDScript playtest log bridge. |
| `clients/python/` | Dependency-free reference client. |
