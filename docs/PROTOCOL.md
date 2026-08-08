# Wire protocol — `godot-ai-os/1`

Everything between an agent and the editor is a single JSON object per message.
There is no framing beyond what the transport provides, no binary payloads, and
no streaming responses. A message is one object; an object is one message.

---

## Transport

Both transports carry identical payloads. Pick whichever your language makes
easy — the plugin tells you which one is active in `session.json`.

| Mode | Framing | When to use it |
| --- | --- | --- |
| `websocket` (default) | RFC 6455 text frames, one JSON object per frame | Anything with a WebSocket client, including browsers |
| `tcp-jsonl` | Raw TCP, one JSON object per `\n`-terminated line | Shell scripts, embedded runtimes, anything where a WebSocket library is a burden |

The listener binds `127.0.0.1` by default. See the security notes in
[SETUP.md](SETUP.md#security) before changing that.

---

## Discovery

The plugin writes its connection details to
`<project>/.godot/ai_agent_os/session.json` on startup and deletes the file on
shutdown. Its presence is how you know an editor is live.

```json
{
  "protocol": "godot-ai-os/1",
  "host": "127.0.0.1",
  "port": 45857,
  "transport": "websocket",
  "token": "9f2c7ab1...",
  "project": "My Game",
  "project_path": "/home/you/my-game/",
  "started_at": 1786078521.86
}
```

The token is regenerated on every editor start. **Read this file when you
connect, never cache the token across sessions.**

---

## Handshake

The first message you send must be the handshake:

```json
{ "type": "hello", "token": "9f2c7ab1..." }
```

Anything else, or a wrong token, gets one error and the connection is closed:

```json
{ "type": "error", "error": "unauthorized", "message": "Send {\"type\":\"hello\"...} first." }
```

On success you get a welcome, immediately followed by the session manifest:

```json
{ "type": "welcome", "protocol": "godot-ai-os/1", "client_id": 3 }
```

```json
{
  "type": "event",
  "event": "session_ready",
  "data": {
    "protocol": "godot-ai-os/1",
    "world_revision": 0,
    "tools": [ { "name": "create_node_safe", "summary": "...", "mutating": true, "input_schema": { } } ]
  }
}
```

`session_ready` carries the complete JSON Schema for every tool, so a well-built
agent never needs a hardcoded tool list. Feed `input_schema` straight into your
model's tool-definition format.

---

## Requests and responses

**Request** (agent → editor):

```json
{ "id": "17", "type": "request", "tool": "create_node_safe", "params": { "type": "Timer", "name": "Spawner" } }
```

`id` is any string you choose; it comes back verbatim. Requests are handled in
arrival order on the editor's main thread, so responses come back in order too —
but match on `id` anyway, because events interleave freely.

**Response** (editor → agent), success:

```json
{ "id": "17", "type": "response", "tool": "create_node_safe", "ok": true,
  "result": { "path": "Spawner", "type": "Timer", "elapsed_msec": 1.4, "checkpoint": "de34c05e" } }
```

**Response**, failure:

```json
{ "id": "17", "type": "response", "tool": "create_node_safe", "ok": false,
  "error": { "code": "invalid_properties",
             "message": "One or more properties were rejected; nothing was created.",
             "details": { "errors": [ { "property": "positon", "problem": "unknown property for Sprite2D",
                                        "did_you_mean": ["position"] } ] } } }
```

Failures are always structured. `code` is a stable machine-readable identifier
(the per-tool set is listed in each schema's `errors` block); `message` is
written to be handed to a model verbatim; `details` carries whatever the tool
knows that would help fix the call. A failing tool never partially applies its
change.

---

## Events

Events are one-way and unsolicited in both directions. They have no `id` and are
never acknowledged.

### Editor → agent

| Event | Payload | Meaning |
| --- | --- | --- |
| `session_ready` | `{protocol, tools[], world_revision}` | Sent once, right after the handshake. |
| `user_prompt` | `{text, mode, world_revision}` | A human typed into the dock and hit Enter. `mode` is `architect` / `coder` / `debugger` / `playtester`. |
| `execute_plan` | `{mode}` | The human pressed **Execute Plan**. |
| `stop` | `{}` | The human pressed **Stop**. Abandon the current step. |
| `world_changed` | `{revision, cause}` | The project changed (possibly by another agent, or by the human). Your cached world model is stale. |
| `scene_saved` | `{path}` | A scene was written to disk. |
| `runtime_log` | `{stream, text}` | A line of console output from a playtest in progress. `stream` is `stdout` or `stderr`. |
| `playtest_finished` | the full playtest report | A `run_playtest` finished. **This is how you get the result** — the tool call itself only acknowledges the launch. |
| `status` | `{state, detail}` | The built-in agent's pipeline changed stage. Only sent when the in-editor agent is driving. |
| `agent_message` | `{text}` | The built-in agent said something. |
| `run_finished` | `{ok, message, steps_executed, filesystem_changed}` | A built-in-agent run ended. |

### Agent → editor

These drive the dock's UI. None of them are required, but an agent that sends
none of them is a black box to the human watching it.

| Event | Payload | Effect |
| --- | --- | --- |
| `chat` | `{text}` | Renders as an agent message. Fenced code blocks and `` `inline code` `` are formatted. |
| `thinking` | `{text}` | Renders as dimmed italic reasoning. |
| `log` | `{level, text}` | A log line. `level` is `info` / `warn` / `error` / `success`. |
| `status` | `{state, detail}` | Sets the pipeline indicator. `state` is `IDLE`, `CLARIFYING`, `PLANNING`, `VALIDATING`, `EXECUTING`, `PLAYTESTING`, `REPAIRING` or `ERROR`. |
| `runtime_log` | `{stream, text}` | Console output from a running playtest. Sent by the runtime log bridge, not usually by agents. |

Text arriving from an agent is BBCode-escaped before display, so markup in a
model's output cannot inject formatting or images into the editor UI.

---

## Asynchronous tools

Most tools return their result in the `response` to your `request`.

`ask_user` is interactive: the built-in pipeline pauses and waits for the human's
dock reply before synthesising a `tool_result`. Over IPC the call returns
`{queued: true, questions: [...]}` immediately — your harness presents the
questions and continues. Pair it with `commit_brief` to lock a 2D/3D design
brief before mutating the project.

`run_playtest` is asynchronous because the thing it is reporting on has not
happened yet. The response acknowledges the launch:

```json
{
  "type": "response", "id": "7", "tool": "run_playtest", "ok": true,
  "result": { "scene": "res://main.tscn", "pid": 15809,
              "timeout_sec": 45.0, "log_file": "user://godot_ai_os/playtest.log" }
}
```

…and the report arrives later as a `playtest_finished` event:

```json
{
  "type": "event", "event": "playtest_finished",
  "data": {
    "outcome": "errors",
    "passed": false,
    "summary": "The scene ran but logged 1 error(s).",
    "scene": "res://main.tscn",
    "elapsed_sec": 1.24,
    "exit_code": 0,
    "error_count": 1,
    "warning_count": 0,
    "diagnostics": [
      { "severity": "error", "kind": "script",
        "message": "Invalid access to property or key 'text' on a base object of type 'null instance'.",
        "file": "res://runtime_bomb.gd", "line": 5, "function": "_ready",
        "raw": "SCRIPT ERROR: Invalid access to property or key 'text' ..." }
    ],
    "output_tail": ["Godot Engine v4.4.1.stable.official"],
    "output_truncated": false
  }
}
```

While it runs, `runtime_log` events stream the console output line by line.

Save the scene before you playtest. The child process reads the project from
disk, not from the editor's unsaved state, so an unsaved edit is invisible to it.

Outcomes are `clean`, `errors`, `crashed` and `timeout`. Only `clean` sets
`passed: true`. A `timeout` is not by itself a failure — a game with no exit
condition always hits it — so pass `quit_after_frames` when you want a
deterministic smoke test.

---

## Multiple clients

The bridge accepts any number of simultaneous connections, all equal peers:

- Responses go only to the client that made the request.
- Events broadcast to every authenticated client.
- A running playtest is just another client (that is how `runtime_log` arrives).

Two agents editing the same scene will not corrupt it — every tool call is
serialised on the editor's main thread — but they will happily undo each other's
work. Watch `world_changed.revision` and re-read the world model when it moves.

---

## Worked example

```
client → { "type": "hello", "token": "9f2c..." }
server → { "type": "welcome", "protocol": "godot-ai-os/1", "client_id": 1 }
server → { "type": "event", "event": "session_ready", "data": { "tools": [ ... ] } }

server → { "type": "event", "event": "user_prompt",
           "data": { "text": "give the player a jump", "mode": "coder", "world_revision": 4 } }

client → { "type": "event", "event": "status", "data": { "state": "PLANNING", "detail": "reading the scene" } }
client → { "id": "1", "type": "request", "tool": "get_world_model", "params": { "max_depth": 3 } }
server → { "id": "1", "type": "response", "ok": true, "result": { "revision": 4, "scene": { ... } } }

client → { "type": "event", "event": "chat", "data": { "text": "Player is a CharacterBody2D. Adding a jump." } }
client → { "type": "event", "event": "status", "data": { "state": "EXECUTING", "detail": "writing player.gd" } }
client → { "id": "2", "type": "request", "tool": "attach_script_safe",
           "params": { "node": "Player", "source": "extends CharacterBody2D\n...", "replace": true } }
server → { "id": "2", "type": "response", "ok": true,
           "result": { "script": "res://scripts/player.gd", "checkpoint": "a91f0c22" } }
server → { "type": "event", "event": "world_changed", "data": { "revision": 5, "cause": "attach_script_safe" } }

client → { "id": "3", "type": "request", "tool": "save_scene", "params": {} }
client → { "type": "event", "event": "status", "data": { "state": "IDLE", "detail": "done" } }
```

---

## Versioning

`protocol` appears in the session file, the welcome, and the `list_tools`
result. Within `godot-ai-os/1`, new tools, new optional parameters and new
result fields may appear at any time; existing parameter meanings, error codes
and event names will not change. A breaking change bumps the number.
