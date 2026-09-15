

# Melee Unlocked - Alpha

An **EXPERIMENTAL** native Windows and Linux build of Super Smash Bros. Melee (NTSC 1.02) with
Slippi online play and an unlocked display frame rate.

The game's own PowerPC code is translated ahead of time into C++ (static recompilation of the
retail executable plus Slippi's Gecko codes) and runs against a native renderer (D3D12 on Windows,
OpenGL on Linux), so the game logic stays exactly what the GameCube ran, at 60 Hz, while the
display runs at any rate. In-between frames come from the game's own animation data and physics
state, not from image interpolation, so an unlocked 200 Hz display shows real intermediate poses
with no added latency.

Nothing from the game is included. You supply your own Melee NTSC 1.02 ISO.

**Linux status:** the Linux port (in this fork) is a first cut. It runs the recompiled game, Slippi
online, the GameCube adapter, memory cards, audio and the unlocked frame rate, but a few
Windows-only extras are absent — see [Linux](#linux) below.

This project is not affiliated with, endorsed by, or supported by the Slippi team, Nintendo or
HAL Laboratory. Slippi netplay compatibility is implemented from Slippi's open source code
(GPL). Questions and bugs about this build go to this repository or the Discord below, not to
the Slippi team.

First gameplay footage can be found here: https://youtu.be/y2KN3s7uvqM

## Discord / Help
Discord can be found here https://discord.gg/K7HHs3r8ty

## Install

Download `MeleeUnlocked-<version>-win64.zip` from [Releases](https://github.com/hero88go/melee-unlocked/releases)
and extract it anywhere. Then pick one of two ways to run it. **The launcher is optional**;
the game does not depend on it, and the manual way is complete on its own.

### Manual (no launcher)

1. Drag your Melee NTSC 1.02 ISO onto `MeleeUnlocked.bat`, or put the ISO next to it named
   `melee.iso` and double-click `MeleeUnlocked.bat`.
2. Play. The first launch precompiles the graphics pipelines (15 to 30 seconds, progress in
   the title bar). In game, F1 (or Z + Start) opens the PC settings.
3. To update, extract a newer zip over the folder. Settings, saves and replays are kept.

### Melee Unlocked Launcher (optional)

A small window in the same zip, `MeleeUnlockedLauncher.exe`, for people who want setup,
updates and the Slippi account check in one place.

- **Build tab**: drop the ISO onto the window. It checks the disc, precompiles the graphics
  pipelines for your GPU once and remembers the path. The ISO is never copied.
- **Play**: press PLAY. It shows which Slippi account will be used.
- **Updates**: it checks for a new release on every start. "Update and restart" installs it in
  place; settings, saves and replays stay.

### Build from source

Windows 10/11, your own ISO, about 5-10 minutes the first time. The game is translated to C++
and compiled on your machine; nothing from the ISO enters the repository.

Either drag the ISO onto `play.bat` in a clone of this repo (it installs Python, CMake and the
Visual Studio 2022 Build Tools with winget if missing, then builds and starts the game), or:

```powershell
git clone https://github.com/hero88go/melee-unlocked.git
cd melee-unlocked
python tools/extract_dol.py "C:/path/to/melee.iso" build/main.dol
python port/recomp/recomp.py --dol build/main.dol --gct-base 0x8065CC80
cmake -S . -B build-review -G "Visual Studio 17 2022" -A x64 -DMELEE_BUILD_EXPERIMENTAL_PORT=ON
cmake --build build-review --config Release --target melee_port --parallel
build-review/port/Release/melee_port.exe --iso "C:/path/to/melee.iso" --threaded-renderer --fps unlocked --frame-mode authored --scale auto --volume 70
```

(Add the target `melee_unlocked` to the build line if you want the optional launcher; run
`build-review/port/Release/MeleeUnlockedLauncher.exe` from the checkout and it finds the repo.)

## Linux

The Linux port lives in this fork. The recompiler and the game/runtime code are shared with the
Windows build; the Linux build swaps the Win32 platform layer and the D3D12 renderer for SDL2 and
OpenGL 4.5, and uses libusb for the GameCube adapter and libcurl/OpenSSL for online reporting. The
GX shaders are the same generators as the D3D12 build, translated to GLSL at runtime.

**What is missing on Linux (Windows-only features):**

- **No in-game PC settings overlay (F1).** Linux reads `port-settings.ini` and the command line
  instead: `--volume`, `--scale`, `--frame-mode`, `--fps`, `--widescreen`, `--vsync`, `--anisotropy`,
  `--sharpness`, `--ssaa`, `--window WxH`, `--fullscreen`.
- **No launcher / self-updater** (`MeleeUnlockedLauncher.exe` and the update check are Windows only).
- **No DLSS/DLAA** (NVIDIA Streamline is D3D12/Windows only).

Everything else is present: recompiled game logic, Slippi online (matchmaking, rollback, replays,
reporting), the GameCube adapter, memory cards, audio, and the unlocked/sub-frame presentation.

### Build from source (Linux)

Tested on Arch; adjust package names elsewhere. First build compiles ~145 generated translation
units, a few minutes on a modern CPU.

```bash
# Arch
sudo pacman -S --needed base-devel cmake ninja python sdl2 glew curl openssl mesa libusb
# Debian/Ubuntu
# sudo apt install build-essential cmake ninja-build python3 libsdl2-dev libglew-dev \
#   libcurl4-openssl-dev libssl-dev libusb-1.0-0-dev

git clone https://github.com/ikz87/melee-unlocked.git
cd melee-unlocked

# Turn your own ISO into C++ (nothing from the ISO enters the repository).
python3 tools/extract_dol.py "/path/to/melee.iso" build/main.dol
python3 port/recomp/recomp.py --dol build/main.dol --gct-base 0x8065CC80

cmake -S . -B build-linux -G Ninja -DMELEE_BUILD_EXPERIMENTAL_PORT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"

# Headless (no window): proves the simulation boots and runs.
./build-linux/port/melee_port --iso "/path/to/melee.iso" --headless --fast --frames 600
# Windowed.
./build-linux/port/melee_port --iso "/path/to/melee.iso" --volume 70
```

The pinned `melee/` decomp checkout is **optional** on Linux. Without it, authored sub-frame
animation uses a stub (the game still runs; the standard interpolation modes are unaffected). With
it, `tools/generate_fobj_host.py` generates the authored sampler from `fobj.c`/`spline.c`.

Useful flags: `--frames N`, `--fast`, `--frame-mode off|authored|interpolate|extrapolate`,
`--window 1280x960`, `--fullscreen`, `--vsync`, `--scale N|auto`, `--widescreen`, `--volume 0-100`,
`--script FILE` (recorded inputs), `--capture out.ppm --capture-frame N [--capture-every N]`,
`--user-dir <Slippi folder>`, `--replay-dir`, `--card-dir`, `--sys-dir`, `--log-file`.

Input scripts use `FRAME A+B+X ... [sx= sy= cx= cy=] [p=N]`, state held until the next line; buttons
are `A B X Y Z L R START DU DD DL DR`, and `@match` / `@loop N` are supported (see
`port/scripts/online_bot.txt`).

### Linux: GameCube adapter and controllers

- The official/Mayflash **WUP-028 adapter** (`057e:0337`) is driven over libusb with the same
  protocol as the Windows build. It is not exposed as a joystick, so udev permissions are needed —
  the Dolphin rule (`51-gcadapter.rules`) or equivalent. Adapter port 1 takes precedence over the
  keyboard/SDL gamepad on player 1.
- Any **SDL2-recognised gamepad** works as player 1, and the **keyboard** fallback is always
  available (arrows = stick, `IJKL` = C-stick, `Z X C V` = A B X Y, `Q W E` = L R Z, `Enter` = Start,
  `TFGH` = D-pad).

### Linux: audio

Audio is emulated by the game's own DSP and mixed (plus Slippi Jukebox music) into SDL2. The ring
buffer is resampled with a fill-steered cubic interpolator to track the sound-card clock, the same
approach as the Windows WASAPI path. The default volume is 70 on Linux (there is no settings panel
to unmute); change it with `--volume` or `volume=N`/`music=N` in `port-settings.ini`.

## FAQ

**Was this "vibe coded"?**

This was developed using Fable 5.1 and GPT 6 Astra, much like the 100% decomp. 
You can either complain about it or enjoy it, the truth is the decomp + PC port would not have been possible or would have taken infinitely longer without the latest AI coding models.
As humans we can either work with the robots or against them, I believe in technlogical progress and making cool shit, if we do not use all tools available we are choosing to limit our results.
I will not handicap myself and theres no reason anyone has to wait any longer for ports and advancements like this. If I were to shy away from every new technology I would not be the person I am today. 

I am interested in collabing with other developers but so far have found no collective space for this type of dicussion; PC port dicussion is actively discouraged in the Melee decomp discord
My vision for the project is keeping it open source so anyone can view the work and make it better. 

## Features

- Unlocked frame rate (monitor rate, a fixed cap, or fully unlocked) with sub-frame animation
- Slippi online against regular Slippi Dolphin players, using your Slippi Launcher login
- GameCube adapter (WUP-028), keyboard fallback. Windows: WinUSB/Zadig driver. Linux: libusb + udev
  permissions; SDL gamepads also work
- DLSS / DLAA (NVIDIA Streamline), internal resolution up to 8x, SSAA, anisotropic filtering,
  sharpening, borderless fullscreen, VSync *(DLSS is Windows/D3D12 only; the rest are on both)*
- Widescreen 16:9 (Slippi's own optional code, online safe)
- Memory card saves as .gci files (Dolphin GCI-folder format, drop in your existing save)
- PC settings overlay in the game window: F1 or Z + Start *(Windows only; on Linux use
  `port-settings.ini` and the command line)*
- Optional launcher with self-update *(Windows only)*

## Slippi online

Everything Slippi Dolphin does for netplay is built in: matchmaking, rollback netcode, the
Slippi code set, replay recording, game reporting. Slippi Dolphin itself is not needed and is
not touched.

**Is the Slippi Launcher required?** For online play, yes: a Slippi account is required and
accounts are created and logged in only through the [Slippi Launcher](https://slippi.gg/downloads).
Install it, log in once, and the game picks up that login automatically. Windows reads
`%APPDATA%\Slippi Launcher\netplay\User\Slippi\user.json`; Linux reads
`~/.config/SlippiOnline/Slippi/user.json` (falling back to `~/.config/slippi-dolphin/netplay[-beta]/Slippi`).
Override with `--user-dir <folder>`. The **Slippi** Launcher also installs the WinUSB driver a
GameCube adapter needs on Windows. For offline play the Slippi Launcher is not required. NOTE:
**we are not affiliated with the Slippi team.**

Unranked, Direct codes and Teams work against players on regular Slippi Dolphin; they change
nothing on their side. Replays (.slp) are written to `replays/` (Windows releases use `Replays\`).


## Bug reports

Open a [GitHub issue](https://github.com/hero88go/melee-unlocked/issues) using the template.
Attach `melee_port.log` from the game folder, your `port-settings.ini`, and the .slp replay if
the bug happened in a match.

## Repository layout

`port/recomp/` is the recompiler (Python): it reads the DOL and the Slippi code tables
(`port/slippi_sys/`, vendored from Slippi) and writes `port/generated/` (not committed).
`port/runtime/` is the host runtime: PowerPC helpers, HLE of the GameCube SDK (OS, VI, PAD, DVD,
AI/AX audio, CARD, EXI), the Slippi EXI device, netcode, game reporting, the renderer and the
sub-frame solver. `port/app/launcher.cpp` is the optional launcher. `tools/` holds validation,
benchmarking and packaging scripts. See `PORT_COMPLETION.md` for the technical state and
evidence, `HANDOFF_FABLE_3.md` for the roadmap.

`port/PortLinux.cmake` is the Linux build. The Linux-only pieces are `port/runtime/gx/gx_gl.cpp`
(OpenGL backend), `port/runtime/gx/gx_gl_shader.cpp` (translates the shared HLSL shader generators
to GLSL, so the TEV combiner and vertex lighting come from one generator), `port/runtime/host/window_sdl.cpp`,
`audio_sdl.cpp`, `gc_adapter_libusb.cpp`, and the small stubs. Windows-only files are wrapped in
`#ifdef _MSC_VER`; POSIX-only files in `#ifndef _MSC_VER`, so both platforms compile the same tree.

`tools/package_release.py` produces the release zip (version from `VERSION`). The replay
playback build (`melee_port_playback`, used to verify frame-exactness against Dolphin replays)
is described in `PORT_COMPLETION.md`.



## Verification

Development launcher: `run-native.bat` (or `python tools/launch_native.py --iso <iso> ...`) starts the
build in `build-review/port/Play/` if present, else the Release build, muted and windowed by default.

- `ctest --test-dir build-review -C Release`: unit tests (Windows)
- `ctest --test-dir build-linux`: unit tests (Linux)
- `python tools/validate_native.py --iso <iso>`: 2400 simulation checkpoints must match across
  headless, hidden, threaded and authored rendering
- `python tools/online_pair.py --script port/scripts/online_bot.txt`: two local instances play a
  full Slippi online match; the log must show no `DESYNC`

## License

GPL-2.0-or-later. Parts of the runtime are ports of Dolphin and Slippi Ishiiruka code (GPL-2.0).
Third-party components: ENet, Dear ImGui, nlohmann/json, NVIDIA Streamline (see `licenses/` in a
release and `port/third_party/`). Super Smash Bros. Melee is the property of Nintendo and HAL
Laboratory; this project contains none of its data.
