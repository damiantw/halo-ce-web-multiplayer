#!/usr/bin/env python3
"""Adapters for the game's mismatched function declarations (``ninja web``).

The game is C89 compiled as MSVC compiled it: some units call functions
through an implicit declaration (returning int) or through a local
declaration whose return type or parameters differ from the definition's.
On x86 and AArch64 such calls work, since a result nobody reads or an extra
argument is harmless. WebAssembly checks every call's signature, and wasm-ld
replaces each mismatched call with a trap ("function signature mismatch").

This script reads those wasm-ld warnings from a link log and writes
  - port/web/src/web_abi_shims.c: per mismatch, an adapter with the caller's
    signature that calls the definition with its own, and
  - port/web/web_abi_shims.json: per calling unit, the -D that routes its
    calls to the adapter (tools/web_build.py adds them).
Entries already in the JSON are kept, so the fix-link-repeat loop converges:
  ninja web 2>&1 | tee link.log; python tools/web_abi_shims.py link.log
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JSON_PATH = ROOT / "port/web/web_abi_shims.json"
C_PATH = ROOT / "port/web/src/web_abi_shims.c"
OBJ_PREFIX = "build/web/obj/"
C_TYPES = {"i32": "int", "i64": "long long", "f32": "float", "f64": "double", "void": "void"}

WARNING = re.compile(r"function signature mismatch: (\S+)\n>>> defined as (.*) in (.*)\n>>> defined as (.*) in (.*)")


def parse_signature(text):
    params, result = text.split("->")
    params = [p.strip() for p in params.strip()[1:-1].split(",") if p.strip()]
    return params, result.strip()


def defines_symbol(obj, name):
    """whether the object (or library member) defines name"""
    if not obj.startswith(OBJ_PREFIX):
        return True  # the C library
    nm = Path(subprocess.run(["which", "emcc"], capture_output=True, text=True).stdout.strip()).parent.parent / "bin/llvm-nm"
    run = subprocess.run([str(nm), "--defined-only", str(ROOT / obj)], capture_output=True, text=True)
    if run.returncode != 0:
        return None  # llvm-nm cannot read some of these objects; the other side decides
    return any(line.split()[-1] == name for line in run.stdout.splitlines() if line.split())


def adapter_name(name, caller):
    params, result = caller
    return f"web_abi_{name}__{'_'.join(params) or 'v'}__{result}"


# adapters that need more than a change of signature
SPECIAL = {
    # random_numbers.c calls time() with a 32-bit time_t; Emscripten's is 64-bit
    "time": "{result} {adapter}(int *a0)\n{{\n\tlong long now = time((long long *)0);\n\n"
            "\tif (a0)\n\t\t*a0 = (int)now;\n\treturn (int)now;\n}}",
    # a variadic definition called without its prototype and no variable
    # arguments: hand it an empty argument list
    "error": "static int web_abi_no_arguments[4];\nextern void error(int, int, int *);\n"
             "{result} {adapter}(int a0, int a1)\n{{\n\terror(a0, a1, web_abi_no_arguments);\n\treturn 0;\n}}",
}


def adapter_source(name, caller, definition):
    cparams, cresult = caller
    if name in SPECIAL:
        prefix = "extern long long time(long long *);\n" if name == "time" else ""
        return prefix + SPECIAL[name].format(result=C_TYPES[cresult], adapter=adapter_name(name, caller))
    dparams, dresult = definition
    adapter = adapter_name(name, caller)
    dargs = []
    for index, ptype in enumerate(dparams):
        if index < len(cparams):
            dargs.append(f"({C_TYPES[ptype]})a{index}")
        else:
            dargs.append(f"({C_TYPES[ptype]})0")  # an argument the caller leaves out (a va_list pointer)
    call = f"{name}({', '.join(dargs)})"
    decl_params = ", ".join(C_TYPES[p] for p in dparams) or "void"
    adapter_params = ", ".join(f"{C_TYPES[p]} a{i}" for i, p in enumerate(cparams)) or "void"
    lines = [f"extern {C_TYPES[dresult]} {name}({decl_params});",
             f"{C_TYPES[cresult]} {adapter}({adapter_params})", "{"]
    for index in range(len(dparams), len(cparams)):
        lines.append(f"\t(void)a{index};")
    if cresult == "void":
        lines.append(f"\t{call};")
    elif dresult == "void":
        lines += [f"\t{call};", "\treturn 0;"]
    else:
        lines.append(f"\treturn ({C_TYPES[cresult]}){call};")
    lines.append("}")
    return "\n".join(lines)


def main():
    log = Path(sys.argv[1]).read_text()
    data = json.loads(JSON_PATH.read_text()) if JSON_PATH.is_file() else {"adapters": {}, "units": {}}
    for name, sig_a, obj_a, sig_b, obj_b in WARNING.findall(log):
        defines_a, defines_b = defines_symbol(obj_a, name), defines_symbol(obj_b, name)
        if defines_a is False and defines_b is not False:
            caller_obj, caller_sig, def_sig = obj_a, sig_a, sig_b
        elif defines_b is False and defines_a is not False:
            caller_obj, caller_sig, def_sig = obj_b, sig_b, sig_a
        else:
            # wasm-ld names the mismatched reference first, then the definition
            caller_obj, caller_sig, def_sig = obj_a, sig_a, sig_b
        caller = parse_signature(caller_sig)
        definition = parse_signature(def_sig)
        adapter = adapter_name(name, caller)
        data["adapters"][adapter] = {"name": name, "caller": caller, "definition": definition}
        unit = caller_obj[len(OBJ_PREFIX):-2] + ".c"
        defines = data["units"].setdefault(unit, [])
        define = f"{name}={adapter}"
        if define not in defines:
            defines.append(define)
        print(f"{unit}: {name} {caller_sig} -> definition {def_sig}")
    JSON_PATH.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n")
    parts = ["/* generated by tools/web_abi_shims.py; see there */", ""]
    for adapter, entry in sorted(data["adapters"].items()):
        parts += [adapter_source(entry["name"], tuple(entry["caller"]), tuple(entry["definition"])), ""]
    C_PATH.write_text("\n".join(parts))


if __name__ == "__main__":
    main()
