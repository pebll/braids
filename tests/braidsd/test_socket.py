#!/usr/bin/env python3
# test_socket.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""braidsd Unix socket transport: several clients, events to all, replies to
the requester, disconnects, permissions, stale sockets, shutdown."""

import argparse
import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from braidsclient import ApiError, Braidsd, Client  # noqa: E402


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"ok - {msg}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--braidsd", required=True)
    args = parser.parse_args()

    confdir = tempfile.mkdtemp(prefix="braidsd-sock-")
    try:
        path = os.path.join(confdir, "braidsd.sock")

        # stale socket file from a crashed daemon is replaced
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(path)
        stale.close()

        d = Braidsd(args.braidsd, confdir, socket_path=path)
        check(stat.S_IMODE(os.stat(path).st_mode) == 0o600, "socket is owner-only")
        check(d.call("hello")["apiVersion"].startswith("0."), "hello over socket")

        # default socket path with --confdir is <confdir>/braidsd.sock
        check(Client.default_socket_path(confdir) == path, "default path for custom confdir")

        c2 = Client.connect(path)
        check(True, "second client gets ready")
        c3 = Client.connect(path)

        # events reach all clients, replies only the requester
        setup_id = d.call("accounts.add", {"protocol": "Dummy"})["setupId"]
        for c in (d.client, c2, c3):
            c.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input")
        check(True, "login step broadcast to all clients")

        # any client may answer
        c2.call("accounts.authInput", {"setupId": setup_id, "field": "phone", "value": "+1 555"})
        c3.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p.get("field") == "code")
        c3.call("accounts.authInput", {"setupId": setup_id, "field": "code", "value": "1234"})
        added = d.wait_event("account.added", lambda p: p["setupId"] == setup_id)
        check(added["account"]["protocol"] == "Dummy", "setup completed through other clients")

        # a client disconnecting (also mid-request) does not affect others
        raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw.connect(path)
        raw.sendall(b'{"jsonrpc":"2.0","id":1,"method":"chats.list"}\n')
        raw.close()
        c3.close()
        check(c2.call("accounts.list")[0]["accountId"] == added["account"]["accountId"],
              "other clients unaffected by disconnects")

        # malformed input gets an error reply, connection stays usable
        raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw.connect(path)
        f = raw.makefile("rw", encoding="utf-8", newline="\n")
        check(json.loads(f.readline())["method"] == "ready", "ready is the first line")
        f.write("not json\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"hello\"}\n")
        f.flush()
        replies = []
        while len(replies) < 2:
            msg = json.loads(f.readline())
            if "id" in msg:
                replies.append(msg)
        check(replies[0]["error"]["code"] == -32700 and replies[1]["id"] == 7, "parse error, then next request")
        raw.close()

        # notifications (no id) get no reply
        c2.wfile.write('{"jsonrpc":"2.0","method":"hello"}\n')
        c2.wfile.flush()
        check(c2.call("hello")["apiVersion"], "notification without reply")

        # second daemon on the same socket path (other confdir) is refused
        other = tempfile.mkdtemp(prefix="braidsd-sock2-")
        try:
            res = subprocess.run([args.braidsd, "--confdir", other, "--socket", path], capture_output=True,
                                 text=True, timeout=30)
            check(res.returncode != 0 and "another braidsd" in res.stderr, "second daemon on socket refused")
        finally:
            shutil.rmtree(other, ignore_errors=True)

        # shutdown over the api
        try:
            c2.call("daemon.shutdown")
        except (EOFError, ApiError):
            pass
        d.proc.wait(timeout=30)
        check(d.proc.returncode == 0, "daemon.shutdown exits cleanly")
        check(not os.path.exists(path), "socket removed at exit")
        c2.close()
        d.client.close()

        # SIGTERM exits cleanly
        d = Braidsd(args.braidsd, confdir, socket_path=path)
        d.proc.terminate()
        d.proc.wait(timeout=30)
        check(d.proc.returncode == 0, "SIGTERM exits cleanly")
        time.sleep(0.1)
        check(not os.path.exists(path), "socket removed after SIGTERM")
    finally:
        shutil.rmtree(confdir, ignore_errors=True)

    print("all tests passed")


if __name__ == "__main__":
    main()
