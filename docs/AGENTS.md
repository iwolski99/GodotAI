# Connecting an agent

The plugin does not talk to any model. It exposes tools and a message bus; your
agent harness supplies the intelligence. That split is deliberate — it means the
plugin works with whatever you already use, and does not go stale when a new
model ships.

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

## A system prompt that works

The tools enforce safety, but they cannot enforce *sequence*. These are the rules
that turn a model that flails into one that doesn't:

```
You are editing a live Godot 4 project through the AI Agent OS bridge.

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

There is no MCP server in this repo yet. Writing one is a thin wrapper — the
tool schemas are already JSON Schema, and `session_ready` hands you the whole
manifest at runtime, so an MCP server is roughly:

```python
mcp_tools = [
    types.Tool(name=t["name"], description=t["summary"], inputSchema=t["input_schema"])
    for t in godot.tools.values()
]
# ... and forward call_tool straight through to godot.call()
```

If you build one, a pull request would be welcome — see
[CONTRIBUTING.md](../CONTRIBUTING.md).

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
