# SDL 3 for the web build, pinned to the release the Android and Windows
# builds use (tools/android_build.py SDL_TAG, tools/windows_build.py
# SDL_VERSION), instead of the one the Emscripten SDK bundles (-sUSE_SDL=3:
# 3.4.2 in emsdk 6.0.10).
#
# Why: SDL 3.4.2's Emscripten joystick driver reads navigator.getGamepads()
# on the calling thread, and with -sPROXY_TO_PTHREAD that is the game's
# worker, which has no Gamepad API: the first gamepad the browser reports
# throws "navigator.getGamepads is not a function" and no gamepad ever
# works. Fixed in SDL 3.4.4 (libsdl-org/SDL be8643f739).
#
# An external Emscripten port (--use-port=port/web/ports/sdl3_pinned.py,
# tools/web_build.py): the same build as the SDK's own tools/ports/sdl3.py
# (sources, flags, and the SDK's pre-generated SDL_build_config.h, whose
# template is unchanged between 3.4.2 and 3.4.16 but for two settings the
# web build does not use), with its own name, library and include folder in
# the Emscripten cache, so it never mixes with the SDK's SDL 3. The zip is
# checked against SHA512; to move to another SDL release, change VERSION and
# HASH (sha512sum of the zip) together.

import glob
import os
import shutil

VERSION = '3.4.16'
TAG = f'release-{VERSION}'
HASH = 'af4109305de11e94619404feffd2874f26a25cbde71b7c8d8f3cd2210232858f18061df85df6a4467d5109f168381d6eb4dc1e27bd375d12f7448d1cbd71da41'
SUBDIR = f'SDL-{TAG}'
NAME = 'sdl3_pinned'

URL = 'https://www.libsdl.org/'
DESCRIPTION = f'SDL {VERSION}, pinned for the web build'
LICENSE = 'zlib license'


def get_lib_name(settings):
  return f'libSDL3_pinned-{VERSION}' + ('-mt' if settings.PTHREADS else '') + '.a'


def get(ports, settings, shared):
  ports.fetch_project(NAME, f'https://github.com/libsdl-org/SDL/archive/{TAG}.zip', sha512hash=HASH)

  def create(final):
    root_dir = ports.get_dir(NAME, SUBDIR)

    # the SDK's pre-generated configuration of SDL 3 for Emscripten
    shutil.copyfile(shared.path_from_root('tools', 'ports', 'sdl3', 'SDL_build_config.h'),
                    os.path.join(root_dir, 'include', 'SDL3', 'SDL_build_config.h'))

    # <SDL3/...> from this port's own include folder (process_args), not the
    # SDK's
    ports.install_headers(os.path.join(root_dir, 'include', 'SDL3'), target=os.path.join(NAME, 'SDL3'))

    # as the SDK's tools/ports/sdl3.py
    glob_patterns = [
      '*.c',
      'atomic/*.c',
      'audio/*.c',
      'camera/*.c',
      'core/unix/*.c',
      'cpuinfo/*.c',
      'dynapi/*.c',
      'events/*.c',
      'io/*.c',
      'io/generic/*.c',
      'filesystem/*.c',
      'gpu/*.c',
      'joystick/*.c',
      'haptic/*.c',
      'hidapi/*.c',
      'locale/*.c',
      'main/*.c',
      'misc/*.c',
      'power/*.c',
      'render/*.c',
      'render/*/*.c',
      'sensor/*.c',
      'stdlib/*.c',
      'storage/*.c',
      'thread/*.c',
      'time/*.c',
      'timer/*.c',
      'video/*.c',
      'video/yuv2rgb/*.c',
      'tray/*.c',
      'storage/generic/*.c',
      'tray/unix/*.c',
      'time/unix/*.c',
      'timer/unix/*.c',
      'main/emscripten/*.c',
      'filesystem/posix/*.c',
      'filesystem/emscripten/*.c',
      'locale/emscripten/*.c',
      'camera/emscripten/*.c',
      'joystick/emscripten/*.c',
      'joystick/virtual/*.c',
      'power/emscripten/*.c',
      'misc/emscripten/*.c',
      'audio/emscripten/*.c',
      'video/emscripten/*.c',
      'video/offscreen/*.c',
      'audio/disk/*.c',
      'loadso/dlopen/*.c',
      'camera/dummy/*.c',
      'audio/dummy/*.c',
      'video/dummy/*.c',
      'haptic/dummy/*.c',
      'sensor/dummy/*.c',
    ]

    flags = []
    if settings.PTHREADS:
      glob_patterns.append('thread/pthread/*.c')
      flags += ['-pthread']
    else:
      glob_patterns.append('thread/generic/*.c')

    srcs = []
    for pattern in glob_patterns:
      matches = glob.glob(os.path.join(root_dir, 'src', pattern))
      assert matches, pattern
      srcs += matches

    includes = [ports.get_include_dir(NAME, 'SDL3'), ports.get_include_dir(NAME), os.path.join(root_dir, 'src')]
    ports.build_port(root_dir, final, NAME, srcs=srcs, includes=includes, flags=flags)

  return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]


def process_args(ports):
  return ['-I' + ports.get_include_dir(NAME)]


def clear(ports, settings, shared):
  shared.cache.erase_lib(get_lib_name(settings))


def show():
  return f'{NAME} (--use-port=port/web/ports/{NAME}.py; {LICENSE})'
