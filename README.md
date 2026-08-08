# Godot AI Agent OS

**An operating system for AI agents inside the Godot editor.** A native C++
GDExtension that lets an AI agent inspect, build, edit and *playtest* a Godot 4
project through a typed, validated tool API — with a chat dock in the editor so
you can watch it work and stop it when it goes wrong.

Type a goal. For vague requests the agent interviews you first (2D vs 3D, genre,
controls, scope) until the brief is solid — then it plans, checks each change
before making it, runs the game, reads the errors, fixes them, and rolls back to
a git snapshot if it can't.

No copy-pasting code out of a chat window. No agent guessing at your scene tree.
No wondering what it just did to your project.

```
   Clarify ──▶ Plan ──▶ Validate ──▶ Execute ──▶ Observe ──▶ Repair ──▶ Snapshot
      │          │           │            │           │          │           │
      │          │           │            │           │          │           └─ git commit
      │          │           │            │           │          └─ feed the error back
      │          │           │            │           └─ run the game, parse the stack trace
      │          │           │            └─ change the project
      │          │           └─ compile the script in memory; refuse it if it's broken
      │          └─ your model, or ours
      └─ ask until 2D/3D + brief are locked (or Skip & Build)
```

> **Status: all three milestones shipped.** Tools, the autonomous pipeline with
> git rollback and playtest error interception, the editing and spatial tools,
> viewport vision, and the generated-asset gateway are complete and tested
> against Godot 4.4.1 on Linux. Two things are written but unverified: the
> **Windows** build paths and the **Blender** post-processing script (neither
> Windows nor Blender exists in the development environment). See the
> [roadmap](docs/ROADMAP.md).

![The AI Agent dock docked on the right of the Godot editor, showing the pipeline state header, the log, the prompt box and the action bar](docs/images/dock.png)

*The dock: a pipeline state header, a log of every tool call and its result, a
prompt box, and the buttons that matter — Execute Plan, Stop, Rollback. Settings
open in a dialog so they never eat the history.*

---

## Why

Every "AI writes your game" workflow today has the same hole in the middle: the
model produces code, and then a human becomes a clipboard. The agent cannot see
the scene tree, so it invents node paths. It cannot check a property type, so it
writes a string where a `Vector2` goes. It cannot tell that deleting `Enemy`
breaks a script three folders away. And when it gets something wrong, there is no
undo that covers both the scene and the four files it generated.

This plugin closes that hole. It gives the agent a real interface to the editor —
one that answers questions honestly, rejects malformed changes with an
explanation, and records a git checkpoint after every mutation so any of it can
be walked back with one button.

---

## What's in Milestone 3

**It can edit, not just add.** `set_node_properties` moves and configures what
already exists. `create_scene` makes reusable prefabs. `connect_signal_safe`
wires `body_entered` to `take_damage` — with `CONNECT_PERSIST`, so the connection
is written into the `.tscn` instead of vanishing on the next scene load.
`patch_script` replaces one function instead of rewriting a 300-line file from
memory, and refuses to write a patch that doesn't compile.

**It knows where things are.** The world model carries world-space position,
rotation in degrees, scale and AABB bounds. Before this, an agent placing a wall
next to another wall was guessing: `position` in a property dump is a *local*
offset and is omitted entirely when it matches the default.

**It can see.** `capture_viewport_screenshot` hands the model the actual frame.

```
run_playtest        →  "did it error?"
capture_viewport    →  "does it look right?"
```

Those are different questions. An unlit level, a rifle at 40× scale, a player
spawned inside the floor — none of them produce a single line of diagnostic
output.

**It can get assets.** Text-to-3D through Meshy or Tripo3D, downloaded and
imported. Or `import_asset_from_url` for any service this plugin has never heard
of. Keys live in the encrypted store, never in `project.godot`.

**And clean them up.** Generated meshes arrive at arbitrary scale, Z-up, 200k
triangles, no collision. `cleanup_mesh` runs them through headless Blender:
decimate to a triangle budget, normalise scale to metres, weld seam vertices,
drop the origin to the footprint. Collision uses Godot's own import-time
mechanism — the mesh is duplicated, decimated hard, and renamed `-convcol` so
the engine builds the body itself.

**Paid calls are gated.** `generate_3d_asset` costs real money, and a repair loop
would happily call it five times because the call *succeeds* every time. A
per-session ceiling and a dock warning on every billed call.

---

## What's in Milestone 2

The loop that makes an agent accountable for whether its changes actually work.
Full write-up: **[docs/PIPELINE.md](docs/PIPELINE.md)**.

**Pre-execution validation.** Every planned call is checked before it runs, and
a call that fails is *never executed* — the model gets the findings and revises,
and the project is untouched. GDScript is compiled **in memory**, so a broken
script never reaches disk. `extends` is checked against the target node's real
class. Node paths are resolved against the open scene. Property bags are
type-checked against `ClassDB`. After the batch, a whole-scene sweep catches the
damage individually-valid edits do together — a delete that orphans a `NodePath`
another step just set.

**Playtest error interception.** `run_playtest` launches the game in a child
process and tails its output, turning engine errors and GDScript stack traces
into structured diagnostics:

```json
{ "severity": "error", "kind": "script", "line": 5, "function": "_ready",
  "file": "res://player.gd",
  "message": "Invalid access to property or key 'text' on a base object of type 'null instance'." }
```

That location is the difference between an agent that fixes a runtime error and
one that guesses at it. Outcomes are `clean` / `errors` / `crashed` / `timeout`,
and `quit_after_frames` gives you a deterministic smoke test.

**Transactional git rollback.** A snapshot is committed *before* each batch runs,
which is the only reason `git reset --hard` is safe here — there is nothing
uncommitted left to destroy. The reset is guarded by
`git merge-base --is-ancestor`, so a stale sha is refused rather than obeyed. The
human's Rollback button stays on the additive `git revert` path, because outside
the pipeline that guarantee does not hold.

**A repair budget.** After three consecutive failures the pipeline stops asking
the model to try again: it rolls back and hands control to you. A model that has
failed three times on the same problem is usually wrong about what the problem is.

**A built-in agent.** Anthropic or OpenRouter, configured in the dock — model
selector fetched live from the provider, thinking on/off, reasoning effort from
`low` to `max`, output token budget. API keys are stored encrypted in `user://`,
never under `res://`, and an environment variable takes priority and is never
written anywhere.

The bridge is unchanged and still first-class: both agents reach the project
through the same tool registry and get the same manifest.

---

## What's in Milestone 1

**A transport.** A loopback WebSocket (or newline-delimited TCP) server inside
the editor, with a per-session token and a discovery file agents read to connect.
Polled on the main thread — no threading bugs waiting to happen in your editor.

**A world model.** A cached JSON view of the *live* scene tree — not the `.tscn`
on disk, which is always behind. Node types, paths, attached scripts, groups,
sub-scene boundaries, the project's script and scene inventory. A revision
counter tells an agent when its view went stale.

**Four tools that refuse to do the wrong thing.**

| Tool | What makes it "safe" |
| --- | --- |
| `get_world_model` | Reads the editor's memory, not stale files. Cached, revision-tracked. |
| `create_node_safe` | Every property checked against the class *before* instantiation. `[64, 32]` becomes a `Vector2`, `"#ff8800"` becomes a `Color`, a typo comes back with `did_you_mean`. Sets `owner` so the node actually persists. |
| `attach_script_safe` | Verifies `extends` against the node's real class, compiles the script, and deletes the file again if it doesn't parse. |
| `safe_delete_node` | Audits children, authored signal connections, `NodePath` properties pointing into the subtree, and `$Name` references in project scripts. Blockers stop the delete; warnings are reported. |

Plus `save_scene`, `open_scene`, `validate_change`, `validate_scene`,
`run_playtest`, `create_checkpoint`, `rollback_last`, `list_tools` and `ping`.
Every one ships a [JSON Schema](project/addons/godot_ai_os/schemas/) that is
handed to the agent on connect — so your tool definitions never drift from the
implementation.

**An editor dock.** Chat history with colour-coded tool calls and results, a
prompt box (Enter sends, Shift+Enter newlines), an agent mode selector, a live
`[PLANNING] [VALIDATING] [EXECUTING] [PLAYTESTING] [REPAIRING]` state header, a
settings panel for the model, and three buttons that matter: **Execute Plan**,
**Stop**, **Rollback Last**.

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/iwolski99/GodotAI.git
cd GodotAI
pip install scons
scons platform=linux target=editor -j$(nproc)      # or macos / windows
```

Open `project/` in Godot 4.4+. The **AI Agent** dock appears on the right — there
is no plugin checkbox to tick, because this is a native `EditorPlugin` that
activates when the extension loads.

Then talk to it:

```bash
python3 clients/python/godot_ai_client.py ./project ping
python3 clients/python/godot_ai_client.py ./project get_world_model '{"max_depth": 2}'
python3 clients/python/godot_ai_client.py ./project create_node_safe \
    '{"type": "Sprite2D", "name": "Player", "properties": {"position": [64, 32]}}'
```

The node appears in the editor immediately, and the call shows up in the dock.

Or skip the client entirely: press **Settings** in the dock, paste an Anthropic
or OpenRouter API key, pick a model, and type what you want built.

Full instructions, including installing into your own project and configuring the
built-in agent: **[docs/SETUP.md](docs/SETUP.md)**.

---

## Connecting your own agent

The built-in agent is optional. The bridge exposes tools; your harness supplies
the intelligence. The reference client is 400 lines of dependency-free Python:

```python
from godot_ai_client import GodotAIClient

with GodotAIClient.from_project("/path/to/your-project") as godot:
    world = godot.call("get_world_model", {"max_depth": 3})
    godot.call("create_node_safe", {"type": "CharacterBody2D", "name": "Player"})
    godot.call("attach_script_safe", {"node": "Player", "source": "extends CharacterBody2D\n..."})
    godot.call("save_scene", {})
```

`godot.tools` arrives pre-populated with every tool's JSON Schema, ready to hand
straight to a model's tool-definition parameter. Worked examples for the Claude
API, a system prompt that produces sane agent behaviour, and the dock-driven
event loop: **[docs/AGENTS.md](docs/AGENTS.md)**.

---

## Documentation

| | |
| --- | --- |
| **[Setup](docs/SETUP.md)** | Build, install, configure, troubleshoot. Start here. |
| **[Protocol](docs/PROTOCOL.md)** | The wire format, message by message. |
| **[Architecture](docs/ARCHITECTURE.md)** | How it works and why it's built this way. |
| **[Connecting agents](docs/AGENTS.md)** | Harness integration, Claude example, system prompt. |
| **[Pipeline](docs/PIPELINE.md)** | The agent loop: diagrams, pseudocode, error interception, prompt wrappers. |
| **[Assets](docs/ASSETS.md)** | Generation, the Blender cleanup pass, collision, and what it costs. |
| **[Roadmap](docs/ROADMAP.md)** | Milestone 3, what shipped, and what's deliberately out of scope. |
| **[Tool schemas](project/addons/godot_ai_os/schemas/)** | The authoritative tool contracts. |

---

## Requirements

- Godot **4.4+** (built against `godot-cpp` 4.4; runs on 4.4 and later)
- A C++17 compiler and SCons 4.0+ to build
- `git` on `PATH` for checkpoints and rollback

Optional, for the built-in agent: an [Anthropic](https://console.anthropic.com/)
or [OpenRouter](https://openrouter.ai/) API key. Driving the plugin from your own
harness over the bridge needs no key.

Optional, for generated assets: a [Meshy](https://www.meshy.ai/) or
[Tripo3D](https://www.tripo3d.ai/) key, and [Blender](https://www.blender.org/)
4.x on `PATH` for the mesh cleanup pass. Everything else works without them.

Linux, macOS and Windows are all supported by the build. Milestones 1 and 2 have
been tested end to end on Linux against Godot 4.4.1. The Windows code paths —
MSVC build, git discovery, line-ending handling — are written and reviewed but
have not been run on a Windows machine; macOS follows standard `godot-cpp`
conventions and is likewise untested. Reports on either are genuinely useful.

---

## Security, plainly

The bridge lets whatever connects to it write files into your project and drive
your editor.

- It binds **`127.0.0.1`** by default. Nothing off your machine can reach it.
- The per-session token stops a stray local process or a browser tab probing
  localhost from connecting by accident. It is **not** a credential — anything
  that can read your project folder can read it. Everything on your machine is
  inside the trust boundary.
- Binding to `0.0.0.0` exposes an editor that can write arbitrary files to your
  whole network. The plugin warns you when you do it. Don't.
- Keep the project in git. The checkpoint after every mutation is what makes an
  autonomous agent's mistakes recoverable — and without a repository the pipeline
  cannot roll back at all, which it warns you about at the start of every run.
- **API keys never touch `res://`.** They go in `user://`, encrypted, outside your
  project — or better, in an environment variable that is read and never stored.
  A key in a project folder gets committed, pushed, and scraped.
- `git reset --hard` is only used on a snapshot the pipeline took moments earlier,
  and only after `git merge-base --is-ancestor` confirms it is in your history.
  The Rollback button you press yourself never uses it.

---

## Contributing

Issues, ideas and pull requests are all welcome — especially **macOS and Windows
build reports**, which are the biggest gap right now, and opinions about what
Milestone 3 should actually contain. See **[CONTRIBUTING.md](CONTRIBUTING.md)**.

---

## Licence

MIT. See [LICENSE](LICENSE).

`godot-cpp` is included as a submodule and is licensed separately under the MIT
licence by the Godot Engine contributors.
