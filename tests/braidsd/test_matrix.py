#!/usr/bin/env python3
# test_matrix.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""braidsd integration test with Matrix, against the local Synapse from
lib/mxchat/test/homeserver.sh and the scripted peer from lib/mxchat/test/peer.

braidsd logs in as a fresh user "alice" (event based setup incl. recovery key),
a peer plays alice's other client (Element: cross-signing, key backup, joins
rooms) and another peer plays "bob". Exits with 77 (skipped) when the
homeserver is not running.
"""

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from braidsclient import Braidsd  # noqa: E402

SKIP = 77


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"ok - {msg}")


class Peer:
    def __init__(self, binary, hs, user, password, store, autojoin=True):
        args = [binary, "-hs", hs, "-user", user, "-pass", password, "-store", store,
                f"-autojoin={'true' if autojoin else 'false'}"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.events = queue.Queue()
        self.results = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.ready = self.wait("ready", timeout=60)

    def _read(self):
        for line in self.proc.stdout:
            msg = json.loads(line)
            (self.results if msg.get("type") == "result" else self.events).put(msg)

    def cmd(self, line, timeout=60):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        res = self.results.get(timeout=timeout)
        if "error" in res:
            raise RuntimeError(f"peer {line}: {res['error']}")
        return res

    def wait(self, typ, predicate=None, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                msg = self.events.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                continue
            if msg.get("type") == typ and (predicate is None or predicate(msg)):
                return msg
        raise TimeoutError(f"peer: no {typ}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)


def wait_until(func, msg, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = func()
        if result:
            return result
        time.sleep(0.2)
    raise AssertionError(f"timeout: {msg}")


def register_user(container, hs, user, password):
    subprocess.run(["docker", "exec", container, "register_new_matrix_user", "-c", "/data/homeserver.yaml",
                    "-u", user, "-p", password, "--no-admin", hs], check=True, capture_output=True)


def setup_account(d, user_id, hs, password, recovery_key):
    setup_id = d.call("accounts.add", {"protocol": "Matrix"})["setupId"]

    def next_input():
        return d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input",
                            timeout=120)

    step = next_input()
    check(step["field"] == "userId", "matrix setup asks for user id")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "userId", "value": user_id})
    step = next_input()
    check(step["field"] == "homeserver" and step["isOptional"], "matrix setup asks for homeserver")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "homeserver", "value": hs})
    step = next_input()
    check(step["field"] == "password" and step["isSecret"], "matrix setup asks for secret password")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "password", "value": password})
    step = next_input()
    check(step["field"] == "recoveryKey" and step["isSecret"], "matrix setup asks for recovery key")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "recoveryKey", "value": recovery_key})
    d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and "verified" in p["message"],
                 timeout=120)
    check(True, "device verified with recovery key")
    added = d.wait_event("account.added", lambda p: p["setupId"] == setup_id, timeout=120)
    return added["account"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--braidsd", required=True)
    parser.add_argument("--peer-src", required=True, help="lib/mxchat/test/peer")
    parser.add_argument("--hs", default="http://localhost:8008")
    parser.add_argument("--container", default="nchat-mx-synapse")
    args = parser.parse_args()

    try:
        urllib.request.urlopen(args.hs + "/_matrix/client/versions", timeout=2)
    except Exception:
        print(f"skip: no homeserver at {args.hs} (lib/mxchat/test/homeserver.sh start)")
        return SKIP

    tmp = tempfile.mkdtemp(prefix="braidsd-mx-")
    procs = []
    try:
        peer_bin = os.path.join(tmp, "peer")
        subprocess.run(["go", "build", "-tags", "goolm", "-o", peer_bin, "."], cwd=args.peer_src, check=True)

        suffix = str(int(time.time()))
        alice, bob = f"alice{suffix}", f"bob{suffix}"
        alice_id, bob_id = f"@{alice}:localhost", f"@{bob}:localhost"
        register_user(args.container, args.hs, alice, "pw-alice")
        register_user(args.container, args.hs, bob, "pw-bob")

        # alice's other client sets up cross-signing and key backup, like Element
        element = Peer(peer_bin, args.hs, alice, "pw-alice", os.path.join(tmp, "element"))
        procs.append(element)
        recovery_key = element.cmd("bootstrap")["recovery_key"]
        bob_peer = Peer(peer_bin, args.hs, bob, "pw-bob", os.path.join(tmp, "bob"))
        procs.append(bob_peer)

        confdir = os.path.join(tmp, "conf")
        d = Braidsd(args.braidsd, confdir)
        account = setup_account(d, alice_id, args.hs, "pw-alice", recovery_key)
        account_id = account["accountId"]
        check(account["protocol"] == "Matrix", "matrix account added")
        check(account["capabilities"]["editWindowSec"] == -1, "matrix edit window unlimited")
        wait_until(lambda: d.call("accounts.list")[0]["state"] == "online", "account online")
        check(True, "account online")

        # encrypted room from bob, joined through alice's other client
        room = bob_peer.cmd(f"create {alice_id} encrypted braids test")["room"]
        element.wait("autojoin", lambda m: m["room"] == room)
        chat = wait_until(lambda: [c for c in d.call("chats.list") if c["id"] == room], "room in chat list")[0]
        check(chat["accountId"] == account_id, "room shown in chat list")
        wait_until(lambda: d.call("chats.get", {"accountId": account_id, "chatId": room})["name"] == "braids test",
                   "room name")
        check(True, "room name resolved")
        d.call("chats.setCurrent", {"accountId": account_id, "chatId": room})

        # receive
        sent = bob_peer.cmd(f"send {room} hello **alice**")["event"]
        added = d.wait_event("message.added", lambda p: p["id"] == sent, timeout=60)
        check(added["text"] == "hello *alice*" or "alice" in added["text"], "message from bob received")
        check(added["senderId"] == bob_id and not added["isOutgoing"], "sender is bob")
        notification = d.wait_event("notification", lambda p: p["msgId"] == sent, timeout=30)
        check(notification["chatName"] == "braids test", "notification for new message")

        # read receipt
        d.call("messages.markRead", {"accountId": account_id, "chatId": room, "msgId": sent})
        bob_peer.wait("receipt", lambda m: m["event"] == sent)
        check(True, "read receipt sent")

        # reply from braidsd
        d.call("messages.send", {"accountId": account_id, "chatId": room, "text": "hi _bob_", "quotedId": sent})
        reply = bob_peer.wait("message", lambda m: m["sender"] == alice_id)
        check(reply["encrypted"], "reply sent encrypted")
        check(reply.get("relates", {}).get("m.in_reply_to", {}).get("event_id") == sent, "reply relates to quote")
        own = d.wait_event("message.added", lambda p: p["id"] == reply["id"], timeout=60)
        check(own["isOutgoing"] and own["quoted"]["id"] == sent, "own reply echoed with quote")

        # edit from braidsd
        d.call("messages.edit", {"accountId": account_id, "chatId": room, "msgId": own["id"], "text": "hi bob!"})
        edit = bob_peer.wait("message", lambda m: m.get("new_body") is not None)
        check(edit["new_body"] == "hi bob!", "edit received by bob")
        updated = d.wait_event("message.updated", lambda p: p["id"] == own["id"] and p["isEdited"], timeout=60)
        check(updated["text"] == "hi bob!", "edit shown in place")

        # reactions both ways
        bob_peer.cmd(f"react {room} {own['id']} 👍")
        reacted = d.wait_event("message.updated", lambda p: p["id"] == own["id"] and p["reactions"]["counts"],
                               timeout=60)
        check(reacted["reactions"]["counts"].get("👍") == 1, "reaction from bob shown")
        d.call("reactions.send", {"accountId": account_id, "chatId": room, "msgId": sent, "emoji": "🎉"})
        reaction = bob_peer.wait("reaction", lambda m: m["target"] == sent)
        check(reaction["key"] == "🎉", "reaction sent to bob")

        # typing
        bob_peer.cmd(f"typing {room} on")
        typing = d.wait_event("typing", lambda p: p["chatId"] == room and p["isTyping"], timeout=30)
        check(typing["userId"] == bob_id, "typing from bob")

        # redaction from bob
        gone = bob_peer.cmd(f"send {room} to be deleted")["event"]
        d.wait_event("message.added", lambda p: p["id"] == gone, timeout=60)
        bob_peer.cmd(f"redact {room} {gone}")
        # removed or marked deleted, depending on the message_delete app config
        d.wait_event(("message.removed", "message.updated"),
                     lambda p: (p.get("msgId") == gone) or (p.get("id") == gone and p["isDeleted"]), timeout=60)
        check(True, "redaction shown")

        # history paging
        for i in range(25):
            bob_peer.cmd(f"send {room} history {i}")
        d.wait_event("message.added", lambda p: p["text"] == "history 24", timeout=120)
        page = d.call("messages.get", {"accountId": account_id, "chatId": room, "limit": 10})
        check(len(page["messages"]) == 10 and page["messages"][-1]["text"] == "history 24", "latest page")
        older = d.call("messages.get", {"accountId": account_id, "chatId": room,
                                        "fromMsgId": page["messages"][0]["id"], "limit": 10})
        check(older["messages"] and older["messages"][-1]["time"] <= page["messages"][0]["time"], "older page")

        # members
        members = d.call("members.get", {"accountId": account_id, "chatId": room})
        check({alice_id, bob_id} <= {m["id"] for m in members["members"]}, "members of room")
        check(d.close() == 0, "clean exit")

        # restart: loaded without login, history from cache, receives new messages
        d = Braidsd(args.braidsd, confdir)
        wait_until(lambda: [c for c in d.call("chats.list") if c["id"] == room], "room after restart")
        page = d.call("messages.get", {"accountId": account_id, "chatId": room, "limit": 5})
        check(page["messages"][-1]["text"] == "history 24", "history after restart")
        sent = bob_peer.cmd(f"send {room} after restart")["event"]
        d.wait_event("message.added", lambda p: p["id"] == sent, timeout=60)
        check(True, "receives after restart")

        log = open(os.path.join(confdir, "log.txt")).read()
        check("pw-alice" not in log and recovery_key not in log, "no password or recovery key in log")

        d.call("accounts.remove", {"accountId": account_id})
        d.wait_event("account.removed", lambda p: p["accountId"] == account_id, timeout=60)
        check(d.call("accounts.list") == [], "account removed")
        check(d.close() == 0, "clean exit after removal")
    finally:
        for proc in procs:
            try:
                proc.close()
            except Exception:
                pass
        shutil.rmtree(tmp, ignore_errors=True)

    print("all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
