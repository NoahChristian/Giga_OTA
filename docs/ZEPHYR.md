# The Zephyr path

README section 7 answers "what about Zephyr?" with *not yet, if you need a
screen*. **That answer is now out of date**, and this page is the current one.

Arm declared Mbed OS end-of-life in July 2026 and Arduino has begun deprecating
the mbedOS-based cores, so this is not an optional direction. The good news is
that most of what this repo documents survives the move; the part that changes
is the part worth changing.

Confidence tags as elsewhere in `docs/`:

| tag | means |
|---|---|
| **measured here** | observed on this sketch, on a GIGA R1 |
| **measured, other board** | observed on this bench on a board running native Zephyr with MCUboot - a second GIGA R1, or a Portenta Machine Control |
| **read** | read out of source or a datasheet and not yet made to happen |

---

## 1. What changed since section 7 was written

**Native Zephyr on a GIGA R1 with the Display Shield works**, display, touch
and all. **Measured, other board**: a seven-screen LVGL application with WiFi,
a watchdog and TLS, running on the shield, on this bench.

The claim that "that core ships no display library and no LVGL" was about the
**Arduino Zephyr core** (`arduino:zephyr_main`, which keeps the `.ino` form).
It was never true of **Zephyr itself**, where LVGL is in-tree and the Display
Shield has an upstream board definition - `-DSHIELD=arduino_giga_display_shield`
supplies the panel, the GT911 touch controller and the SDRAM framebuffer, and
Arduino SA contributed it. That is the distinction section 7 missed, and it is
the whole difference between "wait" and "go".

So there are two Zephyr routes, and they are not equally capable:

| route | form | verdict |
|---|---|---|
| Arduino Zephyr core | `.ino` sketches as LLEXT extensions over a fixed prebuilt loader | Keeps the sketch workflow. Cannot change devicetree or Kconfig without rebuilding the loader, which is not supported natively on Windows. LVGL compiles against a 256 kB sketch heap. |
| **Native Zephyr** | a freestanding Zephyr application built with `west` | Devicetree, Kconfig and the flash map are yours. This is the route that supports a real update story. |

The flash partition alone decides it: an OTA layout *is* a devicetree change.

---

## 2. Two ways to do OTA under Zephyr

### (a) Keep the Arduino bootloader

Everything in README sections 1-3 still applies. Same bootloader, same MBR
partition layout, same `/fs/UPDATE.BIN`, same hosting. A Zephyr application
writes the same file into the same QSPI OTA partition and lets the Arduino
bootloader apply it on the next boot. The sketch-side API and the packaging
change; the mechanism does not.

This is the cheap move, and it inherits the limitation from
[`RELIABILITY.md`](RELIABILITY.md) section 1: one slot, no revert.

### (b) MCUboot, dual-slot, with automatic revert

**Measured, other board** - proven end to end over WiFi: upload into the
secondary slot, mark test, reset, swap 1.79 MB through a scratch area, boot the
new image, confirm on reaching the network. A bad image costs one reboot.

This is the one worth doing, and the rest of this page is about it. Read
[`RELIABILITY.md`](RELIABILITY.md) section 2 first for *why*, and section 3 for
the failure mode that bites afterwards.

---

## 3. A layout that fits a GIGA

**Measured, other board:**

| region | address | size | holds |
|---|---|---|---|
| `boot_partition` | `0x08000000` | 256 kB | MCUboot - 37,420 bytes measured |
| `slot0` | `0x08040000` | 1792 kB | the running application - 1,296,999 bytes signed |
| `slot1` | QSPI `0x100000` | 1792 kB | the staged update |

The secondary slot is **external** because two 1792 kB slots do not fit in 2 MB
of internal flash. It takes over the QSPI region that README section 1 describes
as the 5 MB `ota` partition - same flash, different owner. The Arduino
bootloader's updater has no user once MCUboot is in place.

Keep the slot addresses in **one** file included by both the bootloader build
and the application build. A disagreement between the two fails *silently*.

### The partition must be widened or nothing fits

**Measured, other board.** The upstream board definition describes only flash
bank 1 and splits it into a 256 kB bootloader partition and a 768 kB `slot0`.
That is not enough:

| | |
|---|---|
| LVGL display sample, alone | 436 kB |
| AIROC WiFi shell, alone | 749 kB |
| the two together plus MQTT | overflows 768 kB by 216 kB |

An overlay describing the real 2 MB, giving `slot0` 1792 kB of it, is required
rather than an optimization.

### The STM32H7 bank 2 trap, which is nasty

**Measured, other board**, on the same STM32H747 this repo runs on.

On a dual-core H7, Zephyr's flash driver **doubles** the `flash0` size it reads
from devicetree, because the standard device tree declares 1 MB for a 2 MB part.
Describe the real 2 MB in your overlay - which section above says you must - and
the driver computes 4 MB, which no part in the series has. The bank boundary
moves, **every address in bank 2 is classified as bank 1**, and writes to bank 2
go out through the wrong control register and raise `PGSERR`.

Erase of the same sector still succeeds, so the failure looks nothing like a
bank-selection problem. There is no build-time check that the computed size is
even possible.

This one costs an afternoon if you do not know it, and the symptom - staging
works, writing fails, partway up the slot - looks exactly like bad flash.

### The two ends must agree about swap mode

**Measured, other board.** The bootloader mode defaults to swap-using-offset,
which writes the uploaded image into the secondary slot shifted by one sector.
If the application was built expecting scratch-based swap and the bootloader was
not (or the reverse), the upload lands in the wrong place and the swap does
nothing useful.

The two Kconfig symbols are named differently on the two sides, which does not
help anybody. Set both deliberately and check them against each other.

---

## 4. Installing MCUboot on a GIGA, once

Only the **STM32 ROM bootloader** can write flash sector 0. The Arduino
bootloader's own DFU descriptor marks it readable but not writable
(`01*128Ka`), so it cannot overwrite itself.

**Hold BOOT0, tap RST, release.** The board enumerates as an ST DFU device
(`0483:df11`), and MCUboot goes to `0x08000000` with the signed application at
`0x08040000` in the same session.

Two things recorded so nobody repeats them:

- **A software jump into the ROM bootloader does not work on this part.** The
  backup-register and early-boot half worked; the jump itself left the board
  with no USB at all until RST. **Measured, other board.** BOOT0 + RST is one
  press and cannot fail.
- **dfu-util refuses to act with more than one DFU device visible**, and these
  boards advertise several alt settings on their own. Pass the VID:PID and the
  serial explicitly.

The ROM bootloader is in silicon. It cannot be erased or overwritten by anything
you flash, so **BOOT0 + RST always works** and nothing reachable from software
is a brick.

### The chainload alternative, for assemblies where BOOT0 is not reachable

**Measured, other board** - a Portenta Machine Control, whose carrier occupies
the high-density connector BOOT0 lives on, leaving one RESET button, no SWD
header and no JTAG header.

There, MCUboot is **chainloaded** instead: written at `0x08040000`, which is
where the Arduino bootloader already jumps, with the application above it.
MCUboot links wherever its boot partition is, so moving the partition moved the
bootloader with no source change.

That is **safer** than replacing sector 0, not a compromise: sector 0 is never
written, the Arduino bootloader survives whatever happens above it, and
double-tap RESET always reaches it again. On a GIGA you have the choice; it is
worth knowing the chainload works before assuming you need BOOT0.

### Undoing it

Write `bootloader.bin` from the installed `mbed_giga` core's `bootloaders/GIGA/`
directory back to `0x08000000` the same way, and rebuild the application without
MCUboot support so its vector table sits at the start of `slot0` again rather
than after MCUboot's 1 kB header.

---

## 5. Signing, under MCUboot

The idea transfers; the format does not.

- **ECDSA P-256, not the RSA-2048 default** - smaller signature, smaller
  verification code, and it is the same curve this repo already uses.
- **The signed image is a different file from the plain one.** Build output
  `zephyr.signed.bin` is bootable; `zephyr.bin` is not, because the vector table
  has to sit after MCUboot's 1 kB header. Flashing the plain one leaves a board
  sitting in the bootloader forever with no diagnostic. Exactly the same class
  of mistake as `Giga_OTA.ino.with_bootloader.bin` in this repo.
- **Writing an image over SWD still needs the signed one.** MCUboot validates
  the primary slot on every boot, so SWD skips the *transport*, not the
  signature check.
- This repo's `GSG1` trailer format has no place here. MCUboot carries its own
  header and trailer, and the whole point of the trailer in this repo - one
  URL, no way to pair an image with the wrong signature - is something MCUboot
  solves its own way.

Everything in [`SECURITY.md`](SECURITY.md) sections 2 and 3 - fail-closed key
configuration, one key per application, proving separation in both directions -
was learned on exactly this setup and matters more here than it does under the
Arduino bootloader, because the bootloader itself is now something you build.

---

## 6. Updating thereafter, and recovering

Application images go over the network with `mcumgr`/`smpmgr` against an SMP
server. No cable, no hands on the board, which is the point for anything
mounted on a wall.

**Make that the development reflash path too**, per
[`RELIABILITY.md`](RELIABILITY.md) section 7.

Updating **MCUboot itself** needs BOOT0 + RST, or an SWD probe - anything that
can write sector 0. That should be rare.

Three recovery notes, all **measured, other board**:

- A half-finished swap disables updates until it is cleared, and the only sign
  is a missing log line from a console-less bootloader. See
  [`RELIABILITY.md`](RELIABILITY.md) section 3, which is the most important
  cross-reference on this page.
- Give the bootloader a console over SEGGER RTT in a debug build - no wires, no
  second USB adapter. Start the RTT logger **before** the reset that matters;
  attaching it resets the board, which is how you capture a boot, but it also
  means a request staged beforehand is consumed by that very reset. Stage first,
  then attach.
- Keep a **single-slot recovery bootloader** build around for the deadlock where
  a half-finished swap stops the application booting at all - and the
  application is the only thing that can erase the external slot.

---

## 7. What else transfers, and what does not

| from this repo | under native Zephyr |
|---|---|
| QSPI partition layout and the MBR erase-size trap | Still true of the hardware. Under MCUboot the OTA region is reused as `slot1` rather than as a FAT filesystem. |
| ECDSA P-256 signing, public key on device | The idea carries over exactly. The container does not. |
| "`update()` recorded it, the bootloader applied it" | Same distinction, different names. Still the thing that misleads people. |
| Content-Length and stall guards | Replaced by the SMP transport's own framing, and still not security. |
| Screen capture and simulated taps over serial | The same technique works - a shell command that dumps the framebuffer, and one that injects a tap in logical coordinates. The double-rotation trap in the README's bonus section applies identically: **an LVGL pointer read callback returns raw native coordinates and LVGL rotates them itself.** |
| **The WiFi powersave finding (section 6)** | **Now measured. The problem transfers; the fix does not.** Zephyr ships the same WHD sources, with the same unconditional PM2 and the same 200 ms return-to-sleep delay. But the AIROC driver exposes no power save operation at all, so the obvious Zephyr-native fix silently does nothing. See below. |

### 7.1 The powersave finding under Zephyr - measured

Section 6 of the README documents PM2 being enabled behind your back by
`whd_wifi_on()`, found by disassembling the shipped `libmbed.a`. This table
used to say that whether any of it applied under Zephyr had never been
measured, and that neither the problem nor the fix should be assumed. It has
now been measured, on an STM32H7 carrier with the same CYW4343W radio running
native Zephyr.

**The problem transfers exactly.** Zephyr's AIROC module ships the same WHD
sources, and the enable is in the same place and unconditional:

```c
/* modules/hal/infineon/whd-expansion/WHD/COMPONENT_WIFI5/src/whd_management.c */
whd_wifi_enable_powersave_with_throughput(ifp, DEFAULT_PM2_SLEEP_RET_TIME);
```

with `DEFAULT_PM2_SLEEP_RET_TIME` defined as `200` - the same constant the
disassembly showed as `movs r1, #200`. Same radio, same stack underneath, same
behavior.

**The Zephyr-native fix does not exist, and fails silently.** Zephyr defines
`NET_REQUEST_WIFI_PS` and a `wifi_ps_params` struct, so this compiles and looks
correct:

```c
struct wifi_ps_params ps = { .enabled = WIFI_PS_DISABLED };
net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
```

It does nothing. The AIROC driver's `wifi_mgmt_ops` table has no
`.set_power_save` and no `.get_power_save` - it offers `scan`, `connect`,
`disconnect`, `ap_enable`, `ap_disable`, `iface_status` and `get_stats`. The
request returns `-ENOTSUP`. On a board with no console the warning saying so is
never seen, and the call sits there looking like a fix.

**The mbed workaround is the one that works, one layer down.** The WHD calls
link from application code, and the driver exposes the interface:

```c
whd_interface_t airoc_wifi_get_whd_interface(void);   /* drivers/wifi/infineon/airoc_wifi.h */

whd_interface_t ifp = airoc_wifi_get_whd_interface();
uint32_t before = 99, after = 99;

whd_wifi_get_powersave_mode(ifp, &before);
whd_wifi_disable_powersave(ifp);
whd_wifi_get_powersave_mode(ifp, &after);     /* 0 == NO_POWERSAVE_MODE */
```

Call it after the join, for the reason this repo already gives: powering the
radio up re-arms PM2. And read the mode back - that discipline is this repo's,
and it is what caught the `net_mgmt` version doing nothing.

**What it was worth, and what it was not.** On that board the first HTTP
request after an idle moment took 1467 ms where the same document takes 63-102
ms on a woken link. Most of the remaining apparent slowness turned out to be
the host resolving `.local` on every request rather than anything on the
device - worth separating before blaming the radio.

**It did not fix a stalled OTA there.** Large uploads kept failing with
powersave confirmed off and the mode read back as `0`. The symptom this repo
predicts - "a large inbound download can stall" - was present, but removing PM2
was not sufficient to clear it, and the cause on that bench was never
identified. Reported as a negative result rather than left as an implication.

One more Zephyr-side gotcha that will cost a build cycle, **measured, other
board**: the upstream Kconfig defaults that select the AIROC WiFi part are set
only inside a Bluetooth-enabled branch, so a **WiFi-only** build silently gets
the wrong defaults. Set the part and module symbols explicitly.

And the blobs are not optional - without the vendor WiFi firmware fetched into
the workspace, the radio does not come up at all.

---

## 8. The short version

- Section 7's "not yet" was about the Arduino Zephyr core. **Native Zephyr with
  the upstream Display Shield definition works today**, LVGL included.
- You can keep the Arduino bootloader and reuse everything this repo documents -
  or install MCUboot and get revert, which is the reason to go.
- Widen the flash partition, and then watch out: on a dual-core H7, describing
  the real 2 MB makes the driver compute 4 MB and misroute every bank 2 write.
- Put the slot map in one file both builds include.
- BOOT0 + RST to install the bootloader; chainloading at `0x08040000` is the
  safer variant and the only one on assemblies without BOOT0.
- The signing *idea* transfers; the container does not. The key-handling
  discipline in [`SECURITY.md`](SECURITY.md) matters more, not less.
- The WiFi powersave *problem* in README section 6 does carry across: Zephyr
  ships the same WHD with the same unconditional PM2. The *fix* does not -
  `NET_REQUEST_WIFI_PS` returns `-ENOTSUP` on the AIROC driver and changes
  nothing. Call WHD directly and read the mode back. Section 7.1.
