# The signature, and the parts around it

README section 4 covers what the signature does. This page covers the ways a
signing scheme is lost without the cryptography ever being touched: a key that
is not the key you think it is, a key shared across boards that should not
accept each other's images, a secret compiled into something readable, and an
old image replayed.

Same confidence tags as [`RELIABILITY.md`](RELIABILITY.md):

| tag | means |
|---|---|
| **measured here** | observed on this sketch, on a GIGA R1 |
| **measured, other board** | observed on this bench on a board running native Zephyr with MCUboot - a second GIGA R1, or a Portenta Machine Control |
| **read** | read out of source or a datasheet and not yet made to happen |

---

## 1. The property, restated

The board carries the **public** key only. Dumping a board's flash - which
anyone with the USB port can do, see section 5 - gives no way to sign anything
for it. That is what makes the transport close to irrelevant: a hostile server,
DNS you do not control, plain HTTP, all end at an image that is not signed by
the matching private key and is rejected before `/fs/UPDATE.BIN` is touched.

A shared secret would be simpler and would be broken by the first person to read
the chip.

Rotating the key is an ordinary OTA, not a bench visit - the new public key
travels inside a firmware image signed with the **old** private key. README
section 4 has the detail. USB is the cost only if the old private key is lost,
which is the real reason to back it up.

---

## 2. Make a missing key a build failure, not a warning

This is the most transferable item on this page, and it is the one that was
nearly lost quietly.

**Measured, other board.** A bootloader build takes its signing key from a
config variable. The gitignored file that sets it to the real key was absent -
which happens by accident, not only by carelessness, because `git clean -xdf`
deletes gitignored files. The build **succeeded**. It emitted one warning line
among hundreds, and produced a 35,872-byte bootloader that flashed and ran
perfectly.

It had fallen back to the upstream project's default key - which is **committed
in that project's public repository**. Anyone in the world could sign an image
that bootloader would accept, and nothing downstream objected, because the
images genuinely were correctly signed. With the wrong key.

The fix is to make the fallback impossible rather than to ask a human to notice
a warning:

```
CONFIG_BOOT_SIGNATURE_KEY_FILE="MISSING-key.conf--see-the-README.pem"
```

a path that cannot exist, overridden by the gitignored real config listed
**last** on the command line so it wins. Proved by planting the failure: the
build now dies at key import and produces **no bootloader at all**.

### What this means for this repo

`tools/mkota.py sign` takes the private key path as an argument and defaults to
`ota_private_key.pem`. There is no sample key to fall back to here, which is
better than the situation above by luck rather than by design. Two things are
worth keeping true on purpose:

- **Signing with a missing or wrong key must fail loudly**, never produce an
  unsigned or differently-signed file that looks like a success.
- **Check the pairing before you serve the image**, not after. `mkota.py
  checksig` re-verifies a signed image against the key parsed out of
  `ota_pubkey.h` - the header the firmware actually carries, rather than the
  key you believe you used. Do that against a board you cannot reach *before*
  serving it, because the board's own rejection is the expensive way to learn
  that the keys do not match.

The general rule: **a key the build can silently default to is a key the build
will eventually use.**

---

## 3. One key per application, and prove the separation both ways

**Measured, other board.** Two applications sharing one signing key means
either image installs on either board, and the signature raises no objection -
it is the correct signature. On a bench with several boards that is a real
hazard: the wrong firmware installs cleanly on hardware it was never written
for, and the first symptom is whatever that firmware does to the peripherals.

Keep the private key **per application**, not per bench and not per person, and
store it outside every git repository.

And prove the separation in both directions, **measured, other board**: the
correct key reports the image as validated, and a sibling project's key on the
same bench reports no signature found. A check that only ever passes is not a
check - this repo's `mkota.py checksig` is as good at the negative case as the
positive one, and the negative case is the one worth running.

One thing a signature cannot give you: **a per-project container magic.** The
dual-slot bootloader's image magic is a constant in its library, not a setting.
So the key is the only separation mechanism, and the other half - a product-ID
check made by the firmware itself as part of its confirm gate - has to be
written by the application. This sketch's `GSG1` trailer magic is a format
marker, not a product identity; it does not distinguish two GIGA applications
from each other.

---

## 4. Signed is not versioned

README section 4 already says there is no rollback protection: an older,
legitimately signed image can be replayed at a board, and it will take it.

What the fix looks like, **read**, not implemented here: a monotonic **security
counter** carried in the image and checked by the bootloader, which refuses
anything at or below the value it has seen. Note that it is a *security*
counter, deliberately separate from the display version - it moves only when a
vulnerability is fixed, because every increment is a door you are closing behind
yourself permanently.

Flash-backed counters are the common implementation and are only as good as the
flash: a hardware monotonic counter in a secure element is strictly stronger
(section 6).

Half-measures that cost nothing and are worth having anyway:

- Put the version in the signed region, not beside it. This sketch signs the
  firmware bytes only, and `FW_VERSION` is inside them, so that is already true.
- Have the firmware refuse to confirm an update that moved the version
  backwards, and say so. That does not stop installation - the bootloader has
  already acted - but on a board with revert it means the downgrade does not
  stick.

---

## 5. Secrets do not belong in the image

`arduino_secrets.h` is compiled into the firmware. That is fine for a bench
demonstration and is worth being explicit about, because it has two consequences
that are easy to carry into something real:

- **A leaked image leaks the credentials.** Anyone who gets a copy of the
  firmware you serve - which, over plain HTTP, is anyone on the path - gets the
  network credentials with it. The signature stops them *writing* an image; it
  does nothing to stop them *reading* one.
- **Every board needs its own build**, or every board shares one identity.

The pattern that fixes both, **measured, other board**: put the credentials in a
small non-volatile partition that sits **outside** both image slots, pushed once
per board over USB. Then the image carries no secret, every board runs the same
bytes, and an update does not wipe provisioning. The example header in the repo
carries placeholders only - generated by blanking every value, never by copying
a working file, which is how live credentials have reached committed files on
this bench before.

### Flash is readable over DFU, which is why asymmetric signing is the right call

**Read**, from the Arduino bootloader's own DFU descriptor: it reports
`@Internal Flash 2MB /0x08000000/01*128Ka,15*128Kg`. The trailing `a` on the
first sector means **readable, not writable** - sector 0 is protected against
writes, and nothing above it is protected against reads.

So anything secret sitting in this board's flash can be pulled out over the USB
port by anyone holding the board. The public key is not secret and does not
care. A decryption key would.

---

## 6. Image encryption and secure elements: what they would actually buy

The GIGA R1 has no secure element, so this section is about where the ceiling
is rather than about something to go and do. It is here because the reasoning is
easy to get backwards.

**Not image verification.** Verification uses a **public** key, which needs
integrity, not secrecy. Compiling it into the firmware already provides that. A
secure element adds very little to the thing this repo is about.

What it would buy:

- **Image encryption.** The real argument. Encrypting an update means putting a
  **private** decryption key on the device - a genuine secret, which on this
  board would sit in flash and be readable over DFU per section 5. Without
  somewhere to keep it, image encryption is theater.
- **Anti-rollback**, where a hardware monotonic counter beats a flash-backed
  one.
- **Device identity** for mutual TLS to an update server.
- **Per-device keys rather than a shared secret**, which is the thing that
  matters most for a fleet: a key that cannot be copied off the part.

And one caution worth carrying, **read**, from a board on this bench that does
have one: a factory-provisioned secure element is often an **identity anchor for
somebody else's cloud**, not a general-purpose key store you own. Trusting the
factory certificate chain is weaker than it sounds, because every board carrying
that chain authenticates. Fleet separation has to come from keys you control.

---

## 7. What the transport does and does not cover

- **HTTPS costs about 148 kB here** (README section 3's note on
  `OTA_IS_HTTPS`), and buys confidentiality and server authentication. It does
  not buy image authenticity - the signature already did that, more cheaply and
  without trusting the server.
- **The length and stall checks are not security.** They protect against dropped
  connections and crashed servers. An attacker serves a complete, correct-length
  image, and only the signature stops it. The README is careful about this and
  it should stay careful.
- **`OTA_REQUIRE_SIGNATURE` can be turned off.** It exists for bringing up a
  board before you have a key. Shipping with it off means anyone who can answer
  the URL owns the device.
- **The trigger is local**, by architecture: an update starts only when the unit
  decides to. Moving the trigger to a remote command - a message from a broker,
  a web endpoint - is a reasonable thing to want and is a new attack surface to
  reason about separately, because it lets a stranger choose *when* and *how
  often*, even if the signature still decides *what*.

---

## 8. The short version

- The board holds only the public key. That is what makes the transport nearly
  irrelevant.
- A build that can silently fall back to a default key eventually will. Make the
  missing key a build failure with no artifact.
- One key per application; prove separation with a key that is supposed to be
  refused, not only with one that is supposed to pass.
- Signed is not versioned. Old signed images replay.
- A compiled-in credential is in every copy of the image you serve. Put secrets
  in a partition outside the slots.
- Flash above sector 0 reads back over DFU, so a private key on this board is
  not private.
- A secure element would not improve verification. It would enable encryption,
  hardware anti-rollback and per-device identity - and a factory-provisioned one
  may be the vendor's anchor rather than your key store.
