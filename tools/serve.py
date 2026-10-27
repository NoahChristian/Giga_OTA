#!/usr/bin/env python3
"""Serve an update image over plain HTTP, and print the URL to paste.

The board fetches its update from a URL. In development that URL is almost
always "this machine", and the thing that goes wrong is not the server - it is
putting the wrong address in arduino_secrets.h. So this prints the exact URL
rather than leaving you to work it out, and it finds the address by asking the
OS which interface reaches the LAN rather than guessing at hostname lookups,
which on a multi-homed machine routinely return the wrong one.

Plain HTTP on purpose. HTTPS needs a valid cacert.pem in QSPI partition 1,
which is one more thing that can be wrong on a first attempt; prove the
mechanism over HTTP and add TLS afterwards.

Usage:
    python serve.py [file-or-directory] [port]

With a file, it is served as /UPDATE.ota whatever it is called on disk, so
the URL in arduino_secrets.h never has to change between builds. With a
directory (the default is the repo root), its contents are served by name.
"""
import http.server
import os
import socket
import socketserver
import sys

DEFAULT_PORT = 8000
SERVED_AS = "UPDATE.ota"


def lan_address():
    """The address of the interface that reaches the LAN.

    Connecting a UDP socket sends nothing; it just makes the OS pick a route
    and tell us the local end. More reliable than gethostbyname() on a
    machine with several interfaces.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    if not os.path.exists(target):
        sys.exit("no such file or directory: %s" % target)

    one_file = os.path.isfile(target)
    if one_file:
        serve_dir = os.path.dirname(os.path.abspath(target)) or "."
        real_name = os.path.basename(target)
    else:
        serve_dir = os.path.abspath(target)
        real_name = None

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=serve_dir, **kw)

        def translate_path(self, path):
            # One-file mode: anything asking for /UPDATE.ota gets this file,
            # whatever it is called on disk. Keeps the URL in
            # arduino_secrets.h stable across builds.
            if one_file and path.lstrip("/").split("?")[0] == SERVED_AS:
                return os.path.join(serve_dir, real_name)
            return super().translate_path(path)

        def log_message(self, fmt, *args):
            # The default logs to stderr with a timestamp; what matters here
            # is simply seeing the board ask for the file at all.
            sys.stdout.write("  %s - %s\n" % (self.client_address[0], fmt % args))
            sys.stdout.flush()

    ip = lan_address()
    url = "http://%s:%d/%s" % (ip, port, SERVED_AS if one_file else "")

    print("serving %s" % (os.path.abspath(target)))
    if one_file:
        print("  %d bytes, offered as /%s" % (os.path.getsize(target), SERVED_AS))
    print()
    print("put this in arduino_secrets.h:")
    print('  #define SECRET_OTA_URL "%s"' % url)
    print()
    print("waiting for the board (ctrl-c to stop)")

    # Bind on all interfaces: the address above is for the board to use, but
    # the server should answer on whichever interface it actually arrives on.
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", port), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")


if __name__ == "__main__":
    main()
