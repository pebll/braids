#!/usr/bin/env python3
# test_cli.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""utils/braids-cli against a braidsd with a Dummy account."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from braidsclient import Braidsd  # noqa: E402


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"ok - {msg}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--braidsd", required=True)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()

    confdir = tempfile.mkdtemp(prefix="braids-cli-")

    def cli(*cli_args):
        return subprocess.run([sys.executable, args.cli, "--confdir", confdir] + list(cli_args),
                              capture_output=True, text=True, timeout=60)

    try:
        res = cli("status")
        check(res.returncode == 3 and "no braidsd" in res.stderr, "no daemon: exit 3")

        d = Braidsd(args.braidsd, confdir, socket_path=os.path.join(confdir, "braidsd.sock"))
        setup_id = d.call("accounts.add", {"protocol": "Dummy"})["setupId"]
        d.wait_event("account.auth", lambda p: p.get("field") == "phone")
        d.call("accounts.authInput", {"setupId": setup_id, "field": "phone", "value": "+1 555"})
        d.wait_event("account.auth", lambda p: p.get("field") == "code")
        d.call("accounts.authInput", {"setupId": setup_id, "field": "code", "value": "1234"})
        account_id = d.wait_event("account.added", lambda p: p["setupId"] == setup_id)["account"]["accountId"]

        res = cli("status")
        check(res.returncode == 0 and "Dummy" in res.stdout and account_id in res.stdout, "status")
        res = cli("accounts")
        check(res.stdout.startswith(account_id + "\tDummy"), "accounts")

        deadline = time.time() + 30
        while time.time() < deadline:
            res = cli("--json", "chats", account_id)
            chats = json.loads(res.stdout)
            if len(chats) >= 2:
                break
            time.sleep(0.2)
        check(len(chats) >= 2, "chats --json")
        chat_id = chats[0]["id"]
        check(chat_id in cli("chats").stdout, "chats table")

        res = cli("messages", account_id, chat_id, "3")
        check(res.returncode == 0 and len(res.stdout.splitlines()) == 3, "messages with limit")

        events = subprocess.Popen([sys.executable, args.cli, "--confdir", confdir, "events", "message.added"],
                                  stdout=subprocess.PIPE, text=True)
        time.sleep(0.5)
        res = cli("send", account_id, chat_id, "hello", "from", "cli")
        check(res.returncode == 0, "send")
        line = json.loads(events.stdout.readline())
        check(line["params"]["text"] == "hello from cli", "events stream shows sent message")
        events.terminate()
        events.wait(timeout=10)

        res = cli("call", "chats.get", f"accountId={account_id}", f"chatId={chat_id}")
        check(res.returncode == 0 and json.loads(res.stdout)["id"] == chat_id, "call with key=value")
        res = cli("call", "messages.get", json.dumps({"accountId": account_id, "chatId": chat_id, "limit": 1}))
        check(len(json.loads(res.stdout)["messages"]) == 1, "call with json params")
        res = cli("call", "chats.get", f"accountId={account_id}", "chatId=nope")
        check(res.returncode == 1 and "error 2" in res.stderr, "api error: exit 1")
        res = cli("call", "hello", "oops")
        check(res.returncode == 2, "bad params: exit 2")

        res = cli("shutdown")
        check(res.returncode == 0, "shutdown")
        d.proc.wait(timeout=30)
        check(d.proc.returncode == 0, "daemon exited")
        d.client.close()
    finally:
        shutil.rmtree(confdir, ignore_errors=True)

    print("all tests passed")


if __name__ == "__main__":
    main()
