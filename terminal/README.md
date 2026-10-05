# kitty on Android

This port compiles upstream **kitty 0.48.2** and **CPython 3.13.7** into the APK.
The upstream parser, Screen, key encoder, FreeType rasterizer and HarfBuzz shaping
engine drive the existing guest PTY. Android's GLSurfaceView hosts a GLES 3 cell
renderer; Java supplies lifecycle, hardware-key and InputConnection glue.

It supports the terminal behavior covered by Bash, less and Vim acceptance tests,
Unicode input and display, normal/alternate screens, colors, cursor, basic text
decoration, 2,000 lines of scrollback, resize and activity reconnect. The screen and
glyph cache belong to the process, and survive activity recreation. Direct IME
text and special keys use upstream kitty's encoder. The extra row supplies Escape,
one-key Ctrl/Alt modifiers, Tab and four arrows; there is no separate command field.
Two-finger pinch changes the font size and PTY grid, preserving the chosen size
across restarts. The row hides while a physical alphabetic keyboard is connected;
the menu remains available, and unplugging the keyboard restores the row.
Each terminal has its own screen and response queue, with white default text on
black and a shared glyph cache. Color queries report the active screen's colors;
applications can set and reset them through the upstream OSC callbacks. The guest advertises xterm-256color and
C.UTF-8; the presence of kitty's engine does not advertise every desktop feature.

## Build

Install Python 3.12+, a host C/C++ toolchain, CMake, Ninja, pkg-config, curl, patch,
Android SDK and NDK 29.0.14206865. On macOS, the SDK defaults to
`$HOME/Library/Android/sdk`; elsewhere set ANDROID_HOME or pass `--sdk`.

```sh
python3 terminal/build.py
bash harness/build.sh
```

`sources.lock.json` pins every source archive and the Meson wheel. CPython's
Android dependency binaries have a separate SHA256 lock in `python-deps.lock.json`;
upstream license texts absent from those archives are pinned in `licenses.lock.json`.
The builder verifies archives before extraction, preinstalls verified Python
dependencies, applies `patches/kitty-android.patch`, builds static font/rendering
libraries and packages all required shared libraries, Python modules and licenses.
The host does not execute Android or Debian binaries. Native libraries are linked
for 16 KiB page compatibility and tested on both 4 KiB and 16 KiB systems.

Downloads, extracted sources and intermediate objects live in `terminal/build/cache`.
`--offline` requires those checksum-verified archives. `--runtime-only` recompiles
kitty and the adapter using dependency libraries from a prior successful build;
omit it after changing dependency pins or the NDK. Packaging uses fixed ZIP entry
timestamps. A clean source/dependency build is tested; byte-identical builds across
different host toolchains are not claimed.

The Python runtime is installed under app-private `files/kitty/<content hash>`.
Extraction uses a disposable staging directory, bounds expanded data to 192 MiB,
rejects paths escaping the directory, and makes extension modules read-only.
An interrupted extraction is removed and retried on the next launch.

## Boundaries

- This uses a new GLES adapter over upstream kitty's screen and font output. It
  does not port the desktop GLFW backend or every desktop rendering feature.
- The glyph atlas is capped at 1024 × 1024 × 4 RGBA pixels. System fonts supply
  fallback glyphs; appearance varies by device. Font size/DPI are currently fixed.
- All kitty graphics commands return ENOTSUP. In particular, guest escape sequences
  cannot invoke upstream file/shared-memory transports on Android broker paths.
- Clipboard, remote control, notifications and window-management callbacks are
  inactive. Rich selection, paste UI, mouse protocols, advanced
  decoration and broader IME coverage remain work for the product frontend.
- The service retains detached Linux jobs even when every terminal closes. Android process
  death ends jobs; persistent Debian files and packages remain.

`TerminalAcceptance` is debug-harness instrumentation, not an exported command
receiver. It exercises actual view input, guest programs and GLES frames, verifies
graphics-path rejection, and saves a screenshot and report. Run it after the
developer suite has installed less and vim-tiny:

```sh
adb -s SERIAL shell am instrument -w -r dev.goblinlinux.sentry/.TerminalAcceptance
python3 tests/terminal_lifecycle_test.py SERIAL
```

The source patches and dependency license texts accompany the packaged runtime.
kitty is GPL-3.0-only. The matching source archives, local patches, build recipes
and notices are included in the release handoff described in [source distribution](../release/README.md).
