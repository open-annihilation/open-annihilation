#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Make the live renderer's shaders from their GLSL sources, or check them.

The live renderer (src/app/renderer_live_gpu.cpp) draws through SDL's GPU
interface, which takes a shader in the language of the graphics interface it
runs on. Each stage is written once, in Vulkan's GLSL, in src/app/shaders/:

  live_scene_vertex.glsl      the scene's vertices
  live_scene_fragment.glsl    the scene's terrain, sprites, models and water
  live_resolve_vertex.glsl    the supersampling resolve's triangle, which
                              the lights pass draws too
  live_resolve_fragment.glsl  the resolve's mean of each block
  live_surface_fragment.glsl  the surface the lights reach under each
                              pixel, drawn after the scene's vertex stage
  live_lights_fragment.glsl   the lights' light, glow and heat over the
                              picture
  live_overlay_fragment.glsl  what the processor painted over the turned
                              view added to the picture, and the display
                              gamma
  live_model_vertex.glsl      the models the card keeps: a unit's triangle
                              corner from its type's, for the scene's and
                              the surface's fragment stages

and this makes, for each, the forms the renderer embeds, in
src/app/renderer_live_shaders.inc:

  SPIR-V    glslangValidator compiles the source for Vulkan 1.0, its
            functions inlined (-Os) so that no form names the copies of
            their arguments, and spirv-val validates it
  MSL       spirv-cross writes the SPIR-V as Metal shading language 2.1,
            each resource at its binding's index, as SDL binds them

Resources keep SDL's layout in every form: a vertex stage's uniform blocks
in set 1, a fragment stage's samplers in set 2 and uniform blocks in set 3,
each at its slot as binding. The file holds the tools' versions; it is
committed, so building the game needs none of them. Run this again when a
source changes.

--check makes the file afresh in memory and compares it with the one
committed. The live-shaders test runs it.

Exit status: 0 when the file was made, or --check found it as made; 1 when
--check finds it differs or a tool fails; 77 when --check finds a tool
missing or of another version than the file's, which ctest reports as
skipped.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "src" / "app" / "shaders"
OUTPUT = ROOT / "src" / "app" / "renderer_live_shaders.inc"
# Each stage: the source's name, its stage for glslangValidator, and the
# name of the arrays the file gives it.
STAGES = (
    ("live_scene_vertex", "vert"),
    ("live_scene_fragment", "frag"),
    ("live_resolve_vertex", "vert"),
    ("live_resolve_fragment", "frag"),
    ("live_surface_fragment", "frag"),
    ("live_lights_fragment", "frag"),
    ("live_overlay_fragment", "frag"),
    ("live_model_vertex", "vert"),
)
TOOLS = ("glslangValidator", "spirv-val", "spirv-cross")
# The Metal shading language version spirv-cross writes, as it takes it:
# 2.1, which every Mac the game runs on has.
MSL_VERSION = "20100"
# Bytes of SPIR-V on each line of the file.
BYTES_PER_LINE = 16
SKIPPED = 77


def tool_versions():
    """Reads the versions of the tools.

    @return the text the file records them by, one line a tool
    """
    lines = []
    glslang = subprocess.run(["glslangValidator", "--version"], capture_output=True, text=True,
                             check=True).stdout
    lines.append(next(line for line in glslang.splitlines() if line.startswith("Glslang Version:")))
    tools = subprocess.run(["spirv-val", "--version"], capture_output=True, text=True, check=True)
    lines.append(tools.stdout.splitlines()[0])
    cross = subprocess.run(["spirv-cross", "--revision"], capture_output=True, text=True, check=True)
    lines.append("spirv-cross " + " ".join((cross.stdout + cross.stderr).split()))
    return lines


def compile_stage(name, stage, scratch):
    """Compiles one stage into its two forms.

    @param name the source's name, without .glsl
    @param stage its stage for glslangValidator
    @param scratch a directory for the tools' files
    @return (the SPIR-V's bytes, the MSL's text)
    """
    source = SHADERS / f"{name}.glsl"
    spirv = scratch / f"{name}.spv"
    msl = scratch / f"{name}.metal"
    for command in (
        ["glslangValidator", "-V", "-Os", "--target-env", "vulkan1.0", "-S", stage, "-o",
         str(spirv), str(source)],
        ["spirv-val", "--target-env", "vulkan1.0", str(spirv)],
        ["spirv-cross", str(spirv), "--msl", "--msl-version", MSL_VERSION,
         "--msl-decoration-binding", "--output", str(msl)],
    ):
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"{' '.join(command)}:\n{result.stdout}{result.stderr}")
    return spirv.read_bytes(), msl.read_text()


def file_text(versions, forms):
    """Writes the file the renderer includes.

    @param versions the tools' versions, one line each
    @param forms each stage's name, SPIR-V and MSL, in STAGES' order
    @return the file's text
    """
    out = [
        "// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT",
        "// SPDX-License-Identifier: GPL-3.0-only",
        "",
        "// The live renderer's shaders, made by tools/make_live_shaders.py from",
        "// src/app/shaders/*.glsl; make them again rather than editing them here.",
        "// renderer_live_gpu.cpp includes this in its own namespace. Made with:",
    ]
    out += [f"//   {line}" for line in versions]
    for name, spirv, msl in forms:
        if ")msl\"" in msl:
            raise RuntimeError(f"{name}: the MSL holds the raw string's end")
        out.append("")
        out.append(f"/// {name}.glsl as SPIR-V for Vulkan 1.0.")
        out.append(f"alignas(4) constexpr unsigned char {name}_spirv[] = {{")
        for start in range(0, len(spirv), BYTES_PER_LINE):
            chunk = spirv[start:start + BYTES_PER_LINE]
            out.append("    " + " ".join(f"0x{byte:02x}," for byte in chunk))
        out.append("};")
        out.append(f"/// {name}.glsl as Metal shading language; its entry point is main0.")
        out.append(f"constexpr char {name}_msl[] = R\"msl(")
        out += msl.rstrip("\n").split("\n")
        out.append(")msl\";")
    return "\n".join(out) + "\n"


def make():
    """Makes the file's text from the sources.

    @return the text
    """
    with tempfile.TemporaryDirectory() as scratch:
        forms = [(name, *compile_stage(name, stage, Path(scratch))) for name, stage in STAGES]
    return file_text(tool_versions(), forms)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true",
                        help="compare the committed file with the sources")
    arguments = parser.parse_args()
    missing = [tool for tool in TOOLS if shutil.which(tool) is None]
    if missing:
        print(f"make_live_shaders: {', '.join(missing)} not found", file=sys.stderr)
        return SKIPPED if arguments.check else 1
    try:
        if arguments.check:
            committed = OUTPUT.read_text()
            header = committed.split("\n\n", 2)[1].splitlines()
            recorded = [line[len("//   "):] for line in header if line.startswith("//   ")]
            if recorded != tool_versions():
                print("make_live_shaders: skipped: the tools are not the versions the file was "
                      "made with", file=sys.stderr)
                return SKIPPED
        text = make()
    except (RuntimeError, subprocess.CalledProcessError, StopIteration) as error:
        print(f"make_live_shaders: {error}", file=sys.stderr)
        return 1
    if arguments.check:
        if text != committed:
            print(f"make_live_shaders: {OUTPUT.relative_to(ROOT)} is not what the sources make; "
                  "run tools/make_live_shaders.py", file=sys.stderr)
            return 1
        print("make_live_shaders: the shaders are as their sources make them")
        return 0
    OUTPUT.write_text(text)
    print(f"make_live_shaders: wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
