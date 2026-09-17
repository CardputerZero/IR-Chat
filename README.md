# CardputerZero IR-Chat

Runtime-only infrared chat for M5Stack CardputerZero.

## Features

- Send and receive messages with a compact custom 38 kHz raw IR protocol
- Keep up to 64 messages in memory for the current session only
- Accept messages up to 7 printable ASCII bytes
- Discover receiver and transmitter nodes by LIRC capability instead of device number
- Use a loopback mock backend for SDL desktop development

IR requires line of sight. This protocol is specific to IR Chat and is not an
infrared remote-control protocol.

Messages use the original v1 single-frame protocol. The editor and transport
share a 7-character limit (printable ASCII only). The editor caps input at
7 characters, and the transport rejects oversized payloads.

The candidate limit keeps a frame at 243 pulse/space timings or fewer. Its
conservative airtime bound is 243.38 ms, versus 496.35 ms for the previous
23-character limit. Eight characters have a conservative bound of 256.86 ms,
so seven is the largest length below the chosen 250 ms budget. This budget is
an engineering target, not a measured device limit or a guarantee of reception.
The original 23-digit failure has not been reproduced on physical devices.

For hardware regression testing, install this candidate on both devices:

- Keep distance and alignment fixed and record both devices' software/kernel
  versions and selected RX/TX drivers.
- Send `1`, `12345`, `1234567`, `0000000`, and `~~~~~~~` 100 times each in each
  direction, waiting for each transmission to finish before the next one.
- Record sender errors, receiver missing/corrupted/duplicate messages and
  recovery on the next send. Distinguish a failed local write from a completed
  write whose message did not arrive. Retain the application logs from both
  devices, including sequence, event count, airtime and any errno/CRC errors.
- Verify that the editor prevents an eighth character and that a failed or
  obstructed transmission does not prevent subsequent unobstructed messages.

Treat any failure as requiring investigation before accepting the candidate.
Successful local transmission does not acknowledge reception by the peer.
Offline codec tests cannot establish optical reliability.

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
