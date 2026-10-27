#!/usr/bin/env python3
"""Serve a deliberately broken update, to prove a failed OTA is survivable.

An update mechanism is only safe if you have watched it fail. The two failures
worth rehearsing are the ones that actually happen in the field:

  truncate   The server declares the full Content-Length and then closes the
             connection halfway through the body. This is what a crashed
             server, a dropped upstream or a killed process looks like to the
             board. The board should notice it received fewer bytes than it
             was promised and refuse to stage the image.

  stall      The server sends half the body and then holds the socket open
             for ever, sending nothing. This is what a wedged server or a
             half-dead network path looks like, and it is the nastier of the
             two: there is no error, no close, nothing to react to except the
             absence of data. The board should give up on its own.

In both cases the pass condition is the same and it is not subtle: the board
stays on the firmware it was already running, says what happened, and remains
usable. Anything else - a hang, a reboot loop, a half-written image - is a
mechanism that can brick a board remotely.

Usage:
    python serve_broken.py <file> [truncate|stall] [port]
"""
import os
import socket
import sys
import threading
import time

FRACTION = 0.5          # how much of the body to send before misbehaving
STALL_HOLD_S = 600      # how long to hold a stalled socket open


def serve_one(conn, body, mode):
    try:
        # Read the request line and headers; we do not care what they say.
        conn.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 65536:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk

        cut = int(len(body) * FRACTION)

        # Content-Length is the FULL size: that is the point. The board is
        # told to expect the whole image and then does not get it.
        head = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n" % len(body)
        )
        conn.sendall(head.encode())
        conn.sendall(body[:cut])
        print("  sent %d of %d bytes, then %s" % (cut, len(body), mode))

        if mode == "stall":
            # Hold it open and send nothing. No FIN, no error, no data.
            time.sleep(STALL_HOLD_S)
        # truncate: fall through and close
    except Exception as e:
        print("  connection ended: %s" % e)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "truncate"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8000

    if mode not in ("truncate", "stall"):
        sys.exit("mode must be truncate or stall")
    if not os.path.isfile(path):
        sys.exit("no such file: %s" % path)

    body = open(path, "rb").read()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.listen(5)

    print("serving %s (%d bytes) in %s mode on port %d"
          % (os.path.basename(path), len(body), mode, port))
    print("expect the board to refuse the image and stay on its current "
          "firmware\n")

    try:
        while True:
            conn, addr = srv.accept()
            print("connection from %s" % (addr[0],))
            threading.Thread(target=serve_one, args=(conn, body, mode),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
