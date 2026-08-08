# Generated assets

Getting a 3D model from a text prompt into a game that runs well.

---

## The honest summary

Text-to-3D services are good enough to use and not good enough to use *raw*. A
generated mesh reliably arrives:

- at an **arbitrary scale** — 40 metres or 4 centimetres, rarely what you asked
- **Z-up** if it passed through a DCC tool, and Godot is Y-up
- at **100k–500k triangles** for a prop the player walks past
- with **no collision**, and Godot cannot infer good collision on its own
- with its **origin** wherever the generator left it, often floating in space

Each is a two-minute manual fix and a permanent tax once you are generating
dozens of assets. So this plugin splits the job in two:

```
   generate            clean up             use
   ─────────           ────────             ───
   Meshy / Tripo   →   Blender          →   place it
   (costs money)       (free, local)        (free)

   generate_3d_asset   cleanup_mesh         create_node_safe
                                            + set_node_properties
```

Blender's role here is **post-processing**, not modelling. That is deliberate,
and worth explaining because the obvious expectation is the opposite.

---

## Why not have the AI model things in Blender?

It can, and there is a tool for it (`run_blender_script`). It is just much worse
than it sounds for most of what you want.

An LLM writing `bpy` code produces:

| Good | Bad |
| --- | --- |
| Walls, floors, ramps, stairs | Characters |
| Modular kit pieces | Weapons |
| Railings, pillars, crates | Anything organic |
| Greebled panels, trims | Anything where silhouette matters |
| Parametric layouts | Anything with a face |

The dividing line is whether the shape can be described as boxes and extrusions.
Procedural code is genuinely good at that and produces garbage at everything
else, because it has no feedback loop on how the result *looks*.

So: use `generate_3d_asset` for props and `run_blender_script` for architecture.

---

## The collision trick

This is the part most worth knowing, because it is not obvious and it is the
supported path.

Godot's glTF/FBX importer generates collision **from mesh name suffixes**:

| Mesh name | What Godot builds |
| --- | --- |
| `Crate-col` | Concave trimesh `StaticBody3D` — accurate, static geometry only |
| `Crate-convcol` | Convex hull `StaticBody3D` — cheap, works for moving bodies |
| `Crate-colonly` | Collision with no visual mesh |
| `Crate-navmesh` | Navigation mesh |

So `cleanup_mesh` does not try to walk the imported scene and attach
`CollisionShape3D` nodes afterwards. It **duplicates the visual mesh inside
Blender, decimates the copy hard, strips its materials, and renames it** with the
right suffix. Godot then does the rest at import, through the mechanism the
engine actually documents.

Picking a mode:

- **`convex`** — the default, and right for most props. A convex hull is cheap
  and works on moving bodies. Wrong for anything with a hole in it: a doorway
  with a convex hull is a solid slab.
- **`trimesh`** — accurate to the geometry, but static only. Right for level
  architecture, wrong for anything that moves.
- **`colonly`** — invisible collision. Right for blockout volumes and clip
  brushes.

Collision proxies get a much harder triangle budget than the visual mesh
(`collision_triangles`, default 500), because nobody sees them and a 200-triangle
hull behaves identically to a 20,000-triangle one for anything the player feels.

---

## A worked sequence

```jsonc
// 1. Generate. This is the one call that costs money.
{ "tool": "generate_3d_asset",
  "params": { "prompt": "a rusted steel ammo crate with a hinged lid",
              "name": "ammo_crate",
              "target_triangles": 8000 } }

// -> returns immediately; the finished path arrives as an
//    'asset_ready' event roughly 30-120s later.

// 2. Clean it up. Free, local, and the step that makes it usable.
{ "tool": "cleanup_mesh",
  "params": { "path": "res://assets/generated/ammo_crate.glb",
              "target_triangles": 3000,
              "collision": "convex",
              "scale_to": 1,            // longest edge becomes 1 metre
              "center_origin": true,    // origin on the floor, so y=0 sits it down
              "merge_by_distance_micrometres": 100 } }

// 3. Place it.
{ "tool": "create_node_safe",
  "params": { "parent": "Level", "instance_scene": "res://assets/generated/ammo_crate.glb",
              "name": "AmmoCrate" } }

{ "tool": "set_node_properties",
  "params": { "node": "Level/AmmoCrate", "properties": { "position": [4, 0, -2] } } }

// 4. Look at it. Text cannot tell you it is inside a wall.
{ "tool": "capture_viewport_screenshot", "params": { "viewport": "3d" } }
```

`scale_to` and `center_origin` are the two that save the most grief.
Normalising scale turns "why is my rifle the size of a bus" into a non-event,
and dropping the origin to the footprint means `position.y = 0` puts an object
on the ground instead of half-buried.

---

## Reducing polygons on a model you already have

You do not need to have generated it. `cleanup_mesh` works on anything in the
project:

```jsonc
{ "tool": "cleanup_mesh",
  "params": { "path": "res://assets/downloaded/statue.glb",
              "output": "res://assets/downloaded/statue_lowpoly.glb",
              "target_triangles": 2000 } }
```

Pass `output` to keep the original. Decimation is lossy and irreversible, so
writing to a new file is usually what you want the first time.

The decimation uses Blender's **collapse** mode rather than un-subdivide or
planar, because collapse is the only one that hits a *specific* triangle count —
which is what a performance budget actually needs. The ratio is computed from the
real triangle count rather than guessed, and a mesh already under budget is left
alone rather than being pointlessly re-processed.

The report tells you what happened:

```json
{ "triangles_before": 187432, "triangles_after": 2011,
  "reduction_percent": 98, "collision_generated": true }
```

---

## What it costs

`generate_3d_asset` is the **only** tool here that spends money. Everything else
— cleanup, import, placement, screenshots — is local and free.

Because a generation call *succeeds* every time, nothing in the normal pipeline
would stop an agent from calling it repeatedly: there is no error to back off
from. So there is a hard per-session ceiling:

**Project Settings → AI Agent OS → assets**

| Setting | Default | Meaning |
| --- | --- | --- |
| `max_paid_calls` | 10 | Paid calls allowed per editor session. `-1` disables the limit. |
| `target_directory` | `res://assets/generated` | Where generated assets land. |
| `default_provider` | Meshy | Which service to use when the call doesn't say. |

Every billed call also writes a warning line into the dock naming the tool and
the running count. That is deliberate noise — the failure mode worth preventing
is discovering the cost afterwards.

Two habits that save money more than the ceiling does:

1. **Ask for the triangle budget at generation time** (`target_triangles`) rather
   than decimating afterwards. The provider's remesher knows the topology and
   preserves the silhouette far better than a blind decimate.
2. **Look at what you got** with `capture_viewport_screenshot` before deciding it
   is wrong. Re-rolling a prompt is the single easiest way to spend money on
   nothing.

---

## API keys

Asset keys go in the same encrypted store as the model keys — `user://`, never
`project.godot`. See [SETUP.md §5](SETUP.md#5-configure-the-built-in-agent) for
the full explanation; the short version is that environment variables are checked
first and never written anywhere:

| Provider | Environment variable |
| --- | --- |
| Meshy | `MESHY_API_KEY` |
| Tripo3D | `TRIPO_API_KEY` |
| ElevenLabs | `ELEVENLABS_API_KEY` |
| OpenAI | `OPENAI_API_KEY` |

ElevenLabs and OpenAI are in the credential store but have no generation tool
yet — audio and texture generation are Milestone 4. `import_asset_from_url`
already works with them if you call the API yourself.

---

## Installing Blender

`cleanup_mesh` and `run_blender_script` need Blender 4.x. Everything else works
without it.

The plugin looks in this order:

1. `GODOT_AI_OS_BLENDER`, if set to a full path to the executable
2. `blender` on `PATH`
3. Godot's own **Project Settings → Filesystem → Import → Blender → Blender Path**
4. Platform defaults:
   - Windows: `%ProgramFiles%\Blender Foundation\Blender <version>\blender.exe`
   - macOS: `/Applications/Blender.app/Contents/MacOS/Blender`
   - Linux: `/usr/bin`, `/usr/local/bin`, snap, flatpak

If you have already told Godot where Blender is for `.blend` import, that setting
is reused — no second configuration.

Blender runs with `--background --factory-startup`, so no user add-ons load and
nothing in your Blender configuration can change the result. Output goes to a
temp file in `user://` and is only moved into the project on success, so a crash
mid-decimate cannot leave a truncated mesh where your asset used to be.

> **Not verified end to end.** Blender is not installed in the environment this
> plugin was developed in. The pipeline script is written against the Blender 4.x
> Python API, syntax-checked, and its argument handling is tested — but it has
> not been run. If you try it,
> [a report either way is genuinely useful](https://github.com/iwolski99/GodotAI/issues).

---

## Provider API shapes

The request and response shapes for Meshy and Tripo3D live in one table at the
top of [`src/assets/aios_asset_pipeline.cpp`](../src/assets/aios_asset_pipeline.cpp).

They were written from each provider's published documentation and have **not**
been executed against the live services — no outbound access to them and no keys
in the development environment. If a generation call fails with
`unexpected_response` or a parse error rather than a network error, that table is
the first thing to check, and fixing it is a five-line edit by design.

`import_asset_from_url` exists partly for this reason: it bypasses the provider
integration entirely. Call whatever API you like from your own harness and hand
the resulting URL to the plugin.
