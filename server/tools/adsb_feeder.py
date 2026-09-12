#!/usr/bin/env python3
"""Forwards a local ADS-B receiver's SBS/BaseStation output to the aggregator.

Most receiver software (readsb, dump1090-fa, dump1090-mutability) can already
push SBS out to a remote host on its own, and where it can the dashboard tells
you the --net-connector line to use - that is fewer moving parts than this and
should be preferred. This exists for the cases where it is not an option:
Windows installs with no connector support, a receiver whose configuration you
would rather not edit, PiAware images where the config is managed, or a
machine that reaches the internet through an HTTP proxy.

It opens two sockets: one to the receiver's SBS port (usually 30003 on
localhost) and one out to the aggregator, then copies lines between them. Both
ends reconnect on their own with backoff, so it survives the receiver
restarting, the network dropping, and the server being redeployed.

Standard library only - no pip install - and identical on Debian and Windows.

  python3 adsb_feeder.py --server feed.example.com --port 30117

Run --help for the rest. --check tests both ends and exits, which is the first
thing to try when nothing is arriving.
"""

import argparse
import logging
import signal
import socket
import sys
import threading
import time

log = logging.getLogger("adsb-feeder")

# Reconnect backoff. Starts quick because the common case is a receiver that
# is restarting and will be back in a second or two; caps low enough that a
# feed comes back promptly after a long outage without hammering the server.
BACKOFF_START_SECONDS = 2
BACKOFF_MAX_SECONDS = 60
# A live receiver emits messages constantly. Silence for this long means the
# socket is open but dead - a half-closed TCP connection that neither end has
# noticed - so drop it and reconnect rather than sit on it forever.
IDLE_TIMEOUT_SECONDS = 120
SOCKET_TIMEOUT_SECONDS = 20


class Stopped(Exception):
    """Raised in the worker threads when a shutdown signal arrives."""


def connect(host: str, port: int, what: str, stop: threading.Event) -> socket.socket:
    """Connects, retrying with backoff until it succeeds or we're stopping."""
    delay = BACKOFF_START_SECONDS
    while not stop.is_set():
        try:
            sock = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_SECONDS)
            # Without keepalive a silently dropped path (a NAT table expiring,
            # a router rebooting) leaves this side believing it is connected
            # and quietly discarding every message.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            log.info("%s: connected to %s:%d", what, host, port)
            return sock
        except OSError as exc:
            log.warning("%s: cannot reach %s:%d (%s) - retrying in %ds", what, host, port, exc, delay)
            if stop.wait(delay):
                break
            delay = min(delay * 2, BACKOFF_MAX_SECONDS)
    raise Stopped()


def check(receiver_host: str, receiver_port: int, server_host: str, server_port: int) -> int:
    """Tests both ends once and reports, instead of running the feed."""
    ok = True
    try:
        with socket.create_connection((receiver_host, receiver_port), timeout=10) as sock:
            sock.settimeout(10)
            data = sock.recv(4096)
        if data:
            lines = data.decode("ascii", errors="replace").splitlines()
            print(f"OK   receiver {receiver_host}:{receiver_port} - {len(data)} bytes, e.g. {lines[0][:70]!r}")
            if not data.startswith((b"MSG", b"SEL", b"ID", b"AIR", b"STA", b"CLK")):
                print("     warning: that does not look like SBS/BaseStation output.")
                print("     Port 30003 is SBS; 30005 is Beast binary and will not work here.")
                ok = False
        else:
            print(f"WARN receiver {receiver_host}:{receiver_port} - connected but sent nothing in 10s.")
            print("     Normal only if no aircraft are in range at all.")
    except OSError as exc:
        print(f"FAIL receiver {receiver_host}:{receiver_port} - {exc}")
        print("     Is dump1090/readsb running, and is its SBS output enabled?")
        ok = False
    try:
        with socket.create_connection((server_host, server_port), timeout=15):
            print(f"OK   aggregator {server_host}:{server_port} - reachable")
    except OSError as exc:
        print(f"FAIL aggregator {server_host}:{server_port} - {exc}")
        print("     Check the port matches the one shown on your dashboard, and")
        print("     that the feeder is switched on there.")
        ok = False
    return 0 if ok else 1


def pump(args, stop: threading.Event):
    """One receiver->server session. Returns when either end fails."""
    receiver = connect(args.receiver, args.receiver_port, "receiver", stop)
    try:
        server = connect(args.server, args.port, "aggregator", stop)
    except Stopped:
        receiver.close()
        raise
    forwarded = 0
    last_data = time.monotonic()
    last_report = time.monotonic()
    receiver.settimeout(5)
    try:
        while not stop.is_set():
            try:
                chunk = receiver.recv(8192)
            except socket.timeout:
                # Not an error in itself; only prolonged silence is.
                if time.monotonic() - last_data > IDLE_TIMEOUT_SECONDS:
                    log.warning("receiver silent for %ds - reconnecting", IDLE_TIMEOUT_SECONDS)
                    return
                continue
            if not chunk:
                log.warning("receiver closed the connection")
                return
            last_data = time.monotonic()
            try:
                server.sendall(chunk)
            except OSError as exc:
                log.warning("aggregator send failed (%s) - reconnecting", exc)
                return
            forwarded += chunk.count(b"\n")
            now = time.monotonic()
            if now - last_report >= 300:
                log.info("forwarded %d messages in the last %ds", forwarded, int(now - last_report))
                forwarded = 0
                last_report = now
    finally:
        receiver.close()
        server.close()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Forward a local ADS-B receiver's SBS output to the aggregator.",
        epilog="The server host and port are shown on your account dashboard "
               "once the feeder is enabled for a device.",
    )
    parser.add_argument("--server", required=True, help="aggregator hostname")
    parser.add_argument("--port", type=int, required=True, help="the port your dashboard shows for this device")
    parser.add_argument("--receiver", default="127.0.0.1", help="receiver host (default: %(default)s)")
    parser.add_argument("--receiver-port", type=int, default=30003,
                        help="receiver SBS port, not Beast (default: %(default)s)")
    parser.add_argument("--check", action="store_true", help="test both ends and exit")
    parser.add_argument("--verbose", action="store_true", help="log every reconnect in detail")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    if args.check:
        return check(args.receiver, args.receiver_port, args.server, args.port)

    stop = threading.Event()

    def handle_signal(signum, _frame):
        log.info("signal %s - shutting down", signum)
        stop.set()

    # SIGBREAK is Windows-only and SIGTERM behaves differently there, so
    # register whatever this platform actually has rather than assuming.
    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), handle_signal)
            except (ValueError, OSError):
                pass  # not the main thread, or not supported here

    log.info("feeding %s:%d -> %s:%d", args.receiver, args.receiver_port, args.server, args.port)
    while not stop.is_set():
        try:
            pump(args, stop)
        except Stopped:
            break
        except Exception:
            # Never let an unexpected error end the process: this runs
            # unattended for months and a crash is a feed that silently stops.
            log.exception("unexpected error - restarting the session")
        if stop.wait(BACKOFF_START_SECONDS):
            break
    log.info("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
