#!/usr/bin/env python3
# test_dummy.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""braidsd core test with the Dummy protocol: login flow, chat model, restart, removal."""

import argparse
import os
import shutil
import stat
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from braidsclient import ApiError, Braidsd as BraidsdProcess  # noqa: E402


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"ok - {msg}")


def expect_error(func, code, msg):
    try:
        func()
    except ApiError as e:
        check(e.code == code, f"{msg} (error {e.code})")
        return
    raise AssertionError(f"{msg}: no error")


def wait_until(func, msg, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = func()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError(f"timeout: {msg}")


def add_dummy_account(d, phone):
    setup_id = d.call("accounts.add", {"protocol": "Dummy"})["setupId"]
    step = d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input")
    check(step["field"] == "phone", "setup asks for phone number")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "phone", "value": phone})

    step = d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input")
    check(step["field"] == "code", "setup asks for code")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "code", "value": "000000"})
    d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "error")
    step = d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input")
    check(step["field"] == "code", "invalid code reported, code asked again")

    expect_error(lambda: d.call("accounts.authInput", {"setupId": setup_id, "field": "phone", "value": "1"}),
                 5, "input for a field that is not pending is rejected")
    d.call("accounts.authInput", {"setupId": setup_id, "field": "code", "value": "1234"})
    added = d.wait_event("account.added", lambda p: p["setupId"] == setup_id)
    return added["account"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--braidsd", required=True)
    parser.add_argument("--socket", action="store_true", help="use the unix socket instead of stdio")
    args = parser.parse_args()

    confdir = tempfile.mkdtemp(prefix="braidsd-test-")

    def Braidsd(binary, confdir, sock="braidsd.sock"):
        return BraidsdProcess(binary, confdir, socket_path=os.path.join(confdir, sock) if args.socket else None)

    try:
        d = Braidsd(args.braidsd, confdir)
        hello = d.call("hello")
        check(hello["apiVersion"].startswith("0."), "hello returns api version")
        check("Dummy" in hello["protocols"], "Dummy protocol available")
        expect_error(lambda: d.call("no.such.method"), -32601, "unknown method rejected")
        expect_error(lambda: d.call("accounts.add", {"protocol": "Nope"}), -32602, "unknown protocol rejected")

        account = add_dummy_account(d, "+46 123")
        account_id = account["accountId"]
        check(account_id == "Dummy_+46123", "account id from protocol and phone")
        check(account["protocol"] == "Dummy", "account protocol")

        accounts = d.call("accounts.list")
        check([a["accountId"] for a in accounts] == [account_id], "accounts.list has the account")

        profile_dir = os.path.join(confdir, "profiles", account_id)
        mode = stat.S_IMODE(os.stat(profile_dir).st_mode)
        check(mode == 0o700, "profile dir is owner-only")

        # duplicate account is refused at the phone number step
        setup_id = d.call("accounts.add", {"protocol": "Dummy"})["setupId"]
        d.wait_event("account.auth", lambda p: p.get("setupId") == setup_id and p["step"] == "input")
        expect_error(lambda: d.call("accounts.authInput", {"setupId": setup_id, "field": "phone",
                                                          "value": "+46123"}),
                     5, "duplicate account refused")
        d.call("accounts.authCancel", {"setupId": setup_id})
        d.wait_event("account.setupFailed", lambda p: p["setupId"] == setup_id)
        check(True, "cancelled setup reports setupFailed")

        # chat model
        chats = wait_until(lambda: len(d.call("chats.list")) >= 2 and d.call("chats.list"), "chats synced")
        check(len(chats) >= 2, "chats.list has the dummy chats")
        group = [c for c in chats if c["isGroup"]][0]
        check(group["name"] == "The Office", "chat name resolved from contacts")
        check(chats == sorted(chats, key=lambda c: (not c["isPinned"], -c["lastActivity"])),
              "chats sorted by pin and activity")

        page = d.call("messages.get", {"accountId": account_id, "chatId": group["id"], "limit": 3})
        msgs = page["messages"]
        check(len(msgs) == 3, "messages.get returns limit messages")
        check([m["time"] for m in msgs] == sorted(m["time"] for m in msgs), "page is in ascending time order")
        check(all(m["senderName"] for m in msgs), "sender names resolved")

        older = d.call("messages.get", {"accountId": account_id, "chatId": group["id"],
                                        "fromMsgId": msgs[0]["id"], "limit": 3})
        check(all(m["time"] <= msgs[0]["time"] for m in older["messages"]), "older page is older")

        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "hello",
                                 "quotedId": msgs[-1]["id"]})
        own = d.wait_event("message.added", lambda p: p["text"] == "hello")
        check(own["isOutgoing"] and own["quoted"]["id"] == msgs[-1]["id"], "sent message echoed with quote")
        check(account["capabilities"]["editWindowSec"] == -1, "dummy edit window unlimited")
        d.call("messages.edit", {"accountId": account_id, "chatId": group["id"], "msgId": own["id"],
                                 "text": "hello!"})
        edited = d.wait_event("message.updated", lambda p: p["id"] == own["id"] and p["isEdited"])
        check(edited["text"] == "hello!", "edit applied")
        d.call("reactions.send", {"accountId": account_id, "chatId": group["id"], "msgId": own["id"],
                                  "emoji": "👍"})
        reacted = d.wait_event("message.updated", lambda p: p["id"] == own["id"] and p["reactions"]["counts"])
        check(reacted["reactions"]["counts"] == {"👍": 1} and reacted["reactions"]["own"] == "👍", "own reaction")
        d.call("messages.delete", {"accountId": account_id, "chatId": group["id"], "msgId": own["id"]})
        d.wait_event("message.removed", lambda p: p["msgId"] == own["id"])
        check(True, "delete applied")
        expect_error(lambda: d.call("messages.send", {"accountId": account_id, "chatId": group["id"]}),
                     -32602, "send without text or file rejected")
        expect_error(lambda: d.call("messages.edit", {"accountId": account_id, "chatId": group["id"],
                                                      "msgId": msgs[-1]["id"], "text": "x"}),
                     4, "editing others' messages unsupported")
        expect_error(lambda: d.call("chats.pin", {"accountId": account_id, "chatId": group["id"]}),
                     4, "pin unsupported by capabilities")
        expect_error(lambda: d.call("messages.pin", {"accountId": account_id, "chatId": group["id"],
                                                     "msgId": msgs[-1]["id"]}),
                     4, "message pin unsupported by capabilities")
        expect_error(lambda: d.call("chats.list", {"accountId": 5}), -32602, "invalid param type rejected")
        d.call("messages.markRead", {"accountId": account_id, "chatId": group["id"], "msgId": msgs[-1]["id"]})
        check(d.call("contacts.list", {"accountId": account_id}), "contacts.list")
        check(d.close() == 0, "clean exit")

        # restart: account is loaded from disk and logs in
        d = Braidsd(args.braidsd, confdir)
        accounts = d.call("accounts.list")
        check([a["accountId"] for a in accounts] == [account_id], "account loaded after restart")
        wait_until(lambda: len(d.call("chats.list")) >= 2, "chats synced after restart")
        check(True, "chats available after restart")

        # removal
        d.call("accounts.remove", {"accountId": account_id})
        d.wait_event("account.removed", lambda p: p["accountId"] == account_id)
        check(d.call("accounts.list") == [], "account removed")
        check(not os.path.exists(profile_dir), "profile dir deleted")
        check(not os.path.exists(os.path.join(confdir, "history", account_id)), "history deleted")
        expect_error(lambda: d.call("chats.get", {"accountId": account_id, "chatId": "x"}), 1,
                     "removed account unknown")
        check(d.close() == 0, "clean exit after removal")

        # second instance on the same config dir is refused
        d = Braidsd(args.braidsd, confdir)
        try:
            d2 = Braidsd(args.braidsd, confdir, "second.sock")
            d2.close()
            raise AssertionError("second instance started")
        except (EOFError, OSError):
            check(True, "second instance on same config dir refused")
        d.close()
    finally:
        shutil.rmtree(confdir, ignore_errors=True)

    print("all tests passed")


if __name__ == "__main__":
    main()
