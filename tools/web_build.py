"""Ninja rules for the WebAssembly build (``ninja web``), a feasibility spike.

It compiles the same game sources as ``ninja linux`` with Emscripten for
wasm32 (32-bit pointers, like every other build of the game), plus the
shared platform layer in ``port/linux/src`` and the browser-only pieces in
``port/web/src``, and links ``build/web/halo.{js,wasm}``.

The platform layer's non-x86 / OpenGL ES path is the Android one, so the web
build defines HALO_ANDROID as well as HALO_WEB and overrides the few
Android-host specifics under HALO_WEB. See docs/wasm-spike.md.
"""

import json
import os
from pathlib import Path
from typing import Any, List

from .ninja_syntax import Writer
from .linux_build import (
    PORT_CONFIG, PORT_DIR, XDK_INCLUDE, TOML_DIR, KCP_DIR, MUSL_MATH_DIR, GAME_FLAGS, PLATFORM_FLAGS,
    _load_port_config, _quote, musl_math_sources, xdk_headers,
)

WEB_DIR = Path("port/web")
# SDL 3, pinned to the Android and Windows builds' release rather than the
# SDK's (port/web/ports/sdl3_pinned.py)
SDL_PORT = WEB_DIR / "ports" / "sdl3_pinned.py"
SDL_PORT_FLAG = f"--use-port={SDL_PORT.as_posix()}"
ANDROID_INCLUDE = Path("port/android/include")

# as the Linux build (tools/linux_build.py LINUX_ABI_FLAGS), minus what only
# x86 needs: wasm32 already aligns doubles and 64-bit integers to 8 bytes as
# MSVC does (-malign-double), has no PIC or register struct returns, and no
# frame pointers to walk
WEB_ABI_FLAGS = [
    "-fms-extensions",
    "-fshort-wchar",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-ffp-contract=off",
    "-O2",
    "-g0",
    "-pthread",
    "-DHALO_ANDROID=1",
    "-DHALO_WEB=1",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

# the variadic functions some game units call without a prototype in scope:
# wasm passes variadic arguments in memory and the others as parameters, as
# the Android guest does (tools/android_build.py VARIADIC_PROTOTYPE_FILES)
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
}

# platform units the browser has no use for (self-updater, UPnP, Discord
# rich presence) or that are replaced by port/web/src (page protection)
PLATFORM_EXCLUDE = {"posix_update.c", "posix_upnp.c", "updater.c", "memory_watch.c", "p2p_discord.c"}

WEB_LDFLAGS = [
    "-O2",
    "-pthread",
    SDL_PORT_FLAG,
    "-sMAX_WEBGL_VERSION=2",
    "-sMIN_WEBGL_VERSION=2",
    "-sFULL_ES3=1",
    # the Xbox contiguous window at 0x80000000-0x88000000 (platform.h) lives
    # inside linear memory: wasm32 can address 4 GB
    "-sALLOW_MEMORY_GROWTH=1",
    "-sMAXIMUM_MEMORY=4GB",
    "-sINITIAL_MEMORY=256MB",
    "-sSTACK_SIZE=4MB",
    "-sDEFAULT_PTHREAD_STACK_SIZE=1MB",
    # the game's main loop blocks (it is the Xbox's): run main() on a worker
    # and let it block there; the browser thread only proxies DOM/GL
    "-sPROXY_TO_PTHREAD=1",
    # SDL_GL_SwapWindow yields to the worker's event loop (emscripten_sleep(0))
    # so the OffscreenCanvas presents each frame: JSPI suspends the wasm stack
    "-sJSPI=1",
    "-sOFFSCREENCANVAS_SUPPORT=1",
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas",
    "-sPTHREAD_POOL_SIZE=8",
    "-sEXIT_RUNTIME=0",
    "-sFORCE_FILESYSTEM=1",
    "-sEXPORTED_RUNTIME_METHODS=FS,ENV,callMain",
    "-sENVIRONMENT=web,worker",
    "-sWASMFS=0",
    "-sGL_ENABLE_GET_PROC_ADDRESS=1",
    "--profiling-funcs",
    "-lwebsocket.js",
    "--js-library", "port/web/src/web_library.js",
]


def web_configure_inputs() -> List[Path]:
    return [Path(__file__), WEB_DIR / "web_abi_shims.json", WEB_DIR / "src"] if (WEB_DIR / "src").is_dir() else [Path(__file__)]


def generate_web_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file() or not (WEB_DIR / "src").is_dir():
        return
    config = _load_port_config()
    build_dir: Path = sln.build_dir / "web"
    obj_dir = build_dir / "obj"
    output = build_dir / "halo.js"
    cc = getattr(sln, "web_cc", None) or "emcc"
    prefix_header = PORT_DIR / "include" / "halo_linux_prefix.h"
    # the MSVC inline semantics headers are the Linux build's
    semantics_header = sln.build_dir / "linux" / "halo_msvc_semantics.h"
    platform_semantics_header = sln.build_dir / "linux" / "platform_msvc_semantics.h"

    n.comment("WebAssembly build (ninja web): a feasibility spike, docs/wasm-spike.md")
    n.variable("web_cc", cc)
    n.rule(
        name="web_cc",
        command="$web_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WEB CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # the SDL port, built into Emscripten's cache once before the compiles
    # that use it: left to them, every parallel compile would try to build it
    # at once and fight over the cache's lock
    n.rule(
        name="web_port",
        command="$web_cc -pthread $port -c -x c /dev/null -o $out",
        description="WEB PORT $port",
    )
    n.rule(
        name="web_link",
        command="$web_cc $ldflags -o $out @$out.rsp",
        description="WEB LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    # multiplayer only unless configured with --web-campaign: no Campaign in
    # the main menu and no campaign level loads (docs/gateway.md)
    abi = " ".join(WEB_ABI_FLAGS + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else [])
                   + ([] if getattr(sln, "web_campaign", False) else ["-DHALO_MULTIPLAYER_ONLY=1"]))
    port_include = PORT_DIR / "include"
    sdk_flags = f"-idirafter {XDK_INCLUDE}"
    excluded = set(config.get("exclude_sources", []))
    objects: List[Path] = []
    # calls whose declaration differs from the definition, routed to adapters
    # (tools/web_abi_shims.py)
    shims_path = WEB_DIR / "web_abi_shims.json"
    shim_units = json.loads(shims_path.read_text())["units"] if shims_path.is_file() else {}

    sdl_port_stamp = build_dir / "sdl3_pinned.o"
    n.build(outputs=sdl_port_stamp, rule="web_port", implicit=[SDL_PORT], variables={"port": SDL_PORT_FLAG})

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / source.with_suffix(".o")
        objects.append(obj)
        n.build(
            outputs=obj,
            rule="web_cc",
            inputs=source,
            implicit=[*xdk_headers(), prefix_header, semantics_header, platform_semantics_header],
            order_only=[sdl_port_stamp],
            variables={"cflags": cflags},
        )

    for proj in sln.projects:
        if proj.name not in config["projects"]:
            continue
        options = proj.options
        defines = " ".join(f"-D{d}" for d in options.get("defines") or [])
        includes = " ".join(
            f"-I{_quote(d)}" for d in options.get("include_dirs") or [] if Path(d) != Path("xbox/include")
        )
        game_cflags = " ".join([
            abi, " ".join(GAME_FLAGS), f"-include {prefix_header}", f"-include {semantics_header}",
            defines, f"-I{port_include}", includes, sdk_flags,
        ])
        for obj in proj.objects:
            name = str(obj.file_path).replace(os.sep, "/")
            if obj.status.name == "Missing" or name in excluded or obj.file_path.suffix.lower() != ".c":
                continue
            cflags = game_cflags
            if name in VARIADIC_PROTOTYPE_FILES:
                cflags += f" -include {ANDROID_INCLUDE}/halo_android_variadic_prototypes.h"
            for define in shim_units.get(name, []):
                cflags += f" -D{define}"
            add_object(obj.file_path, cflags)
        for source in sorted(Path(config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)

    platform_dir = Path(config["platform_sources"])
    platform_cflags = " ".join([
        abi, " ".join(PLATFORM_FLAGS), "-w", SDL_PORT_FLAG,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{platform_dir}", f"-I{port_include}", f"-I{WEB_DIR / 'src'}", f"-I{TOML_DIR}", f"-I{KCP_DIR}",
        "-Isource -Isource/cseries", sdk_flags,
    ])
    # posix_*.c talk to the C library only (Emscripten's musl here), with its
    # own ABI: 32-bit wchar_t
    posix_cflags = " ".join(["-O2", "-g0", "-pthread", "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_WEB=1",
                             "-D_FILE_OFFSET_BITS=64", "-w", f"-I{platform_dir}"])
    for source in sorted(platform_dir.glob("*.c")):
        if source.name in PLATFORM_EXCLUDE:
            continue
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    for source in sorted((WEB_DIR / "src").glob("*.c")):
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
    add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w"]))
    for source in musl_math_sources():
        add_object(source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                     f"-include {MUSL_MATH_DIR}/include/libm.h"]))

    n.build(
        outputs=output,
        rule="web_link",
        inputs=objects,
        implicit=[WEB_DIR / "src" / "web_library.js", sdl_port_stamp],
        # Emscripten's runtime checks (heap, stack cookie, argument checks
        # in the JavaScript glue) in development builds only: a
        # configure.py --release build has none, as it has no game
        # assertions
        variables={"ldflags": " ".join(WEB_LDFLAGS + [
            "-sASSERTIONS=0" if getattr(sln, "port_release", False) else "-sASSERTIONS=1"])},
        implicit_outputs=[build_dir / "halo.wasm"],
    )
    n.build(outputs="web", rule="phony", inputs=output)
    n.newline()
