# Working notes for this repo

Things that cost time once and should not cost it twice. The README is for
someone using the pattern; this is for whoever is editing it.

@CLAUDE.local.md

(Optional private notes, gitignored - machine-specific paths and the leak-sweep
term list. A clone without the file just skips the line.)

## Publishing rules - these are hard

- **Private repo. Keep it generic.** This reads as "how to do OTA on a GIGA
  R1 with an LVGL UI", written by someone with one spare board. No references
  to any other project, what any board controls, where anything is installed,
  no SSIDs, no real IPs, no MQTT topics.
- **Before every commit**, sweep for leaks. A real LAN address slipped into a
  README log excerpt once and had to be scrubbed after pushing. The sweep is a
  `grep` over `*.md`, `*.ino`, `*.py` and `*.h`; **the term list lives in
  `CLAUDE.local.md`**, imported above, because writing the things you must not
  leak into a tracked file leaks them.
- `ota_private_key.pem` and `arduino_secrets.h` are gitignored and must stay
  that way. Confirm with `git status --porcelain` before committing, and
  `git log --all --name-only --pretty=format: | grep -i pem` to confirm the
  key has never entered history.

## Build and flash

```bash
arduino-cli compile --fqbn arduino:mbed_giga:giga --build-path <dir> .
arduino-cli upload  --fqbn arduino:mbed_giga:giga -p COMx --input-dir <dir> .
```

- **The OTA payload is `Giga_OTA.ino.bin`, NOT `Giga_OTA.ino.with_bootloader.bin`.**
- Building a payload means bumping `FW_VERSION`, compiling to a *separate*
  build dir, then putting the source version back. The repo's committed
  version is the documented starting point, not whatever was last served.
- The PC tools need `pyserial` and `cryptography`. A board re-enumerates on a
  new port after a DFU upload, so re-check the port rather than assuming it.

## Gotchas that have already bitten

- **Only one `ota.begin()` per boot.** Its `init()` does
  `new mbed::FATFileSystem("fs")` and mounts it, and nothing unmounts. A
  second attempt in the same session fails with `OtaStorageInit (-3)`. The
  sketch keeps one static object and a `storage_ready` flag. Do not
  "tidy" that into a per-attempt local.
- **The serial boot banner is nearly impossible to catch.** The board
  re-enumerates after a DFU upload, so anything printed in the first couple of
  seconds is usually lost. Put state on the screen and read it with
  `dump_screen.py` instead of racing the port.
- **`setStatus()` forces a full repaint; `setStatusQuiet()` does not.** Use
  the quiet one inside loops that already call `lv_timer_handler()`, or a
  700 kB download gets slower than the network.
- **Flipping `OTA_IS_HTTPS` to true costs ~148 kB**, because it is a
  compile-time constant and the TLS stack is otherwise dead-code-eliminated
  (429 mbedTLS symbols drop to 4). A sudden large size change is usually this,
  not a regression - check with `nm | grep -c mbedtls` before worrying.

## Testing it properly

- `sim_tap.py 400 330 COMx` presses Update. Coordinates are **logical**
  800x480; `simTouchReadCb()` does the rotation inversion.
- **Wait for the join before tapping.** `serviceDevCommands()` runs during the
  connect retry loop, so a tap sent too early queues and then fails with a
  network error that looks like a server problem.
- Proving the signature check needs **both directions in one boot** - unsigned
  rejected, then signed accepted. Doing them in separate boots hides the
  `begin()` bug above, which is exactly how it was missed the first time.
- `serve_broken.py <file> truncate|stall` is the recovery rehearsal. Pass
  condition: the board stays on its existing firmware, says what happened, and
  **takes the next good update** without a reboot.

## Claims to be careful about

- `ota.update()` returning `Error::None` means *recorded*, not *applied*. Only
  the bootloader's `I-Sketch updated` and a changed version on screen say it
  worked.
- **Do not use ping to judge WiFi health on these boards.** A busy main loop
  delays low-priority ICMP replies badly enough to drown the signal - a board
  answering HTTP in 40 ms has measured 1500 ms pings. Use a cold HTTP request,
  or read `whd_wifi_get_powersave_mode()` back for the sleep question.
- The Content-Length and stall-timeout checks protect against **accidents**.
  They are not security; only the signature is. Do not let that blur in the
  README.
