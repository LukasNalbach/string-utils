#!/usr/bin/env python3
# Writes the files of external/libsais that patches/libsais.patch adds or changes, from the files of the commit the
# submodule is pinned at:
#
#   src/libsais64.c, include/libsais64.h      + libsais64_plcp_int (libsais_plcp_int of src/libsais.c with 64 bits)
#   src/libsais40.cpp, include/libsais40.h    libsais64 with 40-bit integers (libsais40_impl) and, compiled with
#                                             LIBSAIS_PACKED_BYTES=6, 48-bit integers (libsais48_impl)
#   src/libsais16x40.cpp, include/libsais16x40.h
#                                             libsais16x64 with 40-bit integers (libsais16x40_impl)
#
# include/libsais40.hpp and include/libsais40_types.hpp are not generated. The conditional operators that g++ rejects
# (an entry and an integer as operands) are found by compiling the generated sources with g++. Run it from the root of
# string-utils ([<libsais directory>] for another checkout of libsais), then write the patch with
# patches/make-patch.sh libsais. Moving the submodule to another commit of libsais:
#
#   git -C external/libsais apply --reverse ../../patches/libsais.patch
#   git -C external/libsais checkout <commit>
#   git -C external/libsais apply --include=include/libsais40.hpp --include=include/libsais40_types.hpp ../../patches/libsais.patch
#   python3 patches/port-libsais40.py && patches/make-patch.sh libsais
#
# A pattern of a replacement that does not occur in the source is an error, so a change of libsais that the port does
# not cover stops the script.

import re
import subprocess
import sys
from pathlib import Path

libsais = Path(sys.argv[1] if len(sys.argv) > 1 else "external/libsais")


def upstream(path):
    return subprocess.run(["git", "-C", str(libsais), "show", f"HEAD:{path}"],
        check=True, capture_output=True, text=True).stdout


def replace(text, old, new, count=None, regex=False):
    pattern, replacement = (old, new) if regex else (re.escape(old), lambda match: new)
    result, n = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if n == 0 or (count is not None and n != count):
        sys.exit(f"port-libsais40.py: {n} matches of {old!r} (expected {count if count is not None else '> 0'})")
    return result


def block_end(lines, start):
    # index of the line that closes the brace opened in or after lines[start]
    depth = 0
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and "{" in "".join(lines[start:i + 1]):
            return i
    sys.exit(f"port-libsais40.py: unbalanced braces after {lines[start]!r}")


def function(text, name):
    # the definition of the function `name` (from its first line to the closing brace)
    lines = text.split("\n")
    start = next((i for i, line in enumerate(lines)
        if re.match(rf"^\S.*\b{name}\(", line) and not line.rstrip().endswith(";")), None)
    if start is None:
        sys.exit(f"port-libsais40.py: no definition of {name}")
    return "\n".join(lines[start:block_end(lines, start) + 1]) + "\n"


def remove_functions(text, pattern):
    lines, i, result, removed = text.split("\n"), 0, [], 0
    while i < len(lines):
        if re.match(rf"^static .*\b{pattern}\(", lines[i]):
            i = block_end(lines, i) + 1
            if i < len(lines) and lines[i] == "":
                i += 1
            removed += 1
        else:
            result.append(lines[i])
            i += 1
    if removed == 0:
        sys.exit(f"port-libsais40.py: no function matches {pattern}")
    return "\n".join(result)


def remove_int32_paths(text):
    # removes the blocks `if (n <= INT32_MAX ...) { ... }` that pass the input to the 32-bit functions of libsais
    lines, i, result, removed = text.split("\n"), 0, [], 0
    while i < len(lines):
        if re.match(r"^\s*if \(n <= INT32_MAX.*\)$", lines[i]):
            i = block_end(lines, i) + 1
            if i < len(lines) and lines[i] == "" and result and result[-1] == "":
                i += 1
            removed += 1
        else:
            result.append(lines[i])
            i += 1
    if removed == 0:
        sys.exit("port-libsais40.py: no block `if (n <= INT32_MAX ...)`")
    return "\n".join(result)


def int64_paths_for_packed_ints(text):
    # turns `#if SAINT_BIT == 32 ... #else ... #endif` into `#if SAINT_BIT == 32 ... #elif SAINT_BIT == 64 ... #endif`
    lines, stack = text.split("\n"), []
    for i, line in enumerate(lines):
        directive = line.strip()
        if directive.startswith("#if"):
            stack.append(directive)
        elif directive.startswith("#endif"):
            stack.pop()
        elif directive == "#else" and stack[-1] == "#if SAINT_BIT == 32":
            lines[i] = line.replace("#else", "#elif SAINT_BIT == 64")
    return "\n".join(lines)


def after_directive_block(text, opening, insertion):
    # inserts `insertion` after the #endif that closes the line `opening`
    lines, depth, start = text.split("\n"), 0, None
    for i, line in enumerate(lines):
        directive = line.strip()
        if start is None:
            if directive == opening:
                start, depth = i, 1
            continue
        if directive.startswith("#if"):
            depth += 1
        elif directive.startswith("#endif"):
            depth -= 1
            if depth == 0:
                return "\n".join(lines[:i + 1] + insertion.split("\n") + lines[i + 1:])
    sys.exit(f"port-libsais40.py: no block {opening!r}")


def api_functions(header, prefix):
    return re.findall(rf"\bint64_t ({prefix}\w*)\(", header)


# libsais64_plcp_int: libsais_compute_plcp_int, libsais_compute_plcp_int_omp and libsais_plcp_int of src/libsais.c

def add_plcp_int(source, header):
    libsais_c, libsais_h = upstream("src/libsais.c"), upstream("include/libsais.h")

    compute = function(libsais_c, "libsais_compute_plcp_int") + "\n" + function(libsais_c, "libsais_compute_plcp_int_omp")
    compute = replace(compute, "const int32_t * RESTRICT T", "const sa_sint_t * RESTRICT T", 2)
    compute = compute.replace("libsais_", "libsais64_")
    source = replace(source, "static void libsais64_compute_plcp_gsa(",
        compute + "\nstatic void libsais64_compute_plcp_gsa(", 1)

    plcp_int = function(libsais_c, "libsais_plcp_int")
    plcp_int = replace(plcp_int, "int32_t libsais_plcp_int(const int32_t * T, const int32_t * SA, int32_t * PLCP, int32_t n)",
        "int64_t libsais64_plcp_int(const int64_t * T, const int64_t * SA, int64_t * PLCP, int64_t n)", 1)
    plcp_int = plcp_int.replace("libsais_", "libsais64_")
    source = replace(source, "int64_t libsais64_plcp_gsa(const uint8_t", plcp_int + "\nint64_t libsais64_plcp_gsa(const uint8_t", 1)

    doc = re.search(r"(    /\*\*\n(?:    \*.*\n)*?    \*/\n)    LIBSAIS_API int32_t libsais_plcp_int\(", libsais_h).group(1)
    declaration = doc + "    LIBSAIS64_API int64_t libsais64_plcp_int(const int64_t * T, const int64_t * SA, int64_t * PLCP, int64_t n);\n"
    header = replace(header, r"(    LIBSAIS64_API int64_t libsais64_plcp\(.*\);\n)", lambda match: match.group(1) + "\n" + declaration, 1, regex=True)
    return source, header


# changes of libsais64.c and libsais16x64.c for integers of 5 or 6 bytes

packed_lanes = """
#if defined(LIBSAIS_LANES) && (SAINT_BIT != 32) && (SAINT_BIT != 64)

typedef fast_sint_t PREFIX_lms_base_t;

static PREFIX_lms_base_t PREFIX_lms_base(fast_sint_t base)
{
    return base;
}

static PREFIX_lms_base_t PREFIX_store_lms_lanes(sa_sint_t * RESTRICT p, PREFIX_lms_base_t base, const uint8_t * RESTRICT lanes)
{
    fast_sint_t j; for (j = 0; j < 8; j += 1) { p[j] = (sa_sint_t)(base - (fast_sint_t)lanes[j]); }

    return base - 8;
}

#if defined(LIBSAIS_OPENMP)

static fast_uint_t PREFIX_has_zero(const sa_sint_t * RESTRICT p)
{
    fast_uint_t zero = 0; fast_sint_t j; for (j = 0; j < 16; j += 1) { zero |= (fast_uint_t)(p[j] == 0); }

    return zero;
}

#endif

#endif"""


def port_source(source, old, new, header_32_bits, types, sa_type):
    source = replace(source, f'#include "{header_32_bits}"\n', "", 1).replace(old, new)
    source = replace(source, "typedef int64_t                         sa_sint_t;\ntypedef uint64_t                        sa_uint_t;\n", types, 1)

    # the 32-bit paths and the conversions between 32 and 64 bits they use
    source = remove_int32_paths(source)
    source = remove_functions(source, rf"{new}_convert_\w+")

    # OpenMP takes an int
    source = replace(source, r"\bnum_threads\(((?:[^()]|\([^()]*\))*)\)", r"num_threads((int)(\1))", regex=True)

    # buckets in the free space after SA: aligned by index, not by address
    source = replace(source, r"\(sa_sint_t \*\)" + new + r"_align_up\(&SA\[([^]]*)\], \(size_t\)alignment \* sizeof\(sa_sint_t\)\)",
        r"&SA[sa40_align_index(\1, alignment)]", regex=True)

    # the arrays of the API
    def api(match):
        return match.group(0).replace("int64_t * ", f"{sa_type} * ")
    source = replace(source, rf"^int64_t {new}\w*\(.*\)$", api, regex=True)
    source = replace(source, "sizeof(int64_t)", "sizeof(sa_sint_t)")
    source = replace(source, rf"return ({new}_unbwt_aux(?:_omp)?)\(T, U, A, n, freq, n, &i(, threads)?\);",
        rf"{sa_type} I = i; return \1(T, U, A, n, freq, n, &I\2);", 2, regex=True)

    return source.rstrip("\n") + "\n"


# g++ cannot choose the type of `c ? x : y` if one of x and y is an entry (a class type that converts to and from
# int64_t or uint64_t) and the other one an integer; each such entry is cast to the type it had in libsais64
# (int64_t or uint64_t), so that the expression has the same type as there

compilations = [
    ["-DLIBSAIS_OPENMP", "-fopenmp", "-mavx2"],
    ["-DLIBSAIS_OPENMP", "-fopenmp"],
    [],
    ["-DLIBSAIS_OPENMP", "-fopenmp", "-mavx2", "-DLIBSAIS_PACKED_BYTES=6"],
]


def operand_end(text, i, stops):
    # the end of the operand of ?: that starts at text[i]
    depth, nested = 0, 0
    while True:
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return i
            depth -= 1
        elif depth == 0 and c in stops:
            return i
        elif depth == 0 and c == "?":
            nested += 1
        elif depth == 0 and c == ":":
            if nested == 0:
                return i
            nested -= 1
        i += 1


def cast_operand(text, start, end, cast):
    operand = text[start:end]
    core = operand.strip()
    lead, trail = operand[:len(operand) - len(operand.lstrip())], operand[len(operand.rstrip()):]
    if re.fullmatch(r"[A-Za-z_]\w*(\[[^][]*\])?", core):
        core = f"({cast}){core}"
    else:
        core = f"({cast})({core})"
    return text[:start] + lead + core + trail + text[end:]


def fix_conditionals(path):
    for flags in compilations:
        while True:
            result = subprocess.run(["g++", "-std=c++20", "-fsyntax-only", "-fmax-errors=0", "-w", "-I",
                str(libsais / "include"), *flags, str(path)], capture_output=True, text=True, env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"})
            errors = re.findall(r"^.*?:(\d+):(\d+): error: (.*)$", result.stderr, flags=re.MULTILINE)
            if not errors:
                break
            conditionals = []
            for line, column, message in errors:
                operand_type = r"'([^']*)'(?: \{aka '([^']*)'\})?"
                types = re.fullmatch(rf"operands to '\?:' have different types {operand_type} and {operand_type}", message)
                if types is not None and "sa_packed_int" in message:
                    conditionals.append((int(line), int(column), types.group(2) or types.group(1), types.group(4) or types.group(3)))
            if not conditionals:
                line, column, message = errors[0]
                sys.exit(f"port-libsais40.py: {path}:{line}:{column}: {message}")

            text = path.read_text()
            starts = [0] + [i + 1 for i, c in enumerate(text) if c == "\n"]
            for line, column, type_second, type_third in sorted(set(conditionals), reverse=True):
                question = starts[line - 1] + column - 1
                if text[question] != "?":
                    sys.exit(f"port-libsais40.py: {path}:{line}:{column}: no ? at the position of the error")
                colon = operand_end(text, question + 1, "")
                end = operand_end(text, colon + 1, ",;")
                for operand_type, (start, stop) in reversed([(type_second, (question + 1, colon)), (type_third, (colon + 1, end))]):
                    if "sa_packed_int" in operand_type:
                        text = cast_operand(text, start, stop, "fast_uint_t" if "unsigned" in operand_type else "fast_sint_t")
            path.write_text(text)


def port_libsais40(source, header):
    names = [name for name in api_functions(header, "libsais64") if name != "libsais64"]
    renames = "\n".join(f"    #define {'libsais40' + name[len('libsais64'):]:<27} libsais48{name[len('libsais64'):]}" for name in names)
    types = f"""\
// the number of bytes of an entry: 5 (libsais40, the default) or 6 (libsais48: this file compiled with
// LIBSAIS_PACKED_BYTES=6, with the functions declared in libsais40.h named libsais48*)
#if !defined(LIBSAIS_PACKED_BYTES)
    #define LIBSAIS_PACKED_BYTES        5
#endif

#if LIBSAIS_PACKED_BYTES == 5
namespace libsais40_impl {{
#elif LIBSAIS_PACKED_BYTES == 6
    #define libsais40                   libsais48
{renames}
namespace libsais48_impl {{
#else
    #error "LIBSAIS_PACKED_BYTES must be 5 or 6"
#endif

typedef sa_packed_int<LIBSAIS_PACKED_BYTES, int64_t> sa_sint_t;
typedef sa_packed_int<LIBSAIS_PACKED_BYTES, uint64_t> sa_uint_t;
"""
    source = port_source(source, "libsais64", "libsais40", "libsais.h", types, "sa_sint_t")
    source = replace(source, "#define SAINT_BIT                       (64)", "#define SAINT_BIT                       (8 * LIBSAIS_PACKED_BYTES)", 1)
    source = replace(source, "#define SAINT_MAX                       INT64_MAX", "#define SAINT_MAX                       SA_PACKED_INT_MAX(LIBSAIS_PACKED_BYTES)", 1)
    source = replace(source, "#define SAINT_MIN                       INT64_MIN", "#define SAINT_MIN                       SA_PACKED_INT_MIN(LIBSAIS_PACKED_BYTES)", 1)

    # SIMD on the arrays of entries: with 64 bits only; with 5 or 6 bytes, the scalar code
    source = int64_paths_for_packed_ints(source)
    source = after_directive_block(source, "#if LIBSAIS_CPU_FEATURE >= LIBSAIS_CPU_FEATURE_AVX2", packed_lanes.replace("PREFIX", "libsais40"))
    return source + "\n} // namespace libsais40_impl or libsais48_impl\n"


def port_libsais16x40(source):
    types = "namespace libsais16x40_impl {\n\ntypedef sa_int40_t                      sa_sint_t;\ntypedef sa_uint40_t                     sa_uint_t;\n"
    source = port_source(source, "libsais16x64", "libsais16x40", "libsais16.h", types, "sa_int40_t")
    source = replace(source, "#define SAINT_BIT                       (64)", "#define SAINT_BIT                       (40)", 1)
    source = replace(source, "#define SAINT_MAX                       INT64_MAX", "#define SAINT_MAX                       SA_INT40_MAX", 1)
    source = replace(source, "#define SAINT_MIN                       INT64_MIN", "#define SAINT_MIN                       SA_INT40_MIN", 1)
    if "LIBSAIS_CPU_FEATURE" in source:
        sys.exit("port-libsais40.py: libsais16x64.c uses SIMD, which the port does not cover")
    return source + "\n} // namespace libsais16x40_impl\n"


def port_header(header, old, new, sa_type, namespace):
    header = header.replace(old, new).replace(old.upper(), new.upper())
    header = replace(header, '#ifdef __cplusplus\nextern "C" {\n#endif\n\n    #include <stdint.h>\n',
        f'#include "libsais40_types.hpp"\n\nnamespace {namespace} {{\n', 1)
    header = replace(header, rf"^(    {new.upper()}_API int64_t {new}\w*\(.*\);)$",
        lambda match: match.group(1).replace("int64_t * ", f"{sa_type} * "), regex=True)
    return header


def libsais48_declarations(header):
    declarations, openmp = [], []
    in_openmp = False
    for line in header.split("\n"):
        if line.strip() == "#if defined(LIBSAIS_OPENMP)":
            in_openmp = True
        elif line.strip() == "#endif":
            in_openmp = False
        elif line.startswith("    LIBSAIS40_API int64_t libsais40"):
            declaration = line.replace("LIBSAIS40_API ", "").replace("libsais40", "libsais48").replace("sa_int40_t", "sa_int48_t")
            (openmp if in_openmp else declarations).append(declaration)
    return "\n".join(declarations) + "\n\n#if defined(LIBSAIS_OPENMP)\n" + "\n".join(openmp) + "\n#endif\n"


def main():
    if not (libsais / "src/libsais64.c").exists():
        sys.exit("port-libsais40.py: external/libsais does not exist; run this script from the root of string-utils")

    source64, header64 = add_plcp_int(upstream("src/libsais64.c"), upstream("include/libsais64.h"))
    (libsais / "src/libsais64.c").write_text(source64)
    (libsais / "include/libsais64.h").write_text(header64)

    header40 = port_header(header64, "libsais64", "libsais40", "sa_int40_t", "libsais40_impl")
    header40 = replace(header40, "#ifdef __cplusplus\n}\n#endif\n", "} // namespace libsais40_impl\n\n"
        "// the functions of libsais40_impl with 48-bit integers (src/libsais40.cpp compiled with LIBSAIS_PACKED_BYTES=6)\n"
        "namespace libsais48_impl {\n\n" + libsais48_declarations(header40) + "\n} // namespace libsais48_impl\n", 1)
    (libsais / "include/libsais40.h").write_text(header40)
    (libsais / "src/libsais40.cpp").write_text(port_libsais40(source64, header64))
    fix_conditionals(libsais / "src/libsais40.cpp")

    header16x64 = upstream("include/libsais16x64.h")
    header16x40 = port_header(header16x64, "libsais16x64", "libsais16x40", "sa_int40_t", "libsais16x40_impl")
    header16x40 = replace(header16x40, "#ifdef __cplusplus\n}\n#endif\n", "} // namespace libsais16x40_impl\n", 1)
    (libsais / "include/libsais16x40.h").write_text(header16x40)
    (libsais / "src/libsais16x40.cpp").write_text(port_libsais16x40(upstream("src/libsais16x64.c")))
    fix_conditionals(libsais / "src/libsais16x40.cpp")


if __name__ == "__main__":
    main()
