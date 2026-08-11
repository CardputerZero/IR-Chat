# CardputerZero IR-Chat

Runtime-only infrared chat for M5Stack CardputerZero.

## Features

- Send and receive messages with a compact custom 38 kHz raw IR protocol
- Keep up to 64 messages in memory for the current session only
- Accept messages up to 23 printable ASCII bytes
- Discover receiver and transmitter nodes by LIRC capability instead of device number
- Use a loopback mock backend for SDL desktop development

IR requires line of sight. This protocol is specific to IR Chat and is not an
infrared remote-control protocol.

## Dependencies

Fetch `lvgl`, `spdlog`, and `smooth_ui_toolkit` into `dependencies/`:

```bash
./bootstrap.sh
```

Device builds use the Linux kernel LIRC character-device API directly and do
not require a userspace LIRC library. The receiver must support
`LIRC_CAN_REC_MODE2`, and the transmitter must support `LIRC_CAN_SEND_PULSE`.

## Build

SDL desktop build:

```bash
cmake -S . -B build/sdl -DIR_CHAT_USE_SDL=ON
cmake --build build/sdl -j8
IR_CHAT_SDL_ZOOM=2 ./dist/M5CardputerZero-IR-Chat
```

CardputerZero cross-build from x86 Linux:

```bash
cmake -S . -B build/cp0 \
  -DIR_CHAT_USE_SDL=OFF \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
cmake --build build/cp0 -j8
```

The output binary is `dist/M5CardputerZero-IR-Chat`.

## Usage

- `Z`/`C` or Left/Right: switch between Messages and IR Info
- `F`/`X` or Up/Down: scroll messages
- Enter or a printable key: open the message editor
- Enter: send; Esc: cancel, go back, or exit
- Backspace/Delete and Left/Right: edit the draft
- `R` or Enter on IR Info: retry initialization after an error

## Hardware

By default, the device build scans rc-core/LIRC nodes and selects RX and TX by
capability. These strict overrides are available when automatic discovery is
not appropriate:

- `IR_CHAT_LIRC_RX_DEVICE`: receiver `/dev/lirc*` path
- `IR_CHAT_LIRC_TX_DEVICE`: transmitter `/dev/lirc*` path
- `IR_CHAT_LIRC_RX_RC`: receiver `/sys/class/rc/rc*` path
- `IR_CHAT_LIRC_TX_RC`: transmitter `/sys/class/rc/rc*` path

An override fails initialization when its node is missing or does not advertise
the required capability.

## Package

Build the CardputerZero `arm64` APPLaunch package natively or with the GNU
AArch64 cross-toolchain:

```bash
./packaging/deb/package_deb.sh
```

The generated package is written to `dist/`:

```text
dist/m5cardputerzero-ir-chat_<version>_m5stack1_arm64.deb
```
