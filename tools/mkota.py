#!/usr/bin/env python3
"""Build and inspect Arduino .ota containers, in pure Python.

WHY THIS EXISTS

The documented way to package an OTA image is Arduino's lzss.py + bin2ota.py
(in ArduinoIoTCloud/extras/tools). lzss.py is a ctypes shim:

    LZSS_SO_EXT = "so" if platform.uname()[0] != "Darwin" else "dylib"
    lzss_functions = ctypes.CDLL(f"./lzss.{LZSS_SO_EXT}")

There is no lzss.dll. On Windows the documented path simply does not run, and
building the .so from lzss.c needs a host C compiler you may not have. This
script replaces both tools with no native code and no third-party packages -
zlib.crc32 instead of the crccheck dependency bin2ota.py pulls in.

THE CONTAINER

20 bytes of header, then the payload (see the library's
src/decompress/utility.cpp):

    uint32_t len           bytes of magic + version + payload
    uint32_t crc32         over magic + version + payload
    uint32_t magic_number  per-board, 0x23410266 for the GIGA
    uint64_t version       bitfield, little-endian:
                             header_version:6, compression:1, signature:1,
                             spare:4, payload_target:4, payload_major:8,
                             payload_minor:8, payload_patch:8,
                             payload_build_num:24

A NOTE ON THE COMPRESSION FLAG

bin2ota.py writes its version field as bytearray([0,0,0,0,0,0,0,0x40]) with
the comment "all 0 except the compression flag set". But compression is bit 6
of the first 32-bit word, which on a little-endian target is 0x40 in byte 0,
not byte 7. Byte 7 lands inside payload_build_num. One of the tool and its
comment is wrong.

This script defaults to byte-for-byte compatibility with bin2ota.py so a file
it produces matches the known-good reference, and --compat=spec writes the
flag where the struct says it belongs. If you need to know which the device
honours, build both and try them - that is a hardware question, not one to be
settled by reading.

USAGE

    python mkota.py sign     <in.bin> <out.bin> [key.pem]
    python mkota.py checksig <signed.bin> [ota_pubkey.h]
    python mkota.py pack     <in.bin> <out.ota> [--board GIGA]
    python mkota.py info     <file.ota>
    python mkota.py verify   <file.ota>

"sign" is the one you want: it appends an ECDSA P-256 signature trailer that
the device verifies before staging anything. "checksig" re-checks a signed
image against the public key the firmware actually carries, which is worth
doing before you serve it to a board you cannot reach.

"pack" does NOT compress - there is no LZSS encoder here. Use --no-compress
(the default) and do not call decompress() on the device, or compress the
payload yourself first and pass --compressed to mark it.
"""
import os
import struct
import sys
import zlib

# From bin2ota.py's table. Only the boards this repo could plausibly target
# are listed; the full set is in that script.
MAGIC = {
    "GIGA":           0x23410266,
    "PORTENTA_H7_M7": 0x2341025B,
    "NICLA_VISION":   0x2341025F,
    "OPTA":           0x23410064,
    "PORTENTA_C33":   0x23410068,
}

HDR_LEN = 20


def version_field(compressed, compat):
    """The 8-byte version/flags field.

    compat="bin2ota" reproduces Arduino's tool exactly, including the flag
    byte placement that looks wrong against the struct.
    compat="spec" puts the compression bit where utility.cpp's bitfield says.
    """
    v = bytearray(8)
    if compressed:
        if compat == "bin2ota":
            v[7] = 0x40
        else:
            v[0] = 0x40
    return bytes(v)


def pack(in_path, out_path, board, compressed, compat):
    if board not in MAGIC:
        sys.exit("unknown board %r; known: %s" % (board, ", ".join(sorted(MAGIC))))

    payload = open(in_path, "rb").read()
    if not payload:
        sys.exit("%s is empty" % in_path)

    # A sanity check the bootloader effectively performs too: the first word
    # of an STM32H7 image is the initial stack pointer, which lives in SRAM.
    # Catching a wrong file here beats discovering it after a reboot.
    sp = struct.unpack("<I", payload[:4])[0]
    if not (0x20000000 <= sp <= 0x24100000):
        print("WARNING: first word is 0x%08X, which is not a plausible ARM "
              "initial stack pointer." % sp)
        print("         Is %s really a raw sketch binary?" % in_path)

    body = struct.pack("<I", MAGIC[board]) + version_field(compressed, compat) + payload
    out = struct.pack("<II", len(body), zlib.crc32(body) & 0xFFFFFFFF) + body

    open(out_path, "wb").write(out)
    print("packed %s -> %s" % (in_path, out_path))
    print("  board      %s (magic 0x%08X)" % (board, MAGIC[board]))
    print("  payload    %d bytes" % len(payload))
    print("  compressed %s%s" % (compressed,
                                 "" if not compressed else " (flag only - "
                                 "this script does not compress)"))
    print("  total      %d bytes" % len(out))


def read_header(path):
    raw = open(path, "rb").read()
    if len(raw) < HDR_LEN:
        sys.exit("%s is only %d bytes; not an .ota container" % (path, len(raw)))

    length, crc = struct.unpack("<II", raw[:8])
    magic = struct.unpack("<I", raw[8:12])[0]
    version = raw[12:20]
    body = raw[8:]
    return raw, length, crc, magic, version, body


def info(path):
    raw, length, crc, magic, version, body = read_header(path)

    names = {v: k for k, v in MAGIC.items()}
    print("%s" % path)
    print("  file size   %d bytes" % len(raw))
    print("  len field   %d  (body is %d)" % (length, len(body)))
    print("  crc32       0x%08X" % crc)
    print("  magic       0x%08X  %s" % (magic, names.get(magic, "UNKNOWN BOARD")))
    print("  version     %s" % " ".join("%02X" % b for b in version))

    # Decode the bitfield as utility.cpp declares it.
    val = struct.unpack("<Q", version)[0]
    print("  decoded     header_version=%d compression=%d signature=%d "
          "target=%d  v%d.%d.%d build %d"
          % (val & 0x3F, (val >> 6) & 1, (val >> 7) & 1, (val >> 12) & 0xF,
             (val >> 16) & 0xFF, (val >> 24) & 0xFF, (val >> 32) & 0xFF,
             (val >> 40) & 0xFFFFFF))
    if version[7] == 0x40:
        print("              (byte 7 is 0x40 - bin2ota.py's compression flag "
              "placement)")


def verify(path):
    raw, length, crc, magic, version, body = read_header(path)
    ok = True

    if length != len(body):
        print("FAIL  len field says %d, body is %d bytes" % (length, len(body)))
        ok = False
    else:
        print("ok    len field matches the body")

    actual = zlib.crc32(body) & 0xFFFFFFFF
    if actual != crc:
        print("FAIL  crc32 is 0x%08X, header says 0x%08X" % (actual, crc))
        ok = False
    else:
        print("ok    crc32 matches")

    if magic not in MAGIC.values():
        print("FAIL  magic 0x%08X is not a board this script knows" % magic)
        ok = False
    else:
        names = {v: k for k, v in MAGIC.items()}
        print("ok    magic is %s" % names[magic])

    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------------------
# Signing
#
# The signature travels as a TRAILER on the image rather than as a separate
# file, so there is one URL, one download and no way to end up with an image
# and a signature that do not belong together:
#
#     [ firmware bytes ][ ECDSA signature, DER ][ uint16 siglen ][ 'GSG1' ]
#
# The magic goes last so the device can find the trailer by seeking to the
# end, which it can do without knowing anything about the image first. An
# unsigned image simply has no magic there, and the device can then decide
# whether it is willing to accept one.
#
# ECDSA P-256 over SHA-256 of the FIRMWARE BYTES ONLY - the trailer is not
# part of what is signed, or it would have to sign itself.
# ---------------------------------------------------------------------------
SIG_MAGIC = b"GSG1"
TRAILER_FIXED = 6          # uint16 siglen + 4-byte magic


def sign(in_path, out_path, key_path):
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        sys.exit("needs the 'cryptography' package: pip install cryptography")

    if not os.path.isfile(key_path):
        sys.exit("no private key at %s - run tools/keygen.py first" % key_path)

    firmware = open(in_path, "rb").read()
    if not firmware:
        sys.exit("%s is empty" % in_path)

    # Refuse to sign something that is not a firmware image. Signing is a
    # statement that this is safe to run; making that statement about the
    # wrong file is the expensive mistake here, because the device will
    # believe it.
    sp = struct.unpack("<I", firmware[:4])[0]
    if not (0x20000000 <= sp <= 0x24100000):
        sys.exit("REFUSING to sign %s: first word is 0x%08X, which is not a "
                 "plausible ARM initial stack pointer.\nIs this really a raw "
                 "sketch binary (not the .with_bootloader one)?" % (in_path, sp))

    key = serialization.load_pem_private_key(open(key_path, "rb").read(),
                                             password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        sys.exit("%s is not an EC private key" % key_path)

    sig = key.sign(firmware, ec.ECDSA(hashes.SHA256()))

    out = firmware + sig + struct.pack("<H", len(sig)) + SIG_MAGIC
    open(out_path, "wb").write(out)

    print("signed %s -> %s" % (in_path, out_path))
    print("  firmware   %d bytes" % len(firmware))
    print("  signature  %d bytes (ECDSA P-256 / SHA-256, DER)" % len(sig))
    print("  total      %d bytes" % len(out))


def checksig(path, pub_path):
    """Verify a signed image the way the device will, before shipping it."""
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.exceptions import InvalidSignature
    except ImportError:
        sys.exit("needs the 'cryptography' package: pip install cryptography")

    raw = open(path, "rb").read()
    if len(raw) < TRAILER_FIXED:
        sys.exit("%s is too small to carry a signature" % path)

    if raw[-4:] != SIG_MAGIC:
        sys.exit("FAIL  no signature trailer (magic is %r, expected %r)"
                 % (raw[-4:], SIG_MAGIC))

    siglen = struct.unpack("<H", raw[-6:-4])[0]
    fw_len = len(raw) - TRAILER_FIXED - siglen
    if fw_len <= 0:
        sys.exit("FAIL  signature length %d does not fit the file" % siglen)

    firmware = raw[:fw_len]
    sig = raw[fw_len:fw_len + siglen]

    # Parse the public key out of the committed header, so this checks the
    # key the FIRMWARE will actually use rather than one kept alongside.
    pem_lines = []
    for line in open(pub_path, encoding="utf-8"):
        line = line.strip()
        if line.startswith('"') and line.endswith('"'):
            pem_lines.append(line[1:-1].replace("\\n", ""))
        elif line.startswith('"') and line.endswith('";'):
            pem_lines.append(line[1:-2].replace("\\n", ""))
    pem = "\n".join(pem_lines) + "\n"
    if "BEGIN PUBLIC KEY" not in pem:
        sys.exit("could not read a public key out of %s" % pub_path)

    pub = serialization.load_pem_public_key(pem.encode("ascii"))

    print("%s" % path)
    print("  firmware   %d bytes" % fw_len)
    print("  signature  %d bytes" % siglen)
    try:
        pub.verify(sig, firmware, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        print("FAIL  signature does NOT match the key in %s"
              % os.path.basename(pub_path))
        sys.exit(1)
    print("ok    signature verifies against %s" % os.path.basename(pub_path))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd == "pack":
        if len(sys.argv) < 4:
            sys.exit("usage: mkota.py pack <in.bin> <out.ota> [--board GIGA] "
                     "[--compressed] [--compat spec|bin2ota]")
        args = sys.argv[4:]
        board = "GIGA"
        compressed = False
        compat = "bin2ota"
        i = 0
        while i < len(args):
            if args[i] == "--board":
                board = args[i + 1]; i += 2
            elif args[i] == "--compressed":
                compressed = True; i += 1
            elif args[i] == "--compat":
                compat = args[i + 1]; i += 2
            else:
                sys.exit("unknown option %r" % args[i])
        pack(sys.argv[2], sys.argv[3], board, compressed, compat)

    elif cmd == "info":
        info(sys.argv[2])

    elif cmd == "verify":
        verify(sys.argv[2])

    elif cmd == "sign":
        if len(sys.argv) < 4:
            sys.exit("usage: mkota.py sign <in.bin> <out.bin> [key.pem]")
        key = sys.argv[4] if len(sys.argv) > 4 else os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "ota_private_key.pem")
        sign(sys.argv[2], sys.argv[3], key)

    elif cmd == "checksig":
        if len(sys.argv) < 3:
            sys.exit("usage: mkota.py checksig <signed.bin> [ota_pubkey.h]")
        pub = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "ota_pubkey.h")
        checksig(sys.argv[2], pub)

    else:
        sys.exit("unknown command %r; expected pack, info, verify, sign or "
                 "checksig" % cmd)


if __name__ == "__main__":
    main()
