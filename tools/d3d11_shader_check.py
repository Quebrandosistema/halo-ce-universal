"""Check the Direct3D 11 renderer's dumped shaders on this PC.

The proof of port/windows/D3D11.md's phase 4: every shader the HLSL generators
(port/windows/d3d11/nv2a_vsh_hlsl.c, nv2a_psh_hlsl.c) make for the game's real
shaders must compile with d3dcompiler_47.dll and say exactly what the OpenGL
renderer's GLSL says for the same shader. The shaders are the folder
halo_d3d11.exe writes when debug.gpu_dump_shaders names one (HALO_GPU_DUMP_SHADERS):
each shader's HLSL (.hlsl), its GLSL twin from the unchanged generators (.glsl,
the OpenGL executable's text for the same shader), each pixel shader's key
(.key) and manifest.txt, one line a file.

    python tools/d3d11_shader_check.py <dump folder> [<OpenGL dump folder>]
    python tools/d3d11_shader_check.py --prove <dump folder>
    python tools/d3d11_shader_check.py --copies

For each .hlsl of the manifest:

- compiled with D3DCompile (d3dcompiler_47.dll through ctypes) at the
  manifest's entry and target, D3DCOMPILE_OPTIMIZATION_LEVEL3; a failure is
  reported with the compiler's text; the bytecode sizes and the times are
  summed;
- compared with its GLSL twin: the HLSL must be exactly the twin with the
  dialect's changes (port/windows/D3D11.md, phase 4, "The dialect", items 1 to
  10, and phase 6's item 11: a manifest line marked constant_alpha=1 or 2 has
  the one multiply by the blend constant's alpha before the return) applied,
  the declarations written from hlsl_shaders.h's #defines and strings,
  whitespace aside. A shader that compiles and draws black is the
  failure this is for: it compiles, so only a comparison finds it.

Then the keys: the distinct keys and how many values each field takes (a field
that never varies across many keys may be a key that was not filled in). With
the OpenGL executable's dump folder (debug.gpu_dump_shaders under halo.exe:
vs<id>_<variant>[_lit].glsl and ps_<hash>.glsl), the twins and its files are
matched by content and the unmatched listed.

--prove changes one line of the first pixel shader (in memory) and checks that
the comparison fails on it, with the diff. --copies diffs the two generator
copies against port/linux/src/nv2a_vsh.c and nv2a_psh.c and classes every
differing line under one of the dialect's items; a line under none is a
finding.

Exits 1 on any failure.
"""

import ctypes
import difflib
import hashlib
import re
import struct
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "port" / "windows" / "d3d11" / "hlsl_shaders.h"
D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15

# ---------- hlsl_shaders.h


def header_macros():
    """The header's #defines: {name: body}, continuation lines joined."""
    macros = {}
    lines = HEADER.read_text().splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("#define "):
            body = line
            while body.endswith("\\"):
                index += 1
                body = body[:-1] + " " + lines[index].strip()
            match = re.match(r"#define\s+(\w+)(\([^)]*\))?\s*(.*)$", body)
            if match and not match.group(2):
                macros[match.group(1)] = match.group(3).strip()
        index += 1
    return macros


def unescape(literal):
    return literal.encode().decode("unicode_escape")


def macro_text(macros, name):
    """A string macro's text: its string literals, the macros it names and HLSL_REGISTER_TEXT(X) (X's value)."""
    body = macros[name]
    text = ""
    for token in re.finditer(r'"((?:[^"\\]|\\.)*)"|HLSL_REGISTER_TEXT\((\w+)\)|(\w+)', body):
        if token.group(1) is not None:
            text += unescape(token.group(1))
        elif token.group(2):
            text += macros[token.group(2)]
        elif token.group(3):
            text += macro_text(macros, token.group(3))
    return text


# ---------- the dialect: the GLSL twin made HLSL


def split_arguments(text):
    """The top-level comma-separated arguments of a call's inside."""
    arguments, depth, start = [], 0, 0
    for index, character in enumerate(text):
        if character in "([":
            depth += 1
        elif character in ")]":
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append(text[start:index])
            start = index + 1
    arguments.append(text[start:])
    return arguments


def closing(text, open_at):
    """The index of the parenthesis that closes the one at open_at."""
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unbalanced parentheses")


def rewrite_calls(text, pattern, rewrite):
    """Every call whose name matches pattern (a regex ending before the parenthesis), innermost arguments first,
    rewritten by rewrite(match, arguments_text)."""
    regex = re.compile(pattern + r"\(")
    position = 0
    while True:
        match = regex.search(text, position)
        if not match:
            return text
        open_at = match.end() - 1
        close_at = closing(text, open_at)
        inside = rewrite_calls(text[open_at + 1:close_at], pattern, rewrite)
        replacement = rewrite(match, inside)
        text = text[:match.start()] + replacement + text[close_at + 1:]
        position = match.start() + len(replacement)


VECTOR_TYPES = {"vec": "float", "ivec": "int", "uvec": "uint"}


def grammar(text):
    """Items 2 and 3: functions, constructors, casts and type names."""
    # item 3: the comparisons (componentwise in HLSL), then the renamed functions
    text = rewrite_calls(text, r"\blessThan", lambda m, a: "(%s < %s)" % tuple(x.strip() for x in split_arguments(a)))
    text = rewrite_calls(text, r"\bgreaterThanEqual",
                         lambda m, a: "(%s >= %s)" % tuple(x.strip() for x in split_arguments(a)))
    text = re.sub(r"\bmix\(", "lerp(", text)
    text = re.sub(r"\bfract\(", "frac(", text)
    text = re.sub(r"\binversesqrt\(", "rsqrt(", text)
    text = re.sub(r"\bivec2 size = textureSize\((tex\d), 0\);", r"int2 size; \1.GetDimensions(size.x, size.y);", text)
    text = rewrite_calls(text, r"\btexelFetch", lambda m, a: "%s.Load(int3(%s, %s))" % tuple(
        x.strip() for x in split_arguments(a)))
    text = rewrite_calls(text, r"\btexture", lambda m, a: "%s.Sample(%s_sampler, %s)" % (
        split_arguments(a)[0].strip(), split_arguments(a)[0].strip(), ",".join(split_arguments(a)[1:]).strip()))

    # item 2: a one-argument vector constructor is a cast (which broadcasts), the others HLSL's; float() and int()
    def constructor(match, inside):
        kind, size = match.group(1), match.group(2)
        name = VECTOR_TYPES[kind] + size
        if len(split_arguments(inside)) == 1:
            return "((%s)(%s))" % (name, inside)
        return "%s(%s)" % (name, inside)

    text = rewrite_calls(text, r"\b(vec|ivec|uvec)([234])", constructor)
    text = rewrite_calls(text, r"\b(float|int|uint)", lambda m, a: "(%s)(%s)" % (m.group(1), a))
    text = re.sub(r"\b(vec|ivec|uvec)([234])\b", lambda m: VECTOR_TYPES[m.group(1)] + m.group(2), text)
    return text


def expected_vertex(gl_text, entry, macros):
    """The HLSL a vertex shader's GLSL twin says, with its manifest line's masks."""
    packed = int(entry["packed"], 16)
    integer = int(entry["integer"], 16)
    three = int(entry["three"], 16)
    lines = gl_text.splitlines()
    try:
        start = [line.strip() for line in lines].index("invariant gl_Position;")
    except ValueError:
        return None, "the GLSL has no `invariant gl_Position;`"
    lit_normal = "out vec4 xWorldNormal;" in gl_text
    lit_position = "out vec3 xWorldPosition;" in gl_text
    # item 1 and 6: the prologue (the version, the loose uniforms, the outs, invariant) is the header's
    head = macro_text(macros, "HLSL_VERTEX_CONSTANTS_TEXT") + macro_text(macros, "HLSL_INTERFACE_BEGIN")
    if lit_normal:
        head += macro_text(macros, "HLSL_INTERFACE_NORMAL_MEMBER")
    if lit_position:
        head += macro_text(macros, "HLSL_INTERFACE_POSITION_MEMBER")
    head += macro_text(macros, "HLSL_INTERFACE_END") + macro_text(macros, "HLSL_INTERFACE_STATICS")
    body = []
    inputs_seen = 0
    copies_seen = 0
    main_open = False
    for line in lines[start + 1:]:
        stripped = line.strip()
        # item 6: the lit outs are statics
        match = re.match(r"out (vec[34]) (xWorld\w+);$", stripped)
        if match:
            body.append("static %s %s;" % (grammar(match.group(1)), match.group(2)))
            continue
        # item 7: the inputs, a structure
        match = re.match(r"layout\(location = (\d+)\) in (uint|vec4) v(\d+)_(packed|in);$", stripped)
        if match:
            index = int(match.group(3))
            if inputs_seen == 0:
                body.append("struct vs_in")
                body.append("{")
            if match.group(4) == "packed":
                body.append("uint v%d_packed : TEXCOORD%d;" % (index, index))
            elif integer & (1 << index):
                body.append("int4 v%d_int : TEXCOORD%d;" % (index, index))
            else:
                body.append("float4 v%d_in : TEXCOORD%d;" % (index, index))
            inputs_seen += 1
            if inputs_seen == 16:
                body.append("};")
            continue
        if stripped == "void main()":
            body.append("vs_out main(vs_in input)")
            main_open = True
            continue
        match = re.match(r"vec4 v(\d+) = (unpack_normpacked3\(v\d+_packed\)|v\d+_in);$", stripped)
        if match:
            index = int(match.group(1))
            if "unpack" in match.group(2):
                body.append("float4 v%d = unpack_normpacked3(input.v%d_packed);" % (index, index))
            elif integer & (1 << index):
                body.append("float4 v%d = ((float4)(input.v%d_int));" % (index, index))
            else:
                body.append("float4 v%d = input.v%d_in;" % (index, index))
            copies_seen += 1
            if copies_seen == 16:
                body.extend("v%d.w = 1.0;" % bit for bit in range(16) if three & (1 << bit))
            continue
        # items 6 and 8: the position into the output structure; no point size
        if stripped == "gl_Position = position;":
            body.append("vs_out output;")
            body.append("output.position = position;")
            continue
        if stripped == "gl_PointSize = oPts.x;":
            continue
        if stripped == "}" and main_open and line == "}":
            out = macro_text(macros, "HLSL_INTERFACE_OUTPUT")
            if lit_normal:
                out += macro_text(macros, "HLSL_INTERFACE_NORMAL_OUTPUT")
            if lit_position:
                out += macro_text(macros, "HLSL_INTERFACE_POSITION_OUTPUT")
            out += macro_text(macros, "HLSL_INTERFACE_RETURN")
            body.extend(out.splitlines())
            body.append("}")
            continue
        body.append(line)
    if packed & ~0xffff or inputs_seen != 16 or copies_seen != 16:
        return None, "the GLSL's inputs are not the sixteen expected"
    text = "\n".join(body)
    # item 4: the screen offset is screen.x, the point size gone
    text = re.sub(r"\bscreen_offset\b", "screen.x", text)
    text = re.sub(r"\bpoint_size\b", "0.0", text)
    return normalise(head + grammar(text)), None


SAMPLER_TEXTURES = {"sampler2D": "Texture2D", "sampler3D": "Texture3D", "samplerCube": "TextureCube"}


def expected_pixel(gl_text, entry, macros):
    """The HLSL a pixel shader's GLSL twin says (the target from the manifest: ps_4_1 with SV_Coverage)."""
    lines = gl_text.splitlines()
    coverage = "gl_SampleMask" in gl_text
    if coverage != (entry["target"] == macros["HLSL_PIXEL_COVERAGE_TARGET"].strip('"')):
        return None, "the target %s is not the GLSL's (%s)" % (
            entry["target"], "it writes gl_SampleMask" if coverage else "no gl_SampleMask")
    for forbidden in ("atomicCounter", "visible_samples", "early_fragment_tests", "texture_lod_bias"):
        if forbidden in gl_text:
            return None, "the GLSL has %s, which the desktop's never has" % forbidden
    lit_normal = "in vec4 xWorldNormal;" in gl_text
    lit_position = "in vec3 xWorldPosition;" in gl_text
    textures = []
    start = None
    for index, line in enumerate(lines):
        match = re.match(r"uniform (sampler\w+) tex(\d);$", line.strip())
        if match:
            stage = int(match.group(2))
            textures.append("%s tex%d : register(t%d);" % (SAMPLER_TEXTURES[match.group(1)], stage,
                                                          int(macros["HLSL_TEXTURE_SLOT_FIRST"]) + stage))
            textures.append("SamplerState tex%d_sampler : register(s%d);" % (
                stage, int(macros["HLSL_SAMPLER_SLOT_FIRST"]) + stage))
            start = index + 1
    if start is None or len(textures) != 8:
        return None, "the GLSL has not four samplers"
    # items 1, 4, 5, 6: the prologue (the version, the ins, the output, the loose uniforms, the samplers)
    head = macro_text(macros, "HLSL_PIXEL_PARAMETERS_TEXT") + macro_text(macros, "HLSL_INTERFACE_BEGIN")
    if lit_normal:
        head += macro_text(macros, "HLSL_INTERFACE_NORMAL_MEMBER")
    if lit_position:
        head += macro_text(macros, "HLSL_INTERFACE_POSITION_MEMBER")
    head += macro_text(macros, "HLSL_INTERFACE_END")
    if coverage:
        head += macro_text(macros, "HLSL_PIXEL_COVERAGE_OUTPUT")
    head += macro_text(macros, "HLSL_INTERFACE_STATICS") + "\n".join(textures) + "\n"
    body = []
    main_open = False
    for line in lines[start:]:
        stripped = line.strip()
        if re.match(r"uniform vec4 model_lights\[\d+\];$", stripped):
            body.extend(macro_text(macros, "HLSL_MODEL_LIGHTS_TEXT").splitlines())
            continue
        match = re.match(r"in (vec[34]) (xWorld\w+);$", stripped)
        if match:
            body.append("static %s %s;" % (grammar(match.group(1)), match.group(2)))
            continue
        if stripped == "void main()":
            body.append("ps_out main(vs_out input)" if coverage else "float4 main(vs_out input) : SV_Target")
            main_open = True
            continue
        if main_open and stripped == "{" and line == "{":
            body.append("{")
            body.extend(macro_text(macros, "HLSL_INTERFACE_INPUT").splitlines())
            if lit_normal:
                body.extend(macro_text(macros, "HLSL_INTERFACE_NORMAL_INPUT").splitlines())
            if lit_position:
                body.extend(macro_text(macros, "HLSL_INTERFACE_POSITION_INPUT").splitlines())
            if coverage:
                body.append("ps_out output;")
            main_open = False
            continue
        # item 9: the coverage, the colour returned
        match = re.match(r"gl_SampleMask\[0\] = (.*);$", stripped)
        if match:
            body.append("output.coverage = (uint)(%s);" % match.group(1))
            continue
        match = re.match(r"fragment_color = (.*);$", stripped)
        if match:
            # item 11 (phase 6): the manifest's constant_alpha mark means the one multiply line before the return
            if entry.get("constant_alpha") == "1":
                body.append("result *= blend_constant.w;")
            elif entry.get("constant_alpha") == "2":
                body.append("result *= 1.0 - blend_constant.w;")
            if coverage:
                body.append("output.color = %s;" % match.group(1))
                body.append("return output;")
            else:
                body.append("return %s;" % match.group(1))
            continue
        body.append(line)
    text = "\n".join(body)
    # item 4: the alpha reference is a float4's x
    text = re.sub(r"\balpha_reference\b(?!\.)", "alpha_reference.x", text)
    return normalise(head + grammar(text)), None


def normalise(text):
    """Lines with their whitespace collapsed, blank ones dropped."""
    lines = []
    for line in text.splitlines():
        line = re.sub(r"\s+", " ", line).strip()
        if line:
            lines.append(line)
    return lines


# ---------- the compiler


class Compiler:
    def __init__(self):
        from ctypes import POINTER, c_char_p, c_size_t, c_uint, c_void_p
        self.library = ctypes.WinDLL("d3dcompiler_47")
        self.compile_function = self.library.D3DCompile
        self.compile_function.argtypes = [c_void_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p,
                                          c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
        self.compile_function.restype = ctypes.c_long

    @staticmethod
    def blob(pointer):
        """A blob's bytes through its vtable (GetBufferPointer at 3, GetBufferSize at 4), then Release (2)."""
        from ctypes import POINTER, c_size_t, c_uint, c_void_p
        table = ctypes.cast(ctypes.cast(pointer, POINTER(c_void_p))[0], POINTER(c_void_p))
        data = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(table[3])(pointer)
        size = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(table[4])(pointer)
        result = ctypes.string_at(data, size)
        ctypes.WINFUNCTYPE(c_uint, c_void_p)(table[2])(pointer)
        return result

    def compile(self, source, name, entry, target):
        """(bytecode or None, the compiler's messages, seconds)."""
        from ctypes import byref, c_void_p
        code, errors = c_void_p(), c_void_p()
        begin = time.perf_counter()
        result = self.compile_function(source, len(source), name.encode(), None, None, entry.encode(),
                                       target.encode(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, byref(code), byref(errors))
        seconds = time.perf_counter() - begin
        messages = self.blob(errors).decode(errors="replace") if errors.value else ""
        bytecode = self.blob(code) if code.value else None
        return (bytecode if result >= 0 else None), messages, seconds


# ---------- the keys

# struct nv2a_pixel_shader_key's fields after the combiner words (xgpu.h, as the 32-bit build lays it out)
KEY_TAIL = (("texture_modes", 4), ("sampler_type[0]", 1), ("sampler_type[1]", 1), ("sampler_type[2]", 1),
            ("sampler_type[3]", 1), ("alpha_kill[0]", 1), ("alpha_kill[1]", 1), ("alpha_kill[2]", 1),
            ("alpha_kill[3]", 1), ("color_sign[0]", 1), ("color_sign[1]", 1), ("color_sign[2]", 1),
            ("color_sign[3]", 1), ("alpha_test_function", 4), ("fog_enable", 1), ("fog_table_mode", 1),
            ("count_samples", 1), ("coverage_alpha", 1), ("point_threshold", 1), ("per_pixel_lighting", 1),
            ("alpha_test_samples", 1), ("(padding)", 1))
KEY_TAIL_SIZE = sum(size for _, size in KEY_TAIL)
# the fields the Direct3D 11 device keeps 0 (phase 6's)
KEY_ZERO = ("count_samples", "alpha_test_samples", "(padding)")


def key_statistics(keys):
    """(the fields with their distinct values' count, the combiner words, how many of those vary)."""
    size = len(keys[0])
    words = (size - KEY_TAIL_SIZE) // 4
    columns = defaultdict(set)
    for key in keys:
        if len(key) != size:
            continue
        for word in range(words):
            columns["combiner_state[%d]" % word].add(struct.unpack_from("<I", key, word * 4)[0])
        at = words * 4
        for name, width in KEY_TAIL:
            columns[name].add(int.from_bytes(key[at:at + width], "little"))
            at += width
    varying = sum(1 for name, values in columns.items() if name.startswith("combiner_state") and len(values) > 1)
    return [(name, columns[name]) for name, _ in KEY_TAIL], words, varying


# ---------- the manifest


def read_manifest(folder):
    """{file: {field: value}} from manifest.txt."""
    entries = {}
    for line in (folder / "manifest.txt").read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        fields = dict(part.split("=", 1) for part in parts[1:] if "=" in part)
        entries[parts[0]] = fields
    return entries


def comparison(path, entry, macros):
    """(None, None) if the HLSL is its twin's, else (why, the diff's lines)."""
    twin = path.with_suffix(".glsl")
    if not twin.exists():
        return "no GLSL twin (%s)" % twin.name, []
    if entry["stage"] == "vertex":
        expected, why = expected_vertex(twin.read_text(), entry, macros)
    else:
        expected, why = expected_pixel(twin.read_text(), entry, macros)
    if expected is None:
        return why, []
    actual = normalise(path.read_text())
    if actual == expected:
        return None, None
    diff = list(difflib.unified_diff(expected, actual, "expected from the GLSL twin", path.name, lineterm="", n=1))
    return "differs", diff[:40]


def check(folder, gl_folder):
    macros = header_macros()
    entries = read_manifest(folder)
    shaders = [name for name, entry in entries.items() if name.endswith(".hlsl") and "entry" in entry]
    unlisted = sorted(path.name for path in folder.glob("*.hlsl") if path.name not in entries)
    compiler = Compiler()
    counts = Counter()
    failures = Counter()
    failed = []
    sizes = Counter()
    seconds = Counter()
    slowest = []
    for name in shaders:
        entry = entries[name]
        path = folder / name
        stage = entry["stage"]
        counts[stage] += 1
        counts[stage + " " + entry["target"]] += 1
        problems = []
        source = path.read_bytes()
        bytecode, messages, elapsed = compiler.compile(source, name, entry["entry"], entry["target"])
        if bytecode is None:
            failures["compile"] += 1
            problems.append("compile (%s %s): %s" % (entry["entry"], entry["target"], messages.strip()))
        else:
            sizes[stage] += len(bytecode)
            seconds[stage] += elapsed
            slowest.append((elapsed, name, len(source), len(bytecode)))
        why, diff = comparison(path, entry, macros)
        if why:
            failures["comparison"] += 1
            problems.append("comparison: " + why + ("\n" + "\n".join(diff) if diff else ""))
        if problems:
            failed.append(name)
            print("FAIL %s:" % name)
            for problem in problems:
                for line in problem.splitlines():
                    print("    " + line)
    keys = [(folder / name).read_bytes() for name, entry in entries.items() if name.endswith(".key")]
    print()
    print("files: %d vertex shaders (%d at vs_4_0), %d pixel shaders (%d at ps_4_0, %d at ps_4_1), %d keys; "
          "%d failed" % (counts["vertex"], counts["vertex vs_4_0"], counts["pixel"], counts["pixel ps_4_0"],
                         counts["pixel ps_4_1"], len(keys), len(failed)))
    if unlisted:
        print("not in the manifest (not checked): %s" % ", ".join(unlisted))
    for kind, count in sorted(failures.items()):
        print("failures: %s x%d" % (kind, count))
    for stage in ("vertex", "pixel"):
        if counts[stage]:
            print("compiled, %s: %.0f ms in all, %.1f ms on average; bytecode %d bytes in all, %d on average" % (
                stage, seconds[stage] * 1000, seconds[stage] * 1000 / counts[stage], sizes[stage],
                sizes[stage] // counts[stage]))
    print("  (phase 0 on this PC: vs_large 146 ms 4,960 B, vs_typical 24 ms 2,712 B, ps_large 26 ms 3,524 B, "
          "ps_typical 8.5 ms 1,708 B)")
    for elapsed, name, source_size, code_size in sorted(slowest, reverse=True)[:5]:
        print("  slowest: %s %.1f ms (%d bytes of HLSL, %d of bytecode)" % (name, elapsed * 1000, source_size,
                                                                           code_size))
    if keys:
        fields, words, varying = key_statistics(keys)
        print("distinct pixel keys: %d of %d; of the key's %d combiner words, %d vary" % (
            len(set(keys)), len(keys), words, varying))
        for name, values in fields:
            note = ""
            if name in KEY_ZERO:
                if values != {0}:
                    note = "  <- must be 0 everywhere"
                    failures["key"] += 1
                else:
                    note = "  (0 by design)"
            elif len(values) == 1:
                note = "  <- never varies (%s)" % ", ".join("%#x" % value for value in sorted(values))
            print("  %-22s %d distinct value(s): %s%s" % (name, len(values), ", ".join(
                "%#x" % value for value in sorted(values)[:16]) + (" ..." if len(values) > 16 else ""), note))
    if gl_folder:
        gl_match(folder, entries, gl_folder)
    return 1 if failed or failures["key"] else 0


def content(path):
    return hashlib.sha1("\n".join(normalise(path.read_text())).encode()).hexdigest()


def gl_match(folder, entries, gl_folder):
    """The twins against the OpenGL executable's dump, by content."""
    twins = {}
    for name, entry in entries.items():
        if name.endswith(".glsl") and "twin" in entry:
            twins.setdefault(content(folder / name), []).append(name)
    gl_files = {}
    for path in sorted(gl_folder.glob("*.glsl")):
        gl_files.setdefault(content(path), []).append(path.name)
    print()
    for stage, prefix in (("vertex", "vs"), ("pixel", "ps")):
        mine = {key: names for key, names in twins.items() if names[0].startswith(prefix)}
        theirs = {key: names for key, names in gl_files.items() if names[0].startswith(prefix)}
        matched = set(mine) & set(theirs)
        print("%s: %d distinct twin texts here, %d distinct texts in the OpenGL dump (%d files); %d in both" % (
            stage, len(mine), len(theirs), sum(len(names) for names in theirs.values()), len(matched)))
        for key in sorted(set(mine) - matched, key=lambda k: mine[k][0]):
            hlsl = mine[key][0].replace(".glsl", ".hlsl")
            print("  here only: %s (%s)" % (", ".join(mine[key]), " ".join(
                "%s=%s" % item for item in entries.get(hlsl, {}).items() if item[0] in ("gl", "program", "lit"))))
        for key in sorted(set(theirs) - matched, key=lambda k: theirs[k][0]):
            print("  OpenGL only: %s" % ", ".join(theirs[key]))
    # by name: the OpenGL executable names its pixel shaders by the key's hash_words and its vertex shaders by the
    # program's id and variant, which the manifest gives as gl=: a file of that name with other content is a key, or
    # a twin, that differs from the OpenGL executable's for the same draw
    named = Counter()
    for name, entry in sorted(entries.items()):
        if "gl" not in entry or not name.endswith(".hlsl"):
            continue
        stage = entry["stage"]
        theirs = gl_folder / entry["gl"]
        if not theirs.exists():
            named[stage + " not met by the OpenGL executable"] += 1
        elif content(theirs) == content(folder / name.replace(".hlsl", ".glsl")):
            named[stage + " the same file"] += 1
        else:
            named[stage + " DIFFERENT"] += 1
            print("  DIFFERENT: %s against the OpenGL executable's %s" % (name, entry["gl"]))
    print("by the OpenGL executable's names (gl= in the manifest): %s" % "; ".join(
        "%s %d" % item for item in sorted(named.items())))


# ---------- --prove


def prove(folder):
    """A one-line change in a pixel shader must fail the comparison."""
    macros = header_macros()
    entries = read_manifest(folder)
    for name, entry in entries.items():
        if name.endswith(".hlsl") and entry.get("stage") == "pixel":
            path = folder / name
            lines = path.read_text().splitlines()
            for index, line in enumerate(lines):
                if "return clamp(result, 0.0, 1.0);" in line:
                    lines[index] = line.replace("return clamp(result, 0.0, 1.0);",
                                                "return clamp(result * 0.0, 0.0, 1.0);")
                    break
            else:
                continue
            twin = path.with_suffix(".glsl")
            expected, why = expected_pixel(twin.read_text(), entry, macros)
            actual = normalise("\n".join(lines))
            print("the one-line change in %s (a shader that compiles and draws black): %s" % (
                name, "the comparison fails" if actual != expected else "NOT CAUGHT"))
            for line in difflib.unified_diff(expected, actual, "expected", name, lineterm="", n=0):
                print("    " + line)
            unchanged = normalise(path.read_text())
            print("the same shader unchanged: %s" % ("the comparison passes" if unchanged == expected else "FAILS"))
            return 0 if actual != expected and unchanged == expected else 1
    print("no pixel shader in the folder")
    return 1


# ---------- --copies

COPIES = (("port/linux/src/nv2a_vsh.c", "port/windows/d3d11/nv2a_vsh_hlsl.c"),
          ("port/linux/src/nv2a_psh.c", "port/windows/d3d11/nv2a_psh_hlsl.c"))

# the dialect's items (D3D11.md, phase 4), each by what a differing line has, in this order of precedence; and
# what the Files table says besides them (the copy's header and entry point; the lighting detection not copied)
CLASSES = (
    ("0 the copy's header comment, include and entry point (the Files table)",
     r"^(NV2A_[VP]SH(_HLSL)?\.C|Translation of|GLSL\.$|HLSL, for the|register combiners, as held|phase 4\)|"
     r"to change it\)|\(the constant buffers|hlsl_shaders\.h\)|structure, with|epilogue's output|screen-space conversion "
     r"undone|the original's, line|the desktop's\)\. The model|is not copied|followed automatically|shader's text is|"
     r"constant buffers, the interface's|their samplers|ps_4_1\) and the colour|the alpha test, alpha kill|lighting and "
     r"the debug|line; the Android|come \(the copy|automatically\. tools|is the original's|#include \"hlsl_shaders\.h\"|"
     r"char \*nv2a_(hlsl_)?(vertex|pixel)_shader|const struct nv2a_pixel_shader_key \*key\)|"
     r"unsigned long packed_attribute_mask, const struct|uint32_t integer_mask, uint32_t three|"
     r"\(the original's arguments|const DWORD \*instructions = |unsigned long instruction_count = count;|"
     r"register combiners, as held in the pixel shader render states - into|Direct3D 11 renderer \(port/windows|"
     r"^4\)\. A copy of|^change it\) in which|hlsl_shaders\.h: nv2a_hlsl)"),
    ("1 the prologue: no #version, precision, layout, invariant; no Android branch",
     r"#version|precision|HALO_ANDROID|^#(ifdef|else|endif)|shading_language|SHADER_VERSION|invariant|"
     r"early_fragment|atomic_uint|count_samples|occlusion counter|samples that pass the depth|the #version line"),
    ("9 the pixel shader's ends: SV_Coverage at ps_4_1, the colour returned, no counter",
     r"coverage|SV_Coverage|ps_4_1|gl_SampleMask|fragment_color|return clamp\(result|atomicCounter|"
     r"HLSL_PIXEL_(COVERAGE_)?TARGET|ps_out|comparison_test|output\.color"),
    ("7 the vertex inputs: a structure, packed, integer and three-component",
     r"vs_in|_packed|_int\b|v%lu_in|integer_mask|three_component_mask|w = 1\.0|three-component|layout reads four|"
     r'^xgpu_text_append\(&text, "\};\\n"\);$'),
    ("6 the interface: the structure, its statics, main's input and output",
     r"HLSL_INTERFACE|vs_out|xWorldNormal|xWorldPosition|void main|main\(|out vec4 x|in vec4 x|out float xFog|"
     r"in float xFog|interface's structure|lit members|the constant buffer; the|after the nine|the constant buffers; the|"
     r'^"\}\\n",$|dot0 = 0\.0, dot1 = 0\.0'),
    ("8 clip space and the epilogue: the desktop's, no point size",
     r"gl_Position|gl_PointSize|glClipControl|GL: rows from|clip space is desktop|rows from the top, depth|"
     r"no point size|output\.position"),
    ("4 constant buffers in place of the loose uniforms",
     r"uniform|cbuffer|packoffset|screen_offset|screen\.x|point_size|alpha_reference|model_lights\[%d\]|"
     r"columns the menus shift|"
     r"HLSL_(VERTEX|PIXEL|MODEL)_\w+_TEXT|XGPU_MODEL_LIGHT_COUNT|XGPU_PIXEL_UNIFORMS|texture_lod_bias|oPts"),
    ("5 textures and samplers",
     r"sampler(2D|3D|Cube)|Texture(2D|3D|Cube)|_sampler|\.Sample\(|texture\(tex|SAMPLE_BIAS|register\(t|"
     r"LOD bias|ES samplers|, stage$|^\);$|stage, coordinates|stage, stage, coordinates|HLSL_(TEXTURE|SAMPLER)_SLOT|"
     r"coordinates, stage$|^stage,$"),
    ("3 functions: lerp, frac, rsqrt, Sample, Load, GetDimensions, comparisons",
     r"\bmix\(|lerp\(|fract|frac\(|inversesqrt|rsqrt|texelFetch|\.Load\(|textureSize|GetDimensions|lessThan|"
     r"greaterThanEqual|A < B|A >= B"),
    ("2 types, constructors and casts",
     r"(^|[^A-Za-z0-9_]|\\[tn])[iu]?vec[234]\b|float[234]|int[234]|\(float\)|\(int\)|\buint\(|\bint\(|\bfloat\(|\"float3\"|\"vec3\""
     r"|a float3 \(rgb\)|a vec3 \(rgb\)"),
)


def classify(line):
    text = line.strip()
    for name, pattern in CLASSES:
        if re.search(pattern, text):
            return name
    return None


# a line of C that carries no shader text of its own (a brace, a control line, a call's first line, the format's
# placeholders, a comment's end): it takes the class of the change it is part of, the nearest classed line of its
# hunk, and is counted so
STRUCTURAL = re.compile(r'^([{}();,]*|(if|else|for)\b.*|(xgpu_text_append|snprintf)\(&?text,?|"(%s)*"|.*\*/|'
                        r'/\*.*|xgpu_text_append\(&text, "%s", \w+\);|xgpu_text_append\(&text, "};\\n"\);)$')


def copies():
    """The copies' differing lines, classed."""
    unclassed = 0
    for original, copy in COPIES:
        a = (ROOT / original).read_text().splitlines()
        b = (ROOT / copy).read_text().splitlines()
        counts = Counter()
        structural = Counter()
        lighting = 0
        matcher = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
        for tag, a0, a1, b0, b1 in matcher.get_opcodes():
            if tag == "equal":
                continue
            removed, added = a[a0:a1], b[b0:b1]
            # the lighting detection, not copied (the Files table): one block of the original
            if removed and any("nv2a_vertex_shader_lighting(const DWORD" in line for line in removed) and not added:
                lighting += len(removed)
                continue
            hunk = [(sign, line, classify(line)) for sign, lines in (("-", removed), ("+", added)) for line in lines]
            for position, (sign, line, name) in enumerate(hunk):
                if not line.strip():
                    counts["(blank)"] += 1
                    continue
                if name is None and STRUCTURAL.match(line.strip()):
                    nearby = [other for _, _, other in reversed(hunk[:position]) if other] + \
                        [other for _, _, other in hunk[position + 1:] if other]
                    if nearby:
                        counts[nearby[0]] += 1
                        structural[nearby[0]] += 1
                        continue
                if name is None:
                    unclassed += 1
                    counts["UNCLASSED"] += 1
                    print("  unclassed %s%s: %s" % (sign, Path(copy).name, line))
                else:
                    counts[name] += 1
        print("%s against %s: %d lines and %d lines; %d differing lines" % (
            Path(copy).name, Path(original).name, len(b), len(a), sum(counts.values()) + lighting))
        if lighting:
            print("  %4d  the lighting detection (nv2a_vertex_shader_lighting and its helpers), not copied "
                  "(the Files table)" % lighting)
        for name, count in sorted(counts.items()):
            print("  %4d  %s%s" % (count, name, " (%d of them C around the change: braces, ifs, calls)" %
                                   structural[name] if structural[name] else ""))
    return 1 if unclassed else 0


def main():
    arguments = sys.argv[1:]
    if arguments == ["--copies"]:
        return copies()
    if len(arguments) == 2 and arguments[0] == "--prove":
        return prove(Path(arguments[1]))
    if not 1 <= len(arguments) <= 2 or arguments[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    folder = Path(arguments[0])
    gl_folder = Path(arguments[1]) if len(arguments) == 2 else None
    if not (folder / "manifest.txt").exists():
        print("no manifest.txt in %s" % folder)
        return 2
    if gl_folder and not gl_folder.is_dir():
        print("no folder at %s" % gl_folder)
        return 2
    return check(folder, gl_folder)


if __name__ == "__main__":
    sys.exit(main())
