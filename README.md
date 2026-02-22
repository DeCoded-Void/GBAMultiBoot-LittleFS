# GBAMultiBoot-LittleFS

Upload a file from LittleFS to a Game Boy Advance using the **MultiBoot (Single Game Pak)** protocol in **Normal mode (32-bit)**.

This arduino library implements the master-side portions of the protocol described in GBATEK, then performs the encrypted payload + CRC exchange as the BIOS handler expects. 

Tested with the Raspberry Pi Pico and RP2040 board variants (ex: Waveshare RP2040 Zero).

Protocol Reference: [GBATEK “BIOS Multi Boot (Single Game Pak)”](https://problemkaputt.de/gbatek-bios-multi-boot-single-game-pak.htm). 

---

## LittleFS setup

### Arduino IDE 2.x

1. Install [earlephilhower/arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
2. Ensure you have the correct board and COM port selected in the `Tools > Board` and `Tools > Port` menus.
3. Create a `data/` folder next to your sketch.
4. Put your multiboot image as `data/rom.mb` (or any name you want).
5. Close the `Serial Monitor` and any other programs accessing the Microcontroller's Serial.
6. Press `Ctrl+Shift+P` (Windows) or `Cmd+Shift+P` (macOS) to open the Command Palette. Search for and select the "`Upload LittleFS to Pico/ESP8266/ESP32`" command.

### Arduino IDE 1.x

Use [lorol/arduino-esp32littlefs-plugin](https://github.com/lorol/arduino-esp32littlefs-plugin) (For ESP8266/ESP32) or [earlephilhower/arduino-pico-littlefs-plugin](https://github.com/earlephilhower/arduino-pico-littlefs-plugin/) (For Pico/Pico 2) and follow its procedure for file upload.

#### Note: The LittleFS arduino library is already included in the [Arduino-Pico core](https://github.com/earlephilhower/arduino-pico), you will still need a method to upload files for LittleFS (as already mentioned in this section).

---

## Configuration

### Pins
- `sc` = GBA Serial Clock (MCU output)
- `si` = GBA Slave In (MCU output)
- `so` = GBA Slave Out (MCU input, **pull-up is recommended**)
- `sd` = GBA Serial Data (Tri-stated; Set to force MCU input (for hi-z), or `-1` if disconnected/floating)

**Note**: The MCU is acting as the Master in the protocol.

### Timing
- `bit_delay_us` controls the clock bit time (lower = faster, less stable).
  - Default = `1`
- `word_gap_us` inserts a gap between 32-bit transfers (bigger = more stable).
  - Default = `300`
- `delay_16th_ms` is used for all “wait 1/16 second” delays.
  - Default = `1000u / 16u`
- `words_per_update` controls how much work `update()` performs per call.
  - Default = `16u`
- `yield_every_bytes`/`yield_delay_ms` are optional cooperative yields during payload.
  - Default = `0x1000u` / Default = `1u`

### Options
- `slave_bit`:
  - `2` for slave #1, `4` for slave #2, `8` for slave #3
  - `0` = auto-detect from the `720x` response from GBA during init
  - Default = `2`
- `palette_data` (`pp`) 
  - Default = `0xD1`
- `so_pullup` enables `INPUT_PULLUP` for internal pull-up on `SO`
  - Default = `true`
- `auto_mount_fs` calls `LittleFS.begin()` inside `begin()`
  - Default = `true`

---

## Quick start (Blocking)

```cpp
#define DEBUG_PRINTS 0
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

void setup() {
  mb.setPins({3, 4, 5, -1});     // sc, so, si, sd

  GBAMultiBoot::Timing t;
  t.bit_delay_us = 1;
  t.word_gap_us  = 300;
  mb.setTiming(t);

  GBAMultiBoot::Options o;
  o.slave_bit = 2;               // 2/4/8 or 0=auto
  mb.setOptions(o);

  mb.begin();
  (void)mb.upload("/rom.mb");    // blocks until success/failure
}

void loop() {}
```

---

## Non-blocking  + Progress Checking

```cpp
#define DEBUG_PRINTS 0
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

static void onProgress(uint8_t percent, GBAMultiBoot::State, void*) {
  // update LED, UI, etc
  (void)percent;
}

static void whileRunning(void*) {
  // keep other systems alive
}

void setup() {
  mb.setPins({3, 4, 5, -1});
  mb.setProgressCallback(onProgress, nullptr);
  mb.setRunningCallback(whileRunning, nullptr);

  mb.begin();
  (void)mb.start("/rom.mb");
}

void loop() {
  if (mb.isRunning()) (void)mb.update();
}
```

---

## Debug prints

The library does **no prints by default**.

To get debug output, you must:
1. Start Serial (or any other `Print` stream) in the sketch
2. Call `mb.setDebugStream(&Serial);`

`DEBUG_PRINTS` is an optional **sketch-side** switch. This avoids needing Serial for debugging if you have alternative means of printing out debug info for this library.

Example:

```cpp
#define DEBUG_PRINTS 1
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

void setup() {
#if DEBUG_PRINTS
  Serial.begin(115200);
  while (!Serial) {}
  mb.setDebugStream(&Serial);
#endif

  mb.setPins({3, 4, 5, -1});
  mb.begin();
  (void)mb.upload("/rom.mb");
}
```

With the debug prints set to `1`, you’ll see:
- state transitions (`wait_enter_mode`, `wait_720x`, `header`, `payload`, etc.)
- failures with `result`, current `state`, plus `last_tx` and `last_rx`

---


## Example debug output

If you set a debug stream (see “Debug prints”), you’ll see state transitions and failures.

Example (with a single slave):

```
[mb] state: wait_enter_mode
[mb] state: wait_720x
[mb] state: exchange_info_1
[mb] state: header
[mb] state: header_done
[mb] state: exchange_info_2
[mb] state: palette_wait
[mb] state: handshake
[mb] state: delay_16th
[mb] state: len_info
[mb] state: payload
[mb] state: checksum_finalize
[mb] state: crc_request
[mb] state: crc_wait
[mb] state: crc_signal
[mb] state: crc_exchange
[mb] state: done
```

If something fails, you’ll get info such as:

```
[mb] fail: protocol_mismatch (state header)
[mb] last_tx: 0x00006200
[mb] last_rx: 0x72020000
[mb] expected_nn: 0x60
[mb] got_nn: 0x00
```

Use `mb.state()` + `mb.lastTx()`/`mb.lastRx()` to decide whether to retry, slow down timing, or check wiring.

---

## Results and states

### Result
- `ok` – Upload finished and CRC matched
- `running` – Only during async/non-blocking operation
- `fs_begin_failed` – LittleFS could not mount
- `file_open_failed` – File not found / couldn’t open
- `file_size_invalid` – Header/payload size constraints violated
- `timeout` – A “wait” stage ran too long (init / palette / crc wait)
- `protocol_mismatch` – Unexpected response from the GBA at some step
- `aborted` – User called `abort()`

### State
`state()` tells you which step you were in during the Multiboot Transfer Protocol when something failed.  
Use `stateName(mb.state())` for logging.

---

## Handling failures and retrying

The library does GBATEK's documented behavior:

- “Try 15 times to Send 6200 and Receive 720x, if failed: delay 1/16s and restart”

If the upload still fails (timeout, protocol mismatch, etc.), you can retry at a higher level:

```cpp
#define DEBUG_PRINTS 1
#include <GBAMultiBoot.h>

GBAMultiBoot mb;

void setup() {
#if DEBUG_PRINTS
  Serial.begin(115200);
  while (!Serial) {}
  mb.setDebugStream(&Serial);
#endif

  mb.setPins({3, 4, 5, -1});

  GBAMultiBoot::Timing t;
  t.bit_delay_us = 1;
  t.word_gap_us = 300;
  mb.setTiming(t);

  mb.begin();

  for (int attempt = 1; attempt <= 10; ++attempt) {
#if DEBUG_PRINTS
    Serial.print("[mb] attempt "); Serial.println(attempt);
#endif

    const GBAMultiBoot::Result r = mb.upload("/rom.mb");
    if (r == GBAMultiBoot::Result::kOk) {
#if DEBUG_PRINTS
      Serial.println("[mb] success");
#endif
      break;
    }

#if DEBUG_PRINTS
    Serial.print("[mb] fail: ");
    Serial.print(GBAMultiBoot::resultName(r));
    Serial.print(" (state ");
    Serial.print(GBAMultiBoot::stateName(mb.state()));
    Serial.println(")");

    Serial.print("[mb] last_tx=0x"); Serial.println(mb.lastTx(), HEX);
    Serial.print("[mb] last_rx=0x"); Serial.println(mb.lastRx(), HEX);

    delay(1000 / 16);
#else
    delay(1000 / 16);
#endif
  }
}

void loop() {}
```

---

## Troubleshooting

### `fs_begin_failed`
- Make sure you use the [LittleFS uploader plugin](https://github.com/earlephilhower/arduino-littlefs-upload) and uploaded a filesystem image.

### `file_open_failed`
- In LittleFS paths typically start with `/` (example: `"/rom.mb"`).
- Verify that the file exists in the uploaded filesystem (name/case must match).

### `file_size_invalid`
This is triggered based on constraints documented in GBATEK:
- Total length must be >= `0xC0`
- Total length must be <= `0x40000`
- Payload length (`len - 0xC0`) must be:
  - Multiple of `0x10`
  - Minimum `0x100`
  - Maximum `0x3FF40` 
- Multiboot programs are limited to **256kb**!

### `timeout`
- The GBA isn’t actually in the BIOS/Download screen.
- Wiring, check for:
  - Wrong pin config: You may need to swap the `si` and `so` pins.
  - Common Ground: The GBA and MCU need the same Ground.
  - Pull-up: `SO` needs to be manually pulled up if the board doesn't support it (SO is Idle high!).
- Bit rate may be too fast or unstable: increase `bit_delay_us` and/or `word_gap_us`.
- Verify with a logic analyzer if needed.

### `protocol_mismatch`
Means that there is an unexpected response.
- Check if the voltage levels are 3.3V-safe.
  - If the GBA is in GB/GBC mode, the voltage levels are 5v.
- Check that the GBA and MCU share the same Ground.
- Ensure `SD` is not being driven by the MCU.
- Increase `word_gap_us` (try 500–2000us) and/or `bit_delay_us` (try 2–5us).
- Enable debug prints and verify the reported `state`, `last_tx`, `last_rx`.
- Verify with a logic analyzer if needed.

---

## Credits and Special Thanks

### (in no particular order - a lot more beyond just this!)

- **earlephilhower** & [**Arduino-Pico Library Maintainers**](https://github.com/earlephilhower/arduino-pico) 
- **Martin Korth** - GBATEK writer
- [**Various Contributors** outlined in GBATEK's about section](https://www.problemkaputt.de/gbatek.htm#aboutthisdocument)
- **Endrift** - [mGBA Emulator](https://github.com/mgba-emu/mgba) Author
- **Shonumi** - [GBE+ Emulator Author](https://github.com/shonumi/gbe-plus) & [GBA Accessory Blogger](https://shonumi.github.io/)
- **GreigaMaster** - [Chip Gate Homebrew](https://forums.therockmanexezone.com/battle-chip-gate-homebrew-t16537.html) author (used as the test ROM for this project)
- **jojolebarjos** - [gba-multiboot repo](https://github.com/jojolebarjos/gba-multiboot) Author - their repo was good reference for me to know if I was on the right track
- **MegaMan Battle Network!** - [Collector Community Discord](https://discord.gg/Wa98sZza4g) - Inspiration for me to make this since I didn't find a repo that suited my partiular needs
