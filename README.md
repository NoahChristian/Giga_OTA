# Giga_OTA

A minimal over-the-air firmware update on an **Arduino GIGA R1 WiFi**, with an
LVGL screen so the result is visible rather than inferred.

The whole demonstration is one number. Flash the sketch once over USB, push a
new build over the air, and watch the version label on the display change. No
counters, no log scraping, no "it probably worked".

```
+--------------------------------------------+
|        GIGA R1 over-the-air update         |
|                                            |
|                 FW 1.0.0                   |
|         WiFi: 10.0.0.42   PS mode 0        |
|      QSPI: 4 partitions, OTA 5120 kB       |
|                                            |
|                [ Update ]                  |
|                                            |
|   Ready. Tap Update to fetch a new image.  |
+--------------------------------------------+
```

Status: **work in progress.** See [Where this has got to](#where-this-has-got-to).

---

## Why this repo exists

Searching for GIGA R1 OTA turns up a stale forum answer saying the GIGA is not
supported, a tutorial written for the Portenta H7, and a packaging toolchain
that does not run on Windows. All three are misleading, and working out why
took a day. This repo is the write-up, with a working sketch attached.

The three things worth knowing up front:

1. **The GIGA is supported.** First-class, not by accident.
2. **The bootloader already on your board can do OTA.** Nothing to replace.
3. **The documented packaging tools do not run on Windows**, and you probably
   do not need them anyway.

---

## 1. The bootloader already supports OTA

This is the question that decides whether any of this is possible, and you can
answer it without running anything. The bootloader shipped with the core
(`packages/arduino/hardware/mbed_giga/<ver>/bootloaders/GIGA/bootloader.elf`)
contains `getOTABinaryBase()`, a full FATFS, `QSPIFBlockDevice` and
`FlashIAPBlockDevice`. Its own log strings describe the entire mechanism:

```
I-Start OTA 0x%X 0x%X 0x%X
I-Default OTA data  /  I-Custom OTA data
I-Sketch updated
I-Booting sketch at 0x%x
E-Failed to locate firmware image, error: %d
/fs/UPDATE.BIN   /fs/UPDATE.BIN.LZSS   /fs/scratch.bin
```

So the sequence is: something puts a file at `/fs/UPDATE.BIN` on the QSPI
flash, something records that an update is pending, and on the next boot the
bootloader applies it and prints `I-Sketch updated`.

That last string is the only trustworthy confirmation an update happened.
`ota.update()` returning `Error::None` means the update was *recorded*, not
that it was *applied*.

### Memory map

From the DFU descriptors in that same binary:

| Region | Address | Size |
|---|---|---|
| Bootloader | `0x08000000` | 256 KB |
| Sketch | `0x08040000` | rest of 2 MB internal flash |
| QSPI, filesystem view | `0xA0000000` | 16 MB |
| QSPI, raw view | `0x90000000` | 16 MB |

### QSPI partitions

`/fs/UPDATE.BIN` lives on a FAT filesystem on **MBR partition 2**. The layout
the bootloader expects is the one the core's own `QSPIFormat` example creates:

| Partition | Purpose | Size | Mount |
|---|---|---|---|
| 1 | WiFi firmware and certificates (`cacert.pem`) | 1 MB | `wlan` |
| 2 | **OTA** | 5 MB | **`fs`** |
| 3 | Provisioning KVStore | 1 MB | — |
| 4 | User data | 7 MB | `user` |

**If the QSPI has never been partitioned, OTA cannot work**, and the failure is
opaque. This sketch reads the partition table at boot and puts the answer on
the screen so you are not guessing. If it says the flash is not partitioned,
run `File > Examples > STM32H747_System > QSPIFormat` once.

> **The LBA fields are not in 512-byte sectors.** If you read the MBR
> yourself — and you should, since "is the OTA partition big enough for my
> image" is the whole point — note that `MBRBlockDevice::partition()` takes
> byte addresses (`bd_addr_t`) and stores them divided by
> `max(erase_size, 512)`, which is **4096** on this QSPI. Assume the usual 512
> and every partition reads 8x too small: the 5 MB OTA partition shows up as
> 640 kB, which looks exactly like a board that needs reformatting. Use
> `bd->get_erase_size()` rather than a constant.
>
> A quick sanity check if you are unsure: the four partitions should come out
> in a 1:5:1:7 ratio whatever unit you divide by. If the ratio is right and
> the absolute numbers look small, it is the unit, not the flash.

> **That format erases the QSPI**, including stored WiFi credentials in
> partition 3 and anything in partition 4. On a board in service, know what is
> on there before you run it.

---

## 2. The GIGA is a supported target

The library to use is **`Arduino_Portenta_OTA`** (Library Manager, 1.2.1 at the
time of writing). Its name says Portenta and its README only discusses the H7,
which is why people conclude the GIGA is unsupported. It is not:

- `library.properties` lists `mbed_giga` among its architectures.
- Its own `examples/OTA_Qspi_Flash/OTA_Qspi_Flash.ino` has an
  `#elif defined(ARDUINO_GIGA)` arm pointing at a prebuilt GIGA image.
- Arduino publish that image at
  `https://downloads.arduino.cc/ota/OTA_Usage_Portenta.ino.GIGA.ota`.
- `bin2ota.py` lists `GIGA` with its own magic number, `0x23410266`.

The prebuilt image is genuinely useful: it lets you prove the mechanism works
on your board before you have built anything yourself, which separates "is OTA
possible here" from "is my image correct".

### The API

Six calls, in this order:

```cpp
Arduino_Portenta_OTA_QSPI ota(QSPI_FLASH_FATFS_MBR, 2);   // partition 2

ota.isOtaCapable();   // bootloader new enough?
ota.begin();          // mount the OTA filesystem
ota.download(url, is_https);   // -> /fs/UPDATE.BIN.LZSS
ota.decompress();              // -> /fs/UPDATE.BIN
ota.update();                  // record it for the bootloader
ota.reset();                   // reboot; the bootloader applies it
```

`isOtaCapable()` returning false means the bootloader is older than the library
wants. Update it with
`File > Examples > STM32H747_System > STM32H747_manageBootloader`.

**There is no progress callback.** `download()` blocks until it finishes and
reports only a byte count. A progress bar on this API would either never move
or be a fiction, so this sketch shows stage transitions and a final byte count
instead. If you want real progress, `downloadAndDecompress()` tracks it
internally — but it does not surface it either, so you would be modifying the
library.

---

## 3. The packaging problem, and why you can probably skip it

The documented way to build an image is Arduino's `lzss.py` and `bin2ota.py`
from `ArduinoIoTCloud/extras/tools`:

```bash
./lzss.py --encode SKETCH.bin SKETCH.lzss
./bin2ota.py GIGA SKETCH.lzss SKETCH.ota
```

`lzss.py` is a ctypes shim:

```python
LZSS_SO_EXT = "so" if platform.uname()[0] != "Darwin" else "dylib"
lzss_functions = ctypes.CDLL(f"./lzss.{LZSS_SO_EXT}")
```

There is no `lzss.dll`. **On Windows the documented path does not run**, and
building the `.so` from `lzss.c` needs a host C compiler you may not have.

### What the container actually is

20 bytes of header, then the payload (`src/decompress/utility.cpp`):

```c
uint32_t len;            // bytes of magic + version + payload
uint32_t crc32;          // over magic + version + payload
uint32_t magic_number;   // 0x23410266 for the GIGA
uint64_t version;        // bitfield: header_version:6, compression:1,
                         // signature:1, spare:4, payload_target:4,
                         // payload_major:8, payload_minor:8,
                         // payload_patch:8, payload_build_num:24
```

`tools/mkota.py` in this repo builds and inspects that container in pure
Python — `zlib.crc32`, no ctypes, no native library, no `crccheck` dependency.
It runs anywhere Python does.

### The compression flag is not what it looks like

`bin2ota.py` writes its version field as `bytearray([0]*7 + [0x40])` with the
comment *"all 0 except the compression flag set"*. But `compression` is bit 6
of the first 32-bit word, which little-endian puts in **byte 0**, not byte 7.
Byte 7 lands inside `payload_build_num`.

You can check this against Arduino's own shipped artifact:

```
$ python tools/mkota.py info OTA_Usage_Portenta.ino.GIGA.ota
  len field   86894  (body is 86894)
  crc32       0x7B783DB8
  magic       0x23410266  GIGA
  version     00 00 00 00 00 00 00 40
  decoded     header_version=0 compression=0 signature=0 target=0  v0.0.0 build 4194304
```

That file **is** LZSS-compressed, and the compression bit as the struct defines
it reads **0**. So the device cannot be consulting that bit — `decompress()`
decompresses unconditionally, and the flag is decorative.

**Which means the simplest thing works:** if you do not compress, do not call
`decompress()`. The payload goes to `/fs/UPDATE.BIN.LZSS` and simply needs to
end up at `/fs/UPDATE.BIN`. There is corroboration for the bootloader being
relaxed about this — Arduino's *Zephyr* core ships an OTA library whose README
says its images carry no OTA header at all, because "the mbed bootloader
validates the file by checking that the first word is a valid ARM initial stack
pointer; an OTA header would fail that check."

`mkota.py` warns if the first word of your `.bin` is not a plausible stack
pointer, which catches the commonest mistake — packaging the wrong file.

---

## 4. Verifying it from the PC, not by eye

An update mechanism you have to walk over and look at is only half useful. Two
serial commands make the whole cycle scriptable:

| Command | Does | PC-side tool |
|---|---|---|
| `D` | Dumps the raw RGB565 framebuffer | `tools/dump_screen.py` → a real PNG |
| `P` + `xxxyyy` | Injects a tap at that logical point | `tools/sim_tap.py` |

```bash
python tools/sim_tap.py 400 330        # press Update
# ... board downloads, reboots ...
python tools/dump_screen.py COM12 after_update
# captures/after_update_<timestamp>.png now shows FW 1.0.1
```

`dump_screen.py` needs only `pyserial` and `zlib` — no Pillow. It un-rotates
the framebuffer on the PC side, because the Display Shield's panel is
physically portrait: declaring `Display(800, 480)` makes the library create a
480x800 LVGL display with `ROTATION_270`, so the raw framebuffer comes back
portrait-native.

`sim_tap.py` drives a **second LVGL pointer indev**, so a simulated tap goes
through real hit-testing and click bubbling — the same path a finger takes,
not a shortcut that calls the callback directly.

> **The one trap.** LVGL applies `lv_display_rotate_point()` to every indev
> read. The real touch controller reports raw panel coordinates, so it works
> for free. Feeding *logical* coordinates to your own indev makes LVGL rotate
> them a second time and taps land nowhere. `simTouchReadCb()` inverts
> `ROTATION_270` by hand (`raw_x = 479 - logical_y`, `raw_y = logical_x`) so
> the serial protocol stays in the logical 800x480 space everything else uses.

---

## 5. The WiFi radio sleeps, and the Arduino API cannot stop it

Not strictly an OTA topic, but it will bite you during a multi-hundred-kilobyte
download, so the fix is in the sketch.

`whd_wifi_on()` in the core enables PM2 powersave unconditionally on every
radio power-up. From the shipped `libmbed.a` (`whd_management.o`):

```
bl    whd_wifi_set_up
movs  r1, #200                                 ; return_to_sleep_delay
bl    whd_wifi_enable_powersave_with_throughput
```

After that the radio wakes only on DTIM beacons. Outbound traffic is
unaffected — which is why a board that publishes happily can still be hard to
reach, and why this is easy to misdiagnose as a router problem.

`mbed_giga`'s `WiFi` class exposes no setting for it; there is no
`WiFi.setSleep()` the way there is on the ESP32 core. But `WiFi.h` already
pulls in `whd_wifi_api.h`, so the WHD calls link straight from a sketch:

```cpp
extern "C" { whd_driver_t cybsp_get_wifi_driver(void); }

whd_interface_t ifp = whd_get_primary_interface(cybsp_get_wifi_driver());
whd_wifi_disable_powersave(ifp);
whd_wifi_get_powersave_mode(ifp, &mode);   // read it back: 0 = off
```

Call it **after** the join — `whd_wifi_on()` re-arms PM2 every time the radio
is powered up, so anything that cycles the radio undoes it. If your sketch
disconnects to scan, re-apply it on every reconnect, not once at boot.

The read-back matters. `whd_wifi_get_powersave_mode()` makes the chip state its
own mode (`2` → `0`), which is a fact rather than a hope. Do not try to confirm
this with ping times — a busy main loop delays low-priority ICMP replies badly
enough to drown the signal you are looking for.

---

## 6. What about Zephyr?

The mbed cores are end-of-life, and `ArduinoCore-zephyr` 1.0.0 lists the GIGA
R1 WiFi among its supported boards. So should you wait?

**Not if you need a screen.** That core ships no display library and no LVGL —
its whole library set is `Arduino_LED_Matrix`, `CAN`, `Camera`, `Ethernet`,
`OTA`, `PDM`, `RTC`, `SPI`, `SocketWrapper`, `Storage`, `WiFi`, `Wire`,
`Zephyr_SDRAM`, `ea_malloc`, `stubs`. There is no `Arduino_H7_Video`, no
`Arduino_GigaDisplayTouch`. The only trace of the Display Shield is a
devicetree binding for its DSI bridge, `analogix,anx7625.yaml` — the chip is
described, but nothing drives it from a sketch.

**Most of this transfers anyway.** Zephyr's own OTA library writes to the OTA
partition on QSPI and lets *the mbed bootloader* apply it; it requires running
a `FlashFormat` that creates "the MBR partition table and FAT filesystems on
QSPI that both the Zephyr loader and the mbed bootloader depend on", and it
serves the image over plain HTTP. Same bootloader, same partitions, same
hosting. What changes is the sketch-side API and the image format.

**And the bootloader is ready for whatever comes after.** Alongside the Arduino
OTA path it carries a complete MCUboot — slot swap, image validation,
encrypted images:

```
I-Starting MCUboot
I-Image upgrade secondary slot -> primary slot
I-Swap type: %s
MCUBoot_ECIES_v1
```

`@Arduino boot v.%02d` and `@MCUboot v.%02d` both appear as DFU alt-settings,
so the board can run either bootloader personality. The hardware will not be
what blocks you.

---

## Getting started

```
Giga_OTA.ino               the sketch: LVGL UI + OTA, one file
arduino_secrets.h.example  copy to arduino_secrets.h and fill in
tools/serve.py             static HTTP server; prints the URL to paste
tools/mkota.py             build/inspect .ota containers, pure Python
tools/dump_screen.py       pull a PNG of the screen over serial
tools/sim_tap.py           press a button from the PC
```

Requires the `Arduino_Portenta_OTA` library, and `pyserial` for the two serial
tools.

1. Copy `arduino_secrets.h.example` to `arduino_secrets.h`; fill in SSID,
   password and the update URL.
2. Build and flash over USB. Check the QSPI line on screen; run `QSPIFormat`
   if it says the flash is not partitioned.
3. Prove the mechanism with Arduino's prebuilt image before your own:
   ```bash
   curl -LO https://downloads.arduino.cc/ota/OTA_Usage_Portenta.ino.GIGA.ota
   python tools/serve.py OTA_Usage_Portenta.ino.GIGA.ota
   ```
   Tap Update. Success is `I-Sketch updated` on the console and Arduino's demo
   sketch running. Re-flash this sketch over USB afterwards.
4. Then your own: bump `FW_VERSION`, build, and serve the `.bin`.

---

## Where this has got to

Honest status, because a repo that overstates itself is worse than no repo.

- [x] Bootloader OTA support confirmed by inspection
- [x] QSPI partition scheme identified
- [x] GIGA confirmed as a supported library target
- [x] `.ota` container format decoded
- [x] `mkota.py` validated against Arduino's official GIGA image
- [x] Compression flag behaviour determined from that image
- [x] Sketch builds, runs, and renders; screen capture over serial working
- [x] QSPI confirmed partitioned on a real board — 4 partitions, OTA
      partition 2 is 5120 kB, so no reformat needed
- [x] WHD powersave disable confirmed `2 -> 0` (on a sibling board; not yet
      re-confirmed on this one)
- [ ] WiFi join on this board — waiting on an antenna
- [ ] OTA proven end to end with Arduino's prebuilt image
- [ ] OTA proven with a self-built image
- [ ] HTTPS path proven

If the remaining boxes stay unticked, this README explains how far it got and
why, which is the useful half either way.

## Licence

MIT.
