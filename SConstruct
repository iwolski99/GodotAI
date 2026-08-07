#!/usr/bin/env python
"""
Build script for the Godot AI Agent OS GDExtension.

Typical use:

    scons target=editor                 # debug editor build for the host platform
    scons target=editor -j8             # ...in parallel
    scons target=template_release       # optimised build (still editor-only code)
    scons -c                            # clean

The resulting shared library is written straight into the addon folder so the
demo project in `project/` picks it up without a copy step.
"""

import os

env = SConscript("godot-cpp/SConstruct")

# The plugin is editor-only: every class it registers talks to EditorInterface.
# We still build the "template" targets so users can ship a release-optimised
# editor plugin, but there is no runtime component to link into a game export.
env.Append(CPPPATH=["src/"])

sources = Glob("src/*.cpp")
sources += Glob("src/*/*.cpp")

addon_bin = "project/addons/godot_ai_os/bin/"

if env["platform"] == "macos":
    library = env.SharedLibrary(
        "{}libgodot_ai_os.{}.{}.framework/libgodot_ai_os.{}.{}".format(
            addon_bin,
            env["platform"],
            env["target"],
            env["platform"],
            env["target"],
        ),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "{}libgodot_ai_os{}{}".format(addon_bin, env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )

Default(library)
