# The agent pipeline

`Clarify → Plan → Validate → Execute → Observe → Repair → Snapshot → Continue`

This is the document for the Milestone 2 runtime loop (plus the clarification
interview added on top): what each stage does, the pseudocode it follows, the
C++ that intercepts a running game's output, and the exact text handed back to
the model when something fails.

---

## Why a pipeline at all

An agent with tools and no pipeline is a loop: call a tool, read the result, call
another. That works right up until a change breaks something, and then it fails in
a specific and expensive way — the model does not find out that its edit was wrong
until several steps later, by which point it is debugging a project that three
subsequent edits have moved on from.

The pipeline exists to close that gap. Every step is checked before it runs and
after it runs, and the project is snapshotted in between, so the answer to "did
that work?" arrives while it is still cheap to act on.

Two properties carry the whole design:

1. **A call that fails validation is never executed.** The model gets the findings
   as a tool error and revises. A rejected call costs one round trip. A bad
   executed call costs a playtest, a rollback, and a confused repair attempt.

2. **Every executed step is bracketed by a git snapshot taken *before* it ran.**
   That ordering is what makes `git reset --hard` a safe operation here rather
   than a destructive one: there is nothing uncommitted left to destroy.

---

## The loop

```
                        ┌──────────────────────────────┐
                        │  human types a goal in the   │
                        │  dock, picks a mode          │
                        └───────────────┬──────────────┘
                                        │
                        ┌───────────────▼──────────────┐
                        │  SNAPSHOT (run baseline)     │  git commit
                        │  AIOSGitCheckpoint           │  last_good_snapshot
                        └───────────────┬──────────────┘
                                        │
                        ┌───────────────▼──────────────┐
                        │  CLARIFY  (architect/coder)  │  ask_user / commit_brief
                        │  Build tools withheld until  │  dock answers / Skip & Build
                        │  a 2D/3D brief is locked     │
                        └───────────────┬──────────────┘
                                        │ brief committed
           ┌────────────────────────────▼──────────────┐
    ┌─────▶│  PLAN                                     │
    │      │  AIOSLlmClient -> Anthropic / OpenRouter  │
    │      │  system prompt + tool manifest + history  │
    │      └────────────────────────────┬──────────────┘
    │                                   │  tool_calls[]
    │                        no calls?  │
    │                        ──────────▶│──────▶  done, run_finished
    │                                   │
    │      ┌────────────────────────────▼──────────────┐
    │      │  SNAPSHOT (per batch)                     │  step_snapshot
    │      │  one commit for the whole batch           │
    │      └────────────────────────────┬──────────────┘
    │                                   │
    │      ┌────────────────────────────▼──────────────┐
    │      │  VALIDATE          for each call          │
    │      │  AIOSValidator::validate_planned_call     │
    │      │  script compiles? paths resolve?          │
    │      │  properties typed correctly?              │
    │      └───────┬────────────────────────┬──────────┘
    │              │ errors                 │ clean
    │              │                        │
    │      ┌───────▼────────────┐  ┌────────▼──────────────────────────┐
    │      │  REFUSE            │  │  EXECUTE                          │
    │      │  not executed;     │  │  AIOSToolRegistry::call_tool      │
    │      │  findings -> model │  │  the project actually changes     │
    │      └───────┬────────────┘  └────────┬──────────────────────────┘
    │              │                        │
    │              │        ┌───────────────▼──────────────┐
    │              │        │  OBSERVE                     │
    │              │        │  validate_scene() sweep      │
    │              │        │  + run_playtest if requested │
    │              │        │  AIOSPlaytest tails the log  │
    │              │        └───────────────┬──────────────┘
    │              │                        │
    │              │              ┌─────────┴─────────┐
    │              │        clean │                   │ broken
    │              │              │                   │
    │              │  ┌───────────▼────────┐  ┌───────▼─────────────────┐
    │              │  │  SNAPSHOT          │  │  REPAIR                 │
    │              │  │  checkpoint commit │  │  repair_attempts++      │
    │              │  │  last_good = sha   │  │  findings -> model      │
    │              │  └───────────┬────────┘  └───────┬─────────────────┘
    │              │              │                   │
    │              │              │        budget spent?
    │              │              │                   │
    │              │              │           ┌───────▼─────────────────┐
    │              │              │           │  ROLLBACK               │
    │              │              │           │  git reset --hard       │
    │              │              │           │  step_snapshot          │
    │              │              │           │  -> abort, hand back    │
    │              │              │           │     to the human        │
    │              │              │           └─────────────────────────┘
    │              │              │
    └──────────────┴──────────────┘
              CONTINUE: results go back as tool_result blocks
```

### Clarification before build

Vague goals like "make me an FPS" must not become a generic game. In
`architect` and `coder` modes the pipeline starts in **CLARIFYING**:

1. Only interview tools are offered: `ask_user`, `commit_brief`, plus read-only
   helpers (`get_world_model`, `list_tools`, `ping`, `read_script`, `open_scene`).
2. The model asks focused questions (2D vs 3D, genre, controls, win/lose, MVP
   scope, art direction) via `ask_user`. The dock pauses on
   `user_questions_requested`; the next Send is the human's answer.
3. When the brief is solid, the model calls `commit_brief` with
   `dimensions: "2d" | "3d"`. Build tools unlock and the normal loop continues.
4. **Skip & Build** (the Execute button during clarification) synthesises a
   minimal brief from the original goal and builds immediately — so the human
   is never trapped in the interview.

Debugger and playtester modes skip clarification. Project Settings →
**AI Agent OS → agent/require_brief** turns the interview off entirely.

Every C++ class in Milestone 2 serves exactly one stage:

| Stage | Class | File |
| --- | --- | --- |
| Clarify | `AIOSPipeline` (`ask_user`, `commit_brief`) | `src/pipeline/`, schemas |
| Plan | `AIOSLlmClient`, `AIOSProvider` | `src/agent/` |
| Validate | `AIOSValidator` | `src/validate/` |
| Execute | `AIOSToolRegistry`, `AIOSSceneTools` | `src/tools/` |
| Observe | `AIOSPlaytest`, `AIOSValidator::validate_scene` | `src/playtest/`, `src/validate/` |
| Repair | `AIOSPipeline::build_*_feedback` | `src/pipeline/` |
| Snapshot / Rollback | `AIOSGitCheckpoint` | `src/vcs/` |
| Orchestration | `AIOSPipeline` | `src/pipeline/` |

---

## Pseudocode

The real implementation is [`src/pipeline/aios_pipeline.cpp`](../src/pipeline/aios_pipeline.cpp);
this is the same control flow with the plumbing removed.

### Starting a run

```
start(goal, mode):
    if stage is not IDLE:        return error "already_running"
    if no model configured:      return error "no_model"

    reset counters, clear pending results
    clarifying = require_brief and mode in (architect, coder)

    git_ok = git.is_available()
    if git_ok:
        last_good_snapshot = git.create_snapshot("run start: " + goal)
    else:
        warn the human that rollback is unavailable

    llm.reset_conversation()
    llm.system_prompt = build_system_prompt(mode, git_ok, clarifying)
    llm.tools         = clarifying
                            ? clarify-phase tools only
                            : registry.list_tools()

    if clarifying:
        stage = CLARIFYING
        llm.send_user_message(goal + clarification nudge)
    else:
        stage = PLANNING
        llm.send_user_message(goal)                # async; returns immediately
```

Nothing blocks. The editor keeps running; the response arrives on a signal.
While `ask_user` is outstanding, the dock routes the next Send to
`continue_with_user_answer` instead of starting a new run.

### A model turn

```
on_model_response(response):
    if stage is IDLE:  return                      # late reply from a stopped run

    emit thinking, emit text                       # straight to the dock

    if response.tool_calls is empty:
        if clarifying and not brief_ready:
            stage = CLARIFYING                     # wait for dock answers
            return
        stage = IDLE
        emit run_finished(ok)                      # the model considers it done
        return

    handle_tool_calls(response.tool_calls)
```

### Executing a batch

```
handle_tool_calls(calls):
    # One snapshot per batch, not per call. A model routinely emits several
    # calls that only make sense together (create a node, then attach its
    # script); rolling back to the middle of that would leave a half-built
    # scene that is worse than either end state.
    if git_ok and any call is mutating:
        step_snapshot = git.create_snapshot("before step N")

    for call in calls:
        if awaiting_playtest:
            deferred_calls.append(call)            # queue behind the running game
        else:
            execute_call(call)

    if not awaiting_playtest:
        finish_turn()
```

### One call

```
execute_call(call):
    stage = VALIDATING
    findings = AIOSValidator.validate_planned_call(call.name, call.input)

    if findings contain an error:
        stage = REPAIRING
        push tool_result(is_error = true,
                         content = build_validation_feedback(call.name, findings))
        return                                     # NOT executed. Project untouched.

    for each warning in findings:
        log it, but do not block

    if call.name == "run_playtest":                # asynchronous: special-cased
        stage = PLAYTESTING
        playtest.start(call.input)
        awaiting_playtest   = true
        playtest_tool_use_id = call.id
        return                                     # the turn cannot finish yet

    stage = EXECUTING
    envelope = registry.call_tool(call.name, call.input)
    steps_executed += 1 if envelope.ok
    push tool_result(envelope)
```

### Finishing a turn

```
finish_turn():
    # Individually valid edits can still combine into a broken scene: a delete
    # that orphans a NodePath another step set. This is why validation runs twice.
    stage = VALIDATING
    sweep = AIOSValidator.validate_scene()

    if sweep has errors:
        repair_attempts += 1
        stage = REPAIRING

        if repair_attempts >= max_repair_attempts:
            if auto_rollback and step_snapshot:
                git.reset_to_snapshot(step_snapshot)   # hard reset; safe, see below
            abort("repair_budget_exhausted")           # always; auto_rollback only chooses cleanup
            return

        send sweep findings back as text + pending results
        return                                     # let the model fix it

    # success
    repair_attempts = 0
    if steps_executed > 0:
        last_good_snapshot = git.create_checkpoint("agent step N")

    if pending_results is empty:
        stage = IDLE; emit run_finished(ok)
    else:
        stage = PLANNING
        llm.send_tool_results(pending_results)     # -> back to on_model_response
```

### When the playtest finishes

```
on_playtest_finished(report):
    if not awaiting_playtest:  return              # a manual playtest, not ours
    awaiting_playtest = false

    failed = report.outcome in ("errors", "crashed")

    push tool_result(is_error = failed,
                     content  = build_playtest_feedback(report))

    if failed:
        repair_attempts += 1
        if repair_attempts >= max_repair_attempts:
            if auto_rollback:
                git.reset_to_snapshot(step_snapshot)
            abort("repair_budget_exhausted")       # always; never continue the loop
            return

    drain deferred_calls through execute_call()
    finish_turn()
```

---

## The repair budget

`max_repair_attempts` (default 3, in Project Settings) is what stops a model that
has misdiagnosed a problem from digging. It counts *consecutive* failures — a
successful turn resets it to zero.

When the budget is spent the pipeline does not ask the model to try again. It
resets to `step_snapshot`, aborts the run, and says so in the dock. This is
deliberate: a model that has failed three times in a row on the same problem is
usually wrong about what the problem is, and the cheapest next move is a human
glance at the findings.

---

## Two rollbacks, and why they are different

| | `reset_to_snapshot()` | `rollback_last()` |
| --- | --- | --- |
| git command | `git reset --hard <sha>` + `git clean -fd` | `git revert` |
| Used by | the pipeline, automatically | the human's **Rollback** button |
| Safe because | a snapshot committed *everything* immediately before the step | reverting is additive and destroys nothing |
| Destroys uncommitted work | yes — but there is none, by construction | no |

The hard reset is guarded. Before it runs:

```cpp
git merge-base --is-ancestor <sha> HEAD
```

If the sha is not an ancestor of `HEAD`, the reset is refused with
`not_an_ancestor`. A stray or stale sha can therefore never discard unrelated
history — the worst case is a refusal, not a loss.

The human's button never uses the hard path, because outside the pipeline the
"everything was just committed" guarantee does not hold: you might have unsaved
work the plugin knows nothing about.

---

## Intercepting the running game

This is the Observe stage, and capturing a child process's output turned out to be
the awkward part. Both obvious approaches fail:

- `OS::execute()` captures stdout but **blocks** — it would freeze the editor for
  the entire playtest.
- `OS::execute_with_pipe()` is non-blocking to start, but reading the returned
  pipe `FileAccess` blocks whenever no data is ready, which reintroduces the
  freeze at a less predictable moment.

So the plugin uses Godot's own `--log-file` flag: the child writes its complete
stdout and stderr to a file we name, and the editor tails that file from
`_process`. Non-blocking, cross-platform, no autoload required inside the game,
and it captures engine errors and GDScript stack traces that never reach stdout in
a readable form.

```cpp
// src/playtest/aios_playtest.cpp
PackedStringArray args;
args.push_back("--path");
args.push_back(project_dir);              // globalized res://
args.push_back("--log-file");
args.push_back(absolute_log);             // user://godot_ai_os/playtest.log

if (headless) {
    args.push_back("--headless");
}
if (quit_after > 0) {
    // Deterministic exit for smoke tests: the game shuts itself down after
    // N frames instead of relying on the timeout to kill it.
    args.push_back("--quit-after");
    args.push_back(String::num_int64(quit_after));
}
args.push_back(scene);

// create_process rather than execute: execute blocks until the child exits,
// which would freeze the editor for the whole playtest.
pid = (int)os->create_process(os->get_executable_path(), args, false);
```

The log is drained incrementally, from a byte offset, so re-reading is cheap:

```cpp
void AIOSPlaytest::_drain_log() {
    Ref<FileAccess> file = FileAccess::open(log_path, FileAccess::READ);
    if (file.is_null()) {
        return;                            // not created yet; try next frame
    }

    const int64_t size = file->get_length();
    if (size < read_offset) {
        // The file shrank, which means the child truncated and restarted it.
        read_offset = 0;
    }
    if (size == read_offset) {
        file->close();
        return;                            // nothing new
    }

    file->seek(read_offset);
    rx_buffer += file->get_buffer(size - read_offset).get_string_from_utf8();
    read_offset = size;
    file->close();

    PackedStringArray lines = rx_buffer.split("\n");
    // The last element is a partial line unless the buffer ended on a newline;
    // hold it back so a diagnostic is never split across two polls.
    rx_buffer = lines[lines.size() - 1];
    for (int i = 0; i < lines.size() - 1; i++) {
        _consume_line(lines[i]);
    }
}
```

### Parsing a diagnostic

Godot writes an error as a message line followed by an indented location line:

```
SCRIPT ERROR: Invalid access to property or key 'text' on a base object of type 'null instance'.
   at: _ready (res://runtime_bomb.gd:5)
```

Getting file, line and function out of that second line is the entire reason an
agent can fix the bug instead of guessing at it. So the parser holds each finding
open until it has seen — or ruled out — the continuation line:

```cpp
void AIOSPlaytest::_consume_line(const String &p_line) {
    const String line = p_line.strip_edges();
    if (line.is_empty()) {
        return;
    }

    // Location continuation for the diagnostic we are holding.
    if (has_pending && line.begins_with("at:")) {
        const String rest = line.substr(3).strip_edges();
        const int open = rest.rfind("(");
        const int close = rest.rfind(")");
        if (open >= 0 && close > open) {
            const String location = rest.substr(open + 1, close - open - 1);
            const int colon = location.rfind(":");
            if (colon >= 0) {
                pending["file"] = location.substr(0, colon);
                pending["line"] = location.substr(colon + 1).to_int();
            } else {
                pending["file"] = location;
            }
            pending["function"] = rest.substr(0, open).strip_edges();
        } else {
            pending["function"] = rest;
        }
        _flush_pending();
        return;
    }

    struct Marker { const char *prefix; const char *severity; const char *kind; };
    static const Marker markers[] = {
        { "SCRIPT ERROR:",      "error",   "script" },
        { "USER SCRIPT ERROR:", "error",   "script" },
        { "USER ERROR:",        "error",   "user"   },
        { "ERROR:",             "error",   "engine" },
        { "USER WARNING:",      "warning", "user"   },
        { "WARNING:",           "warning", "engine" },
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(Marker); i++) {
        const String prefix = markers[i].prefix;
        if (!line.begins_with(prefix)) {
            continue;
        }
        _flush_pending();

        pending = Dictionary();
        pending["severity"] = markers[i].severity;
        pending["kind"]     = markers[i].kind;
        pending["message"]  = line.substr(prefix.length()).strip_edges();
        pending["raw"]      = line;
        has_pending = true;

        if (String(markers[i].severity) == "error") { error_count++; }
        else                                        { warning_count++; }

        emit_signal("playtest_output", "stderr", line);
        return;
    }

    // Anything we were holding is complete: this line is not its location.
    _flush_pending();

    if (output_lines.size() < AIOS_MAX_OUTPUT_LINES) {
        output_lines.push_back(line);
    }
    emit_signal("playtest_output", "stdout", line);
}
```

The result, verified against Godot 4.4.1:

```json
{
  "severity": "error",
  "kind": "script",
  "message": "Invalid access to property or key 'text' on a base object of type 'null instance'.",
  "file": "res://runtime_bomb.gd",
  "line": 5,
  "function": "_ready"
}
```

### Outcomes

| Outcome | Meaning |
| --- | --- |
| `clean` | Exited on its own, nothing logged. The only outcome with `passed: true`. |
| `errors` | Ran to completion but logged errors. |
| `crashed` | Non-zero exit code. |
| `timeout` | Still running when the clock ran out, so it was killed. |

`timeout` is **not** a failure by itself — a game with no exit condition always
hits it. Pass `quit_after_frames` for a deterministic smoke test.

---

## The prompt wrapper

A model repairs what it can see. Handing back `"validation failed"` produces a
guess; handing back the specific finding, its location, and what the pipeline did
about it produces a fix. Three builders in
[`aios_pipeline.cpp`](../src/pipeline/aios_pipeline.cpp) are the entire
self-healing mechanism — everything else is plumbing that gets their text to the
model.

### 1. Validation failure

Sent as the `tool_result` content, with `is_error: true`, for a call that was
refused:

```
VALIDATION FAILED - this call was NOT executed. The project is unchanged.

Tool: attach_script_safe

Findings:
  [ERROR] parse_error: The script does not compile. The parser's message (with
          line number) is in Godot's Output panel. Nothing was written to disk.
  [ERROR] incompatible_base: The script extends RigidBody3D but the target node
          is a Label, which does not inherit from it.

Fix the errors above and call the tool again with corrected arguments. Warnings
did not block execution and only need attention if they are actually wrong. If a
finding is a false positive - for example a node reference to something you are
about to create later in this plan - say so and proceed with the corrected call
anyway.
```

Saying *"NOT executed. The project is unchanged."* first is the important part. A
model that assumes its edit half-landed will try to clean up after an edit that
never happened.

### 2. Playtest result

```
PLAYTEST RESULT: ERRORS
The scene ran but logged 1 error(s).
Scene: res://main.tscn
Ran for 1.2s. 1 error(s), 0 warning(s).

Diagnostics (in the order they were logged):
  [ERROR/script] Invalid access to property or key 'text' on a base object of
                 type 'null instance'.
      at res://runtime_bomb.gd:5 in _ready()

Last console output before the run ended:
  Godot Engine v4.4.1.stable.official

The errors above are real and reproducible. Open the file each one points at,
fix the specific cause, and run the playtest again to confirm.
```

The diagnostic list is capped at 25. A script erroring every frame produces
hundreds of identical lines, and the first few are the ones that explain it.

### 3. Rollback notice

```
ROLLBACK PERFORMED.

Reason: repair budget exhausted after 3 attempts
The project was reset to snapshot 8b203230 - every file change from your last
step has been undone. The scene tree and the filesystem are back to the state
they were in before it.

Your previous approach did not work. Do not retry it unchanged. Call
get_world_model to see the restored state, work out what actually went wrong,
and take a different approach. If you cannot see a different approach, say so
plainly instead of retrying.
```

The last sentence matters more than it looks. Without an explicit invitation to
give up, a model will keep proposing variations of a plan that cannot work.

### The system prompt

Assembled per run from the mode and whether git is available
(`build_system_prompt`). It tells the model the pipeline exists, because a model
that knows a validation gate is in front of it behaves differently — it stops
treating a rejection as a mysterious failure and starts treating it as review.

The git paragraph flips entirely when the project is not a repository:

> This project is NOT a git repository, so there is no rollback. Every change you
> make is permanent. Be correspondingly careful: prefer `dry_run` first, and
> prefer small reversible steps over large ones.

Modes (`architect`, `coder`, `debugger`, `playtester`) change only the final
paragraph. Keeping the difference small and concrete beats four divergent prompts
that drift apart.

---

## Configuration

Project Settings → **AI Agent OS**:

| Setting | Default | What it does |
| --- | --- | --- |
| `agent/max_turns` | 24 | Hard cap on model turns in one run. Stops a loop from running up a bill. |
| `agent/max_repair_attempts` | 3 | Consecutive failures before rollback and abort. |
| `agent/auto_playtest` | on | After a mutating batch that called `save_scene`, launch a 60-frame headless smoke test and feed the report back (or end the run if the model already finished). |
| `agent/auto_rollback` | on | Reset to the step snapshot when the repair budget is spent. Turn off to inspect the wreckage. |
| `agent/require_brief` | on | Interview before build in architect/coder modes. Off skips straight to planning. |
| `agent/driver_lock` | on | Built-in pipeline XOR mutating IPC — the other path gets `busy`. |
| `agent/dock_routing` | Auto | `Auto` (key → built-in, else IPC), `Built-in`, or `External` (always broadcast `user_prompt`). |
| `vcs/auto_checkpoint` | on | Commit after each mutating tool call. |

Turning `auto_rollback` off is a debugging aid: the run still aborts, but the
broken state is left on disk for you to look at. `last_good_snapshot` is printed
in the dock so you can reset by hand.

---

## What this does not do

- **No parallel tool calls.** Calls in a batch run in order, one at a time. Godot's
  editor API is single-threaded and the ordering between "create node" and "attach
  script to it" is load-bearing.
- **No partial rollback.** The unit of undo is a batch, not a call. See the batch
  snapshot note above.
- **Cross-run memory is opt-in via tools.** Each conversation still starts fresh,
  but `remember` / `recall_memory` persist notes in
  `.godot/ai_agent_os/memory.json`, and the built-in prompt includes recent
  notes at run start.
- **The pipeline cannot save your scene for you.** It tells the model to call
  `save_scene`, and a snapshot only captures what is on disk. Silently writing a
  file a human has open is worse than losing an agent's work. When
  `auto_playtest` is on, a successful `save_scene` in a batch triggers a smoke
  test so a run cannot end with zero runtime proof after a save.
