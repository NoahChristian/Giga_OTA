# Surviving a bad image

The README's section 5 proves recovery from a **failed download**. That is the
easy half. This page is about the other half, which this sketch does not
handle: a download that succeeds perfectly and delivers an image that does not
work.

Everything here was learned on the same bench, mostly on boards that had moved
to a dual-slot bootloader. Each claim is tagged so you know what it cost:

| tag | means |
|---|---|
| **measured here** | observed on this sketch, on a GIGA R1 |
| **measured, other board** | observed on this bench on a board running native Zephyr with MCUboot - a second GIGA R1, or a Portenta Machine Control |
| **read** | read out of source or a datasheet and not yet made to happen |

---

## 1. The two failure modes, and which one this sketch covers

| failure | what this sketch does | what it costs |
|---|---|---|
| Download is cut short | Refuses to stage anything shorter than `Content-Length`; `/fs/UPDATE.BIN` is never touched | Nothing. Try again. |
| Server accepts the connection and then sends nothing | Gives up after `DL_STALL_MS`; `/fs/UPDATE.BIN` is never touched | Nothing. Try again. |
| Image is not signed by the carried key | Rejected after download, before staging | Nothing. |
| **Image is complete, correctly signed, and does not boot** | **Nothing. It is installed.** | **A trip to the board with a cable.** |

That last row is the whole subject of this page. The sketch's own staging
discipline - verify the temporary file, and only then copy it over
`/fs/UPDATE.BIN` - is doing real work, and it stops at the point where the
bytes are provably the bytes you signed. Whether those bytes *run* is a
different question, and the Arduino bootloader has no opinion about it: it
applies `/fs/UPDATE.BIN`, prints `I-Sketch updated`, and jumps.

**There is no revert.** A GIGA running this sketch has one image slot. If the
image you signed hangs in `setup()`, the board is wherever it is until someone
reaches it.

---

## 2. Test-and-confirm, which is the fix

A dual-slot bootloader makes a bad image cost one reboot instead of one trip.
The shape, **measured, other board**, proven end to end over WiFi:

1. Upload the signed image into the **secondary** slot. The running image is
   untouched for the whole transfer - so everything in section 1 above is still
   true, for free.
2. Mark the new image **test**, not permanent, and reset.
3. The bootloader swaps the slots and boots the new image.
4. The new image must **confirm itself**. If it does not, the next reset swaps
   the old one back.

Step 4 is the one people get wrong, in two different ways.

### Do not let the upload tool do the confirming

Every upload tool has a flag that marks the image permanent immediately
(`--confirm`, or whatever it is called). Using it throws away the only property
that made the exercise worthwhile. The image is then permanent before anything
has run.

### Confirm from a health gate, not from `main()`

If the image confirms itself on the first line it executes, "confirmed" means
"the vector table was valid," which the bootloader already knew. Confirm from a
gate that has checked the things the board exists to do - **measured, other
board**: the network is up, the peripherals answer, the first real reading
arrived. Then "the update stuck" is a statement about the firmware working, not
about bytes arriving.

Give the gate a way to say which condition is failing. An image that refuses to
confirm itself and will not say why is indistinguishable from a bootloader
problem.

### What this sketch could borrow today

Not the swap - that needs a different bootloader (see
[`ZEPHYR.md`](ZEPHYR.md)). But two things are free here:

- **`ota.update()` returning `Error::None` means recorded, not applied.** The
  README already says this. The corollary is that the *next* boot owes you
  evidence: a changed `FW_VERSION` on screen is the acceptance test, and
  `dump_screen.py` can read it from the PC without anyone watching the panel.
- **Keep the image you are replacing.** Nothing stops a build process from
  retaining the last-known-good signed image next to the new one. It does not
  make the board self-healing, but it makes the recovery a one-command reflash
  instead of a rebuild from a tag nobody wrote down.

---

## 3. A failed swap can disable updates until somebody clears it

**Measured, other board**, and the most valuable single item on this page,
because it bites *after* a failure, when you are already not at your best.

MCUboot writes `swap_info` into the **primary** slot's trailer before it starts
moving sectors. If the swap then dies partway, that byte stays set. On the next
boot the bootloader reads it, sees a non-`NONE` swap type, and takes the
"resume a swap I already decided on" branch instead of consulting its swap
tables. A freshly staged update is **ignored and left pending**.

The outward sign is a missing log line, from a bootloader that ships with no
console. So the symptom is: uploads succeed, the board reboots, and nothing
changes, forever.

The general form, which is not specific to MCUboot: **the thing that stages an
update keeps state, and that state survives the failure that created it.** Find
out where it lives and have a command that clears it, before you need one.

### Clearing it teaches the erase-geometry lesson twice

The recovery command erases the scratch area, the secondary slot's last sector
**and the primary slot's last sector**. The third is the one that gets left out,
and it is the one holding the byte that caused the problem.

Each erase length is read from that area's **own** geometry rather than
hardcoded. Observed:

```
scratch: erased 0x40000 (all)
slot1  : erased 0x1000   at +0x17f000     4 KB, external QSPI
slot0  : erased 0x20000  at +0x160000   128 KB, internal flash
```

Two different erase units from one command. A hardcoded 64 KB would have been
legal internally, rejected on QSPI, and would have left the primary trailer
uncleared - that is, it would have failed to undo exactly the thing it exists to
undo.

This is the same mistake as the MBR unit trap in README section 1, wearing
different clothes: **ask the block device for its geometry; never assume a
number.**

---

## 4. The bootloader is the one component that cannot tell you anything

A shipping bootloader has no console. On this GIGA you get a handful of strings
over USB if you are fast enough to catch them, and on a board in a wall you get
nothing at all.

So move the reporting into the application, which *can* talk. **Measured, other
board**: a shell command that prints what the bootloader decides from - slot
geometry, both image headers checked against the magic, and the swap type it
would choose - turns an invisible decision into a readable one. It exists
precisely because the bootloader declines in silence.

The equivalent here is already half-built and worth finishing: this sketch reads
the QSPI partition table at boot and puts the answer on screen. The same place
could carry the size and first bytes of `/fs/UPDATE.BIN`, and whether the update
flag is set. Right now, "did my upload land?" can only be answered by rebooting.

---

## 5. A board that dies partway through boot looks exactly like a cold start

**Measured, other board** - this one cost a full day.

A board that crashes during start-up reboots, crashes at the same place, and
reboots, with nothing anywhere saying it never finished. Each cycle looks like a
normal power-on. It is a particularly cruel failure after an OTA, because the
obvious theory - the update did not install - is wrong and sends you to the
wrong half of the system.

Three cheap things fix it, all in retained RAM:

- **Boot-progress breadcrumbs.** One byte per milestone. The next boot can say
  "last time I got as far as the network and stopped."
- **A liveness stamp**, rewritten on every watchdog kick. Distinguishes
  "execution stopped at the error" from "something returned and the board kept
  running," and dates the last moment the board was known alive.
- **Reset cause**, read from the hardware, so WATCHDOG and POWER-ON and
  SOFTWARE are three different events rather than one.

Two caveats that come with the technique:

- **None of it survives a power cycle.** Pulling the plug on a frozen board
  destroys the only evidence of why it froze. Let the watchdog do the reset.
- **Do not reboot from the fatal-error handler.** Halting and letting the
  independent watchdog fire means the next boot's reset cause reads WATCHDOG,
  which is the bit that classifies the event. Rebooting from the handler
  replaces it with a software reset and throws that away.

### On this silicon, retained RAM needs a cache flush

**Measured, other board**, same STM32H747 as this one, and genuinely
surprising:

The M7's data cache is **write-back**, and a reset **invalidates** it rather
than writing it back. A retained variable written over and over to the same
address can sit dirty in cache and never reach SRAM at all - so the breadcrumb
you carefully wrote is not there after the reset that mattered.

Follow every write to a retained structure with a flush of its own cache line,
and align those structures to the cache line size (32 bytes on this part) so a
flush cannot drag an unrelated variable's dirty data out with it.

Also worth knowing: nothing guarantees a bootloader leaves a `.noinit` section
alone - it runs first and uses internal RAM for its own stack and buffers. The
robust alternative on this part is the 4 KB backup SRAM, which a bootloader has
no reason to touch.

---

## 6. Verify each step; do not trust a composite command

**Measured, other board.** The upload tool's single "upgrade" verb uploads the
image, marks it pending, and resets. The upload succeeded, the board reset, and
the bootloader booted the **old** image with the new one showing
`pending=False`. The mark had not taken. The swap only happened after the
pending flag was written explicitly as its own step.

Do each step and read the state back between them. A composite command reports
on itself.

Two more of the same family:

- **A "connected" line from a connectionless transport is not evidence.** Over
  UDP, the client's `Connecting... OK` is a local socket call - it means the
  kernel accepted a `connect()`, not that anything answered. A board that is not
  on the network produces the identical line, followed by a timeout that reads
  like a slow handler.
- **Flash the signed image, never the plain one.** A signed image has its vector
  table behind the bootloader's header; the plain build does not. Flashing the
  plain one leaves a board sitting in the bootloader forever with no diagnostic.
  The analogous trap in this repo is `Giga_OTA.ino.with_bootloader.bin`, which
  is also the wrong file and also fails without explaining itself.

---

## 7. Ship the update path, then use it as the development path

The strongest reliability practice found on this bench is not a guard at all.

Once updates go over the network, **make that the ordinary way to reflash during
development** - not the thing you do on release day. Then the update path is
exercised dozens of times a week, by the person best placed to notice it
misbehaving, and the day it actually matters is not its first real outing.

The corollary is the warning. Writing an image directly - over SWD, or over DFU
with a cable - **proves the application and nothing about the update path.** It
skips the transport, the staging, the slot bookkeeping and the confirm. It is
the right tool when the application is too broken to accept an upload, and it is
not a test of anything that ships.

On this sketch the same rule has a cheaper form: prove the signature check in
**both directions in one boot** - unsigned rejected, then signed accepted. Doing
them in separate boots hides the one-`begin()`-per-boot bug documented in
`CLAUDE.md`, which is exactly how it was missed the first time.

---

## 8. The short version

- Recovering from a failed *download* is the easy half, and this sketch does it.
- A complete, correctly signed, non-booting image costs a bench trip here.
  Dual-slot test-and-confirm reduces that to one reboot.
- Confirm from a health gate, never from the upload tool and never from the
  first line of `main()`.
- Whatever stages your update keeps state that survives a failed staging. Know
  where it is and have a way to clear it.
- Ask the device for its erase geometry. Twice now, assuming it has been the
  bug.
- Breadcrumbs in retained RAM, flushed out of the D-cache, are the difference
  between "it reboots" and "it reboots at the network bring-up."
- Use the update path to develop, so it is not new on the day it matters.
