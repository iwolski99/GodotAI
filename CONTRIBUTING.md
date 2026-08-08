# Contributing

This is an open project because the problem is bigger than one person's use case.
Bug reports, build reports from platforms I can't test, and arguments about the
roadmap are all genuinely useful.

---

## Getting set up

```bash
git clone --recurse-submodules https://github.com/iwolski99/GodotAI.git
cd GodotAI
pip install scons
scons platform=<linux|macos|windows> target=editor -j8
```

The first build compiles `godot-cpp` too and takes a while. After that, a change
to one `.cpp` rebuilds in seconds.

While iterating on C++, a syntax-only check is much faster than a full link:

```bash
g++ -std=c++17 -fsyntax-only \
    -Igodot-cpp/include -Igodot-cpp/gen/include -Igodot-cpp/gdextension -Isrc \
    src/**/*.cpp
```

---

## Testing a change

There is no automated test suite yet (it's on the [roadmap](docs/ROADMAP.md) for
Milestone 3). Until then, the way to verify a change is to drive the real editor:

```bash
# Terminal 1 — a headless editor is enough for everything except the dock UI
godot --headless --editor --path project/

# Terminal 2
python3 clients/python/godot_ai_client.py ./project ping
python3 clients/python/godot_ai_client.py ./project get_world_model '{"max_depth": 2}'
```

If you touched a tool, exercise its failure paths too — the error messages are
part of the interface, and a tool that fails uselessly is a bug. Test the
`dry_run` path as well; agents rely on it.

If you touched the dock, you need a real editor window. Headless will not show
you a layout bug.

---

## What a good pull request looks like

**Scope it.** One change per PR. A new tool and a transport refactor in the same
diff is two reviews pretending to be one.

**A new tool needs three things**, or it isn't finished:

1. The implementation in `src/tools/`
2. A row in `TOOL_TABLE` in `src/tools/aios_tool_registry.cpp`
3. A schema in `project/addons/godot_ai_os/schemas/<name>.json`

The schema is not documentation — it is shipped to every agent on connect and is
how models learn to call your tool. Write the `description` for a model that has
never seen Godot: say what the tool does, what it refuses to do, and what the
common mistake is. Fill in the `errors` block with every code the tool can
return.

**Mutating tools must** validate everything before changing anything, support
`dry_run`, and return the `AIOSJson::ok()` / `AIOSJson::error()` envelope rather
than pushing errors into the editor log.

**Say what you tested.** "Built on macOS 14, ran the four core tools against
4.4.1" tells a reviewer more than a paragraph of description.

---

## Code style

Follow the file you're editing. Broadly, this codebase uses Godot's own
conventions:

- Tabs for indentation, `snake_case` methods, `p_` prefix on parameters,
  `r_` prefix on out-parameters
- Classes are `AIOS`-prefixed and live in the global namespace with
  `using namespace godot;`
- Comments explain *why*, not *what*. If a line needs a comment to say what it
  does, rename something instead. The comments worth writing are the ones that
  stop the next person from "simplifying" a decision that was deliberate.

Python follows PEP 8; the reference client stays dependency-free, and that is a
hard constraint rather than a preference.

---

## Reporting a bug

Include:

- Godot version (`godot --version`) and OS
- What you called and what came back — the full error envelope, not a summary
- The relevant part of the Output panel

For anything involving an agent misbehaving, the dock's history is usually the
fastest way to show what happened.

---

## Ideas that would help most right now

- macOS and Windows build verification (Milestone 1 was tested on Linux only)
- Asset pipeline tools / plan review / diff preview (remaining Milestone 3)
- macOS and Windows build verification
- The MCP server now ships in [`clients/mcp/`](clients/mcp/README.md) — feedback
  from real Cursor setups is especially useful
- Concrete opinions on Milestone 2's scope, from someone who has tried to run an
  agent against a real project

---

## Licence

Contributions are accepted under the MIT licence, the same as the rest of the
project.
