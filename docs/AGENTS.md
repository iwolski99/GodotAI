# Connecting an agent

There are two ways to drive this plugin, and they are equally supported.

**The built-in agent.** Add an API key in the dock's Settings panel and type a
goal. The plugin runs the whole
[Plan → Validate → Execute → Observe → Repair](PIPELINE.md) loop itself, against
Anthropic or OpenRouter. Nothing to write. See
[SETUP.md §5](SETUP.md#5-configure-the-built-in-agent).

**Your own harness over the bridge.** The plugin exposes tools and a message bus;
you supply the intelligence. This is what the rest of this document covers. Use
it when you already have a harness, when you want a model or provider the
built-in client does not speak, or when the agent needs to do things outside
Godot as well.

Both reach the project through the same tool registry and receive the same
manifest, so neither is a second-class path.

---

## The 20-line version

```python
import sys; sys.path.insert(0, "clients/python")
from godot_ai_client import GodotAIClient

with GodotAIClient.from_project("/path/to/your-project") as godot:
    godot.set_state("PLANNING", "reading the scene")
    world = godot.call("get_world_model", {"max_depth": 3})

    godot.set_state("EXECUTING", "adding a player")
    godot.call("create_node_safe", {
        "type": "CharacterBody2D",
        "name": "Player",
        "properties": {"position": [320, 180]},
    })
    godot.call("attach_script_safe", {
        "node": "Player",
        "source": "extends CharacterBody2D\n\nconst SPEED := 300.0\n\n"
                  "func _physics_process(_d: float) -> void:\n"
                  "\tvelocity.x = Input.get_axis(\"ui_left\", \"ui_right\") * SPEED\n"
                  "\tmove_and_slide()\n",
    })
    godot.call("save_scene", {})

    godot.say("Added a Player with left/right movement.")
    godot.set_state("IDLE", "done")
```

---

## Driving it from the dock

To make the dock's input box actually do something, listen for `user_prompt` and
answer it. This is the whole loop:

```python
from godot_ai_client import GodotAIClient, ToolError

with GodotAIClient.from_project(project) as godot:
    tool_definitions = [
        {"name": t["name"], "description": t["summary"],
         "input_schema": t["input_schema"]}
        for t in godot.tools.values()
    ]

    for event in godot.events():
        name = event.get("event")

        if name == "user_prompt":
            prompt = event["data"]["text"]
            mode = event["data"]["mode"]          # architect / coder / debugger / playtester
            godot.set_state("PLANNING", "thinking")
            run_your_model(prompt, mode, tool_definitions, godot)

        elif name == "stop":
            cancel_whatever_is_running()
            godot.set_state("IDLE", "stopped")

        elif name == "world_changed":
            invalidate_your_cached_world_model()
```

`godot.tools` is populated from the `session_ready` event and contains the full
JSON Schema of every tool, so `tool_definitions` above needs no maintenance when
the plugin gains a tool.

---

## Wiring it to Claude

The tool schemas the plugin ships are already in the shape the Claude API's
`tools` parameter expects, so the adapter is thin:

```python
import anthropic
from godot_ai_client import GodotAIClient, ToolError

client = anthropic.Anthropic()
godot = GodotAIClient.from_project(project)

tools = [{"name": t["name"], "description": t["summary"], "input_schema": t["input_schema"]}
         for t in godot.tools.values()]

messages = [{"role": "user", "content": prompt}]
while True:
    response = client.messages.create(
        model="claude-opus-5", max_tokens=8192, tools=tools, messages=messages,
        system=SYSTEM_PROMPT,
    )
    messages.append({"role": "assistant", "content": response.content})

    results = []
    for block in response.content:
        if block.type == "text":
            godot.say(block.text)
        elif block.type == "tool_use":
            godot.set_state("EXECUTING", block.name)
            try:
                out = godot.call(block.name, block.input)
                results.append({"type": "tool_result", "tool_use_id": block.id,
                                "content": json.dumps(out)})
            except ToolError as exc:
                # Hand the structured error straight back — it is written to be
                # actionable, and the model will usually fix its own call.
                results.append({"type": "tool_result", "tool_use_id": block.id,
                                "is_error": True,
                                "content": json.dumps({"code": exc.code,
                                                       "message": str(exc),
                                                       "details": exc.details})})
    if not results:
        break
    messages.append({"role": "user", "content": results})

godot.set_state("IDLE", "")
```

The same shape works with any harness that speaks tools; only the SDK calls
change.

---

## Clarifying before you build

If you are driving the bridge yourself, copy the built-in interview: when the
human says something like "make me an FPS", do **not** start creating nodes.
Ask until you know at least:

- **2D or 3D** (never guess — it changes every node type)
- Genre specifics and camera/feel
- Controls
- Win / lose (or sandbox)
- MVP scope for *this* session
- A buildable art direction (primitives are fine)

The plugin exposes two tools for this: `ask_user` (pause with questions) and
`commit_brief` (lock the brief; the built-in pipeline then unlocks build tools).
External harnesses can implement the same handshake over chat events if they
prefer not to use those tools.

## A system prompt that works

The tools enforce safety, but they cannot enforce *sequence*. These are the rules
that turn a model that flails into one that doesn't:

```
You are editing a live Godot 4 project through the AI Agent OS bridge.

If the goal is underspecified, interview the human before building. Resolve
2D vs 3D explicitly. Prefer ask_user / commit_brief (or an equivalent chat
handshake) over inventing a generic game.

Before you change anything, call get_world_model. Node paths are relative to
the scene root, which is ".". Never guess a path.

Prefer dry_run: true for any call you are not certain about. The validation
report tells you exactly what would happen, and it costs nothing.

Scene edits live in the editor's memory until you call save_scene. Call it once
at the end of a coherent batch of edits — not after every node.

When a tool returns an error, read it. The `code` says what class of problem it
is, `message` says what to do about it, and `details` usually contains the fix
(a did_you_mean for a misspelled property, a blockers list for a delete). Fix
the call and retry rather than trying a different tool.

safe_delete_node blockers exist for a reason. Do not pass force: true to get
past one unless the human asked you to, or you have removed the dependency the
blocker names.

Tell the human what you are doing with chat and status events as you go. They
are watching a dock, not a log file, and they can stop you at any point.
```

---

## Model Context Protocol

An MCP server ships at `clients/mcp/server.py`. It forwards every tool from the
editor bridge to any MCP-capable client (including Cursor).

```bash
pip install -r clients/mcp/requirements.txt
python3 clients/mcp/server.py /path/to/your-project
```

Add to `.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "godot-ai-os": {
      "command": "python3",
      "args": ["/path/to/GodotAI/clients/mcp/server.py", "/path/to/your-project"]
    }
  }
}
```

The Godot editor must be running with the plugin enabled. Tool schemas come from
`session_ready` at connect time — no manual manifest maintenance.

---

## Things that will bite you

**The token changes every time the editor restarts.** Re-read `session.json` on
connect. If the file is missing, no editor is running.

**Multiple agents can connect at once.** They are equal peers and will happily
overwrite each other's work. Watch `world_changed.revision`.

**`get_world_model` omits instanced sub-scene internals by default.** They are
not editable from the parent scene, so editing them is not possible through
these tools either — open the sub-scene with `open_scene` instead. Pass
`include_instanced_children: true` if you only need to look.

**`include_properties` is off by default** because it multiplies response size.
Turn it on for the specific query where you need it, not globally.

**A failed `attach_script_safe` deletes the file it created.** This is
intentional — a `.gd` that does not parse poisons the project's script cache.
Fix the source and call again.

---

## Borrowing the built-in agent's prompt

The system prompt the in-editor pipeline uses is assembled in
`AIOSPipeline::build_system_prompt()`, and it is worth reading even if you are
writing your own — [PIPELINE.md](PIPELINE.md#the-system-prompt) explains the
reasoning.

The part most harnesses get wrong is telling the model what happens *around* its
tool calls. A model that does not know a validation gate exists treats a rejection
as a mysterious failure and starts guessing. A model that knows treats it as
review and corrects the call. One paragraph is enough:

> Every tool call you make goes through a fixed pipeline: it is validated
> statically, then executed, then the scene is re-checked, and the result comes
> back to you. A call that fails validation is NOT executed — you get the findings
> and a chance to correct it, and the project is untouched.

If your harness implements its own rollback, say so too, and say it plainly:

> Before each batch of changes the pipeline takes a git snapshot. If your changes
> break the project, it resets to that snapshot and tells you. Rollback is real:
> your edits genuinely disappear. Retrying the same thing after a rollback wastes
> a cycle.

And when the project is *not* a git repository, invert it rather than omitting it
— an agent that assumes an undo it does not have takes bigger swings than it
should:

> This project is NOT a git repository, so there is no rollback. Every change you
> make is permanent. Prefer `dry_run` first, and prefer small reversible steps
> over large ones.

### Feeding failures back

When a validation or playtest failure goes back to the model, send the specific
finding, its location, and what you did about it — not a summary.
[PIPELINE.md](PIPELINE.md#the-prompt-wrapper) has the three exact templates the
built-in agent uses; they are short, and copying them is faster than rediscovering
why `"validation failed"` on its own produces a guess instead of a fix.
