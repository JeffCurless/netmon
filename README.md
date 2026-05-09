# NetMon

A WiFi network monitor for the **Raspberry Pi Pico W** and **Pico 2 W**, built on top of [picoOS](https://github.com/JeffCurless/picoOS) and displayed on a Pimoroni Display Pack 2 (320×240).

NetMon continuously scans for nearby WiFi networks and presents them on an interactive display — signal strength, channels, security type, and BSSID — with a live RSSI history graph for any selected network.

```
Home
├── WiFi          — live scan list with RSSI, detail view per network
├── Bluetooth     — stub (future)
└── About         — version info and memory stats
```

---

## Hardware

| Item | Notes |
|------|-------|
| Raspberry Pi Pico W or Pico 2 W | RP2040 or RP2350 with CYW43 WiFi |
| Pimoroni Display Pack 2 | ST7789V 320×240, four buttons (A/B/X/Y) |
| Micro-USB cable | Must carry data, not just power |

---

## Navigation

| Button | Action |
|--------|--------|
| **A** | Move cursor up |
| **B** | Move cursor down |
| **X** | Select / enter |
| **Y** | Back |

---

## Setting up the environment

### 1. Install the toolchain

**Linux (Debian / Ubuntu / Raspberry Pi OS)**

```bash
sudo apt update
sudo apt install -y \
    cmake \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    build-essential \
    python3 \
    git
```

**macOS**

```bash
brew install cmake python3
brew install --cask gcc-arm-embedded
```

### 2. Get the Pico SDK

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/workspace/pico-sdk
cd ~/workspace/pico-sdk
git submodule update --init --recursive
```

Add to your `~/.bashrc` (or `~/.zshrc`):

```bash
export PICO_SDK_PATH="$HOME/workspace/pico-sdk"
```

---

## Cloning the repository

NetMon uses picoOS as a **git submodule**. You must initialise the submodule after cloning or the build will fail.

```bash
# Clone and initialise the submodule in one step
git clone --recurse-submodules git@github.com:JeffCurless/netmon.git
cd netmon
```

If you have already cloned without `--recurse-submodules`:

```bash
git submodule update --init
```

After a `git pull` that advances the submodule pointer, always sync:

```bash
git pull
git submodule update --init
```

### Submodule quick reference

| Task | Command |
|------|---------|
| First-time clone with submodule | `git clone --recurse-submodules <url>` |
| Initialise after a plain clone | `git submodule update --init` |
| Sync after `git pull` | `git submodule update --init` |
| Update picoOS to latest main | `cd picoOS && git pull && cd .. && git add picoOS && git commit -m "Update picoOS"` |
| Check which picoOS commit is pinned | `git submodule status` |

---

## Building

The `./build` script compiles the firmware and places the `.uf2` image in `kits/`, named `netmon-<VERSION>.uf2`. The version is read from the `VERSION` file at the repo root — increment it there before a release.

```bash
# Build for Pico W with Display Pack 2 (default)
./build

# Build for both Pico W and Pico 2 W
./build all

# Build for Pico 2 W only
./build pico2

# Remove all build directories
./clean
```

Build directories follow the pattern `build_<board>_<display>/` (e.g. `build_picow_D2/`).

### Manual single-variant build

```bash
cmake -B build_picow_D2 \
      -DPICO_SDK_PATH="$HOME/workspace/pico-sdk" \
      -DPICO_BOARD=picow \
      -DPICOOS_DISPLAY_ENABLE=ON \
      -DPICOOS_DISPLAY_PACK2=ON
make -j$(nproc) -C build_picow_D2
```

---

## Flashing

### Method A — BOOTSEL drag and drop

1. Hold **BOOTSEL** on the Pico while plugging in USB.
2. The Pico mounts as **RPI-RP2**.
3. Copy the firmware:

```bash
cp kits/netmon-0.0.3.uf2 /media/$USER/RPI-RP2/
```

The Pico reboots into NetMon automatically.

### Method B — from the running shell

If NetMon (or picoOS) is already running, type `update` at the USB shell prompt to reboot directly into BOOTSEL mode, then copy the `.uf2` as above.

### Connecting to the USB shell

```bash
pip install pyserial
python3 picoOS/tools/console.py
```

The console auto-detects the Pico by USB VID:PID. Press **Ctrl-C** to exit.

---

## Project structure

```
netmon/
├── VERSION              Firmware version string (e.g. 0.0.3)
├── CMakeLists.txt       Root build — board aliases, app injection, add_subdirectory(picoOS/src)
├── build                Convenience build script
├── clean                Convenience clean script
├── apps/
│   ├── app_table.c      Defines app_table[] — the shell's `run` command registry
│   ├── netmon.c         Display UI: screens, WiFi scan loop, rendering
│   └── netmon.h         Entry point declaration
└── picoOS/              Git submodule — kernel, shell, drivers, build infrastructure
    └── src/
        ├── kernel/      task, sched, mem, sync, syscall, fs, vfs, dev, wifi
        ├── shell/       USB CDC interactive shell
        ├── apps/        app_table.h (ABI header) + built-in demo apps
        └── drivers/     ST7789 display, RGB LED
```

### How app injection works

`CMakeLists.txt` sets `PICOOS_INCLUDE_DEMO_APPS=OFF` and passes the project's own source files via `PICOOS_APP_SOURCES` and `PICOOS_APP_INCLUDE_DIRS` before calling `add_subdirectory(picoOS/src)`. This replaces picoOS's built-in demo `app_table[]` with NetMon's own.

---

## Adding a new application

1. Create `apps/<name>.c` and `apps/<name>.h` with entry `void <name>_entry(void *arg)`.
2. Add an entry to `app_table[]` in `apps/app_table.c`.
3. Add the source file to `PICOOS_APP_SOURCES` in the root `CMakeLists.txt`.
4. Launch from the USB shell: `run <name>`.

Application rules:
- Never return from the entry function for persistent apps — use `for (;;)`.
- Use `sys_sleep(ms)` from `kernel/syscall.h`, **not** `sleep_ms()` — the SDK call busy-waits and starves the scheduler.
- Use priority **4** for normal apps (shell=2, idle=7; lower number = higher priority).

---

## picoOS documentation

NetMon builds on picoOS. The following documents in the submodule cover the kernel APIs, build system, and architecture in detail:

| Document | Contents |
|----------|----------|
| [picoOS/README.md](picoOS/README.md) | Overview, hardware support, shell commands, architecture summary |
| [picoOS/docs/setup.md](picoOS/docs/setup.md) | Full environment setup, build, flash, and debug guide |
| [picoOS/docs/application.md](picoOS/docs/application.md) | How to write and register a picoOS application |
| [picoOS/docs/picoOS_API.md](picoOS/docs/picoOS_API.md) | Kernel API reference (syscall, sync, fs, dev, shell) |
| [picoOS/docs/project-submodule.md](picoOS/docs/project-submodule.md) | Tutorial: using picoOS as a git submodule in your own project |
| [picoOS/docs/design.md](picoOS/docs/design.md) | Architecture design rationale |
| [picoOS/docs/fedora-build.md](picoOS/docs/fedora-build.md) | Fedora / RPM toolchain setup |
| [picoOS/docs/expandfilesystem.md](picoOS/docs/expandfilesystem.md) | Flash filesystem sizing notes |
| [picoOS/docs/imperfections.md](picoOS/docs/imperfections.md) | Catalogue of deliberate teaching imperfections in the kernel |

---

## Memory budget

With Display Pack 2 enabled on RP2040 (Pico W), approximately **143 KB** of the 264 KB SRAM is available as headroom. Run the picoOS memory report tool after a build:

```bash
python3 picoOS/tools/mem_report.py build_picow_D2/picoOS/src/*.elf.map
```

---

## License

NetMon is source-available for educational and non-commercial use under the same terms as picoOS (MIT with Commons Clause). See [picoOS/README.md](picoOS/README.md) for details.
