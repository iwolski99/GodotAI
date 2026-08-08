# Review: agent loops, strengths, and gaps

Findings from reviewing the Milestone 2 pipeline against external agentic
harnesses (Claude Code, Cursor, custom IPC clients), plus the clarify-before-build
work on branch `cursor/clarify-before-build-eb57`.

All of this work lives in the isolated git worktree
`/home/ubuntu/worktrees/clarify-before-build`. The base branch is untouched.

---

## Verdict

The built-in Milestone 2 loop and an external agentic model are **parallel
control planes over shared tools**. They integrate cleanly when only one drives
the project. They conflict when both are active at once.

A few bugs could stop an agent from finishing a goal. The quick ones are fixed
on this branch; larger design changes are proposed below for review before
implementation.

---

## How the two paths relate

```
External agent (Claude Code / Cursor / custom harness)
    └─ JSON over IPC ──► AIOSPlugin::_handle_request
                              └─ AIOSToolRegistry ──► tools
                         (no Plan / Validate / Repair orchestration)

Dock + API key (built-in agent)
    └─ AIOSPipeline (Clarify → Plan → Validate → Execute → Observe → Repair)
              └─ same AIOSToolRegistry + private AIOSLlmClient feedback loop
```

Docs correctly say both paths share the tool registry and manifest. That is
true for **tool execution**. It is not true for **orchestration**: only the
built-in client runs inside the Milestone 2 state machine.

| Scenario | Clean? |
| --- | --- |
| Built-in only | Yes — designed path |
| External only (no API key configured) | Yes — dock emits `user_prompt` |
| External tools while built-in is idle (key set) | Mostly — tools work; dock will not feed the harness |
| Both actively driving | **No** — shared playtest / git / scene, no ownership lock |

They do **not** nest. An external model’s own feedback loop is not wrapped by
Validate → Observe → Repair. The plugin is a typed tool bus for that path; the
pipeline is a second, in-process orchestrator.

---

## Strengths

- **Single tool funnel.** Built-in and IPC both go through `AIOSToolRegistry`
  with the same schemas, checkpoint policy, and world-model invalidation.
- **Validate-before-execute** for the built-in loop. Rejected calls never
  mutate; feedback builders hand actionable text back to the model.
- **Transactional git design.** Snapshot before a mutating batch; ancestor-
  guarded `git reset --hard` on repair-budget exhaustion.
- **Non-blocking playtest.** Child process + `--log-file` tailing; structured
  diagnostics with file / line / function.
- **Clarify-before-build (this branch).** Architect/coder modes interview via
  `ask_user`, lock a 2D/3D brief with `commit_brief`, withhold mutating tools
  until then, and offer **Skip & Build** as an escape hatch.
- **Stop is non-cooperative.** Cancels the LLM request, kills playtest, and
  broadcasts `stop` — the human override does not depend on the agent agreeing.
- **Honest async protocol.** `run_playtest` ack + `playtest_finished` event;
  `ask_user` returns `{queued: true}` over IPC for harnesses to present.

---

## Weaknesses and risks

| Severity | Issue | Status |
| --- | --- | --- |
| High | Playtest repair budget reset the counter and continued the model loop instead of aborting | **Fixed** on this branch |
| High | Scene repair budget with `auto_rollback` off could also loop forever | **Fixed** on this branch |
| High | No mutual exclusion between built-in pipeline and IPC agents | Proposed (major) |
| High | Dock prompts go only to the built-in agent when an API key exists | Proposed (major) |
| High | Native crash with no script errors can be reported as playtest `clean` (no cross-platform exit-code API) | Proposed |
| Medium | Stop during playtest could restart the model via synchronous `playtest_finished` | **Fixed** on this branch |
| Medium | Skip & Build mapped any goal containing `fps` to 3D | **Fixed** on this branch |
| Medium | `auto_playtest` setting is wired on the pipeline but never read | Proposed |
| Medium | Clarify / `commit_brief` gates are pipeline-local; IPC can still mutate | Proposed (major) |
| Medium | Unsaved scene edits are invisible to playtest and git snapshots (documented, still a common failure mode) | Known / docs |
| Low | `user_questions_requested` / `brief_committed` signals unused by the plugin (dock already works via stage + chat) | Optional cleanup |
| Low | PROTOCOL status enum previously omitted `CLARIFYING` | **Fixed** on this branch |

---

## Concrete conflict evidence

### 1. Dock routing is exclusive when a key is configured

In `AIOSPlugin::_on_prompt_submitted`, a configured built-in client always
starts (or continues) the pipeline and returns. IPC clients never receive
`user_prompt` in that configuration. External harnesses that listen for dock
prompts — as documented in `docs/AGENTS.md` — are starved.

### 2. IPC bypasses the Milestone 2 loop

`_handle_request` calls `registry->call_tool` directly. There is no
`validate_planned_call`, no post-batch `validate_scene`, no repair budget, and
no step snapshot/rollback on that path. External agents must reimplement that
loop themselves (the docs nudge this, but it is easy to skip).

### 3. Shared singleton playtest

Both paths use one `AIOSPlaytest`. A second `start()` returns `already_running`.
Only one Observe session can exist; concurrent agents block each other.

### 4. Clarify gates are not global

Tool allowlisting and the soft refuse of mutating calls live in
`AIOSPipeline::_execute_call`. Registry `commit_brief` validates and returns a
dict; it does not lock tools for IPC. Clarification is real for the built-in
agent and optional etiquette for everyone else.

### 5. Git / rollback races

The pipeline snapshots then hard-resets. IPC mutations that land between those
points are included in “everything on disk” and get wiped. The human Rollback
button uses `git revert`; the pipeline uses `reset --hard` — different
semantics if both an agent and a human act.

---

## Bugs that could block goal completion

### Fixed on this branch

1. **Playtest repair budget did not abort.** After N failed playtests the code
   rolled back (sometimes), reset `repair_attempts` to 0, and called
   `_finish_turn()`, so the model kept going. Now budget exhaustion always
   aborts; `auto_rollback` only chooses whether to reset the tree first.
2. **Scene budget with rollback off could loop.** Same rule applied: spent
   budget always aborts, leaving wreckage on disk when rollback is disabled.
3. **Stop / playtest race.** `AIOSPlaytest::stop()` emits `playtest_finished`
   synchronously. Clearing `awaiting_playtest` *after* that call let the
   handler restart a model turn. Ownership is now dropped first.
4. **Skip & Build `fps` → 3D.** Bare `fps` no longer forces 3D when the goal
   also asks for 2D / top-down / side-scroll / platformer.

### Still open

5. **Crash-as-clean.** Process death without logged `SCRIPT ERROR` / `ERROR`
   lines becomes `clean` because Godot’s `OS` API has no portable exit status
   here. A segfault can look like success.
6. **`auto_playtest` dead.** The setting is stored and exposed but never
   consulted; Observe only happens if the model chooses `run_playtest`.
7. **Unsaved scenes.** Playtest and git see disk only. Forgetting `save_scene`
   means the agent playtests or rolls back the wrong state.

---

## Quick fixes implemented

Branch: `cursor/clarify-before-build-eb57`  
PR: https://github.com/iwolski99/GodotAI/pull/1  
Worktree: `/home/ubuntu/worktrees/clarify-before-build`

| Fix | Where |
| --- | --- |
| Repair budget always aborts (scene + playtest paths) | `src/pipeline/aios_pipeline.cpp` |
| Stop clears `awaiting_playtest` before killing the child | `src/pipeline/aios_pipeline.cpp` |
| Skip & Build 2D-aware dimension heuristic | `src/pipeline/aios_pipeline.cpp` |
| Document `CLARIFYING` in PROTOCOL status enum | `docs/PROTOCOL.md` |
| Align PIPELINE.md pseudocode with abort-always behaviour | `docs/PIPELINE.md` |

Also on this branch (prior commit): clarify-before-build interview
(`ask_user`, `commit_brief`, dock UI, system prompt, docs).

Verification: `scons platform=linux target=editor` succeeded after the fixes.

---

## Proposed solutions (not implemented — need approval)

### Major (design decisions)

1. **Driver lock.** Exclusive “driver” session: built-in XOR IPC owns the
   project for a run. The other path queues or gets a clear `busy` error.
   Single playtest owner.
2. **Optional server-side pipeline mode for IPC.** Validate → execute → scene
   sweep → playtest await as structured events, so external harnesses do not
   have to reimplement Milestone 2 poorly to get the same safety.
3. **Dock routing policy.** Project setting: `Built-in` / `External` / `Ask`.
   Do not silently swallow `user_prompt` when a key exists if the human wants
   an external harness.
4. **Global brief lock.** Enforce “no mutating tools until `commit_brief`”
   at the registry (or a session flag), not only by filtering the built-in
   LLM tool list — if clarify should apply to all agents.

### Medium (straightforward once approved)

5. **Wire `auto_playtest`.** After a mutating batch that called `save_scene`,
   optionally launch a short `quit_after_frames` smoke test and feed the
   report back — so goals cannot “finish” with zero runtime proof when the
   setting is on.
6. **Stronger crash heuristic.** Treat suspiciously silent / immediate process
   death as non-pass (`crashed` or `unknown`) even without exit codes; keep
   intentional `quit_after_frames` exits as `clean` when error-free.
7. **Python client helper.** `wait_playtest(timeout)` that listens for
   `playtest_finished`, so reference harnesses complete the Observe half of
   the loop.

### Small / cleanup

8. Connect or remove unused `user_questions_requested` / `brief_committed`
   dock wiring (UI already works via stage + agent messages).
9. Align `ask_user` schema `returns` docs with the IPC `{queued: true}` shape.

---

## Recommendation

- **Ship / review the current branch** for clarify-before-build plus the quick
  reliability fixes (repair abort, Stop race, Skip & Build heuristic).
- **Decide next** on driver lock and dock routing — those determine whether
  external agents and the built-in pipeline are meant to coexist or alternate.
- Treat server-side pipeline-for-IPC and global brief lock as follow-ups once
  that ownership model is chosen.

---

## Revert

This work is isolated:

```bash
# Worktree
git worktree list
# Branch / PR
#   cursor/clarify-before-build-eb57
#   https://github.com/iwolski99/GodotAI/pull/1
```

Closing or deleting the PR/branch leaves the base branch unchanged.
