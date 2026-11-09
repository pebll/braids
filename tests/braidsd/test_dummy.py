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
        check(d.call("accounts.reconnect")["count"] == 1, "reconnect all accounts")
        expect_error(lambda: d.call("accounts.reconnect", {"accountId": "nope"}), 1, "reconnect unknown account")

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
        pending = d.wait_event("message.added", lambda p: p["text"] == "hello" and p.get("pending"))
        check(pending["isOutgoing"] and pending["pending"] in ("waiting", "sending"), "sent message shown at once as pending")
        own = d.wait_event("message.added", lambda p: p["text"] == "hello" and not p.get("pending"))
        check(own["isOutgoing"] and own["quoted"]["id"] == msgs[-1]["id"], "sent message echoed with quote")
        d.wait_event("message.removed", lambda p: p["msgId"] == pending["id"])
        check(True, "the pending message gives way to the echo")
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
        # polls: sent, own vote counted at once, others' votes (also one that
        # came before its poll), ended
        check(account["capabilities"]["polls"], "polls capability")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"],
                                 "poll": {"question": "Meeting day?", "options": ["Monday", " Friday ", ""],
                                          "multiple": True}})
        pending = d.wait_event("message.added", lambda p: (p.get("poll") or {}).get("question") == "Meeting day?"
                               and p.get("pending"))
        check([o["text"] for o in pending["poll"]["options"]] == ["Monday", "Friday"], "pending poll, options trimmed")
        sent = d.wait_event("message.added", lambda p: (p.get("poll") or {}).get("question") == "Meeting day?"
                            and not p.get("pending"))
        d.wait_event("message.removed", lambda p: p["msgId"] == pending["id"])
        check(sent["poll"]["multiple"] and [o["id"] for o in sent["poll"]["options"]] == ["0", "1"], "poll sent")
        d.call("messages.vote", {"accountId": account_id, "chatId": group["id"], "msgId": sent["id"],
                                 "optionIds": ["0", "1"]})
        voted = d.wait_event("message.updated", lambda p: p["id"] == sent["id"])
        check(voted["poll"]["own"] == ["0", "1"] and voted["poll"]["total"] == 1
              and [o["votes"] for o in voted["poll"]["options"]] == [1, 1], "own vote counted")
        expect_error(lambda: d.call("messages.vote", {"accountId": account_id, "chatId": group["id"],
                                                      "msgId": sent["id"], "optionIds": ["7"]}),
                     -32602, "unknown option rejected")
        expect_error(lambda: d.call("messages.send", {"accountId": account_id, "chatId": group["id"],
                                                      "poll": {"question": "x", "options": ["only one"]}}),
                     -32602, "poll with one option rejected")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/poll"})
        lunch = d.wait_event("message.added", lambda p: (p.get("poll") or {}).get("question") == "Lunch?")
        check(not lunch["isOutgoing"] and lunch["poll"]["options"][1]["votes"] == 1
              and lunch["poll"]["options"][1]["voters"] == ["Jim_0"], "a vote that came before its poll is counted")
        expect_error(lambda: d.call("messages.vote", {"accountId": account_id, "chatId": group["id"],
                                                      "msgId": lunch["id"], "optionIds": ["0", "1"]}),
                     -32602, "single-choice poll takes one option")
        d.call("messages.vote", {"accountId": account_id, "chatId": group["id"], "msgId": lunch["id"],
                                 "optionIds": ["0"]})
        d.wait_event("message.updated", lambda p: p["id"] == lunch["id"] and p["poll"]["own"] == ["0"])
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/vote 0"})
        two = d.wait_event("message.updated", lambda p: p["id"] == lunch["id"] and p["poll"]["total"] == 3)
        check(two["poll"]["options"][0]["votes"] == 2 and two["poll"]["options"][0]["voters"] == ["Pam_0"],
              "others' votes counted with voters")
        d.call("messages.vote", {"accountId": account_id, "chatId": group["id"], "msgId": lunch["id"], "optionIds": []})
        d.wait_event("message.updated", lambda p: p["id"] == lunch["id"] and p["poll"]["own"] == []
                     and p["poll"]["total"] == 2)
        check(True, "own vote retracted")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/endpoll"})
        d.wait_event("message.updated", lambda p: p["id"] == lunch["id"] and p["poll"]["closed"])
        expect_error(lambda: d.call("messages.vote", {"accountId": account_id, "chatId": group["id"],
                                                      "msgId": lunch["id"], "optionIds": ["0"]}),
                     -32602, "no votes in an ended poll")
        check(True, "poll ended")

        # "joined Telegram": off by default, no message and no new chat until someone writes
        accounts = d.call("accounts.list")
        check(all(a["options"]["showJoinedMessages"] is False for a in accounts), "joined messages off by default")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/joined"})
        try:
            ev = d.wait_event(("message.added", "chat.updated", "notification"),
                              lambda p: "Holly_0" in (p.get("chatId"), p.get("id")), timeout=1.5)
            check(False, f"the joined message is left out: {ev}")
        except TimeoutError:
            check(True, "the joined message is left out")
        check(not any(c["id"] == "Holly_0" for c in d.call("chats.list", {"accountId": account_id})),
              "a chat with only a joined message is not listed")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/joined hi there"})
        d.wait_event("chat.updated", lambda p: p["id"] == "Holly_0")
        check(any(c["id"] == "Holly_0" for c in d.call("chats.list", {"accountId": account_id})),
              "listed once someone writes")
        d.call("accounts.setOption", {"accountId": account_id, "name": "showJoinedMessages", "value": True})
        check(d.call("accounts.list")[0]["options"]["showJoinedMessages"] is True, "option set")
        d.call("accounts.setOption", {"accountId": account_id, "name": "showJoinedMessages", "value": False})
        expect_error(lambda: d.call("accounts.setOption", {"accountId": account_id, "name": "nope", "value": True}),
                     -32602, "unknown option rejected")

        expect_error(lambda: d.call("messages.send", {"accountId": account_id, "chatId": group["id"]}),
                     -32602, "send without text or file rejected")

        # outbox: offline, sending fails; the message waits and goes out on reconnect
        chat = {"accountId": account_id, "chatId": group["id"]}
        d.call("messages.send", {**chat, "text": "/offline 2"})
        d.wait_event("account.state", lambda p: p["accountId"] == account_id and p["state"] != "online")
        r = d.call("messages.send", {**chat, "text": "while offline"})
        check(r["msgId"].startswith("braids-out-"), "send returns the local id")
        waiting = d.wait_event("message.added", lambda p: p["id"] == r["msgId"])
        check(waiting["pending"] == "waiting", "offline: the message waits")
        page = d.call("messages.get", {**chat, "limit": 5})
        check(any(m["id"] == r["msgId"] and m["pending"] == "waiting" for m in page["messages"]),
              "the newest page lists the waiting message")
        d.wait_event("account.state", lambda p: p["accountId"] == account_id and p["state"] == "online", timeout=10)
        d.wait_event("message.added", lambda p: p["text"] == "while offline" and not p.get("pending"))
        d.wait_event("message.removed", lambda p: p["msgId"] == r["msgId"])
        check(True, "back online: sent, echoed, the pending one removed")

        # a waiting message can be deleted; it is not sent later
        expect_error(lambda: d.call("messages.resend", {**chat, "msgId": "braids-out-1"}), 3, "resend of an unknown message")
        d.call("messages.send", {**chat, "text": "/offline 2"})
        d.wait_event("account.state", lambda p: p["accountId"] == account_id and p["state"] != "online")
        r2 = d.call("messages.send", {**chat, "text": "doomed"})
        d.call("messages.delete", {**chat, "msgId": r2["msgId"]})
        d.wait_event("message.removed", lambda p: p["msgId"] == r2["msgId"])
        d.wait_event("account.state", lambda p: p["accountId"] == account_id and p["state"] == "online", timeout=10)
        time.sleep(0.5)
        page = d.call("messages.get", {**chat, "limit": 5})
        check(not any(m["text"] == "doomed" for m in page["messages"]), "a deleted waiting message is not sent")
        check(True, "an outbox message can be deleted")
        expect_error(lambda: d.call("messages.edit", {"accountId": account_id, "chatId": group["id"],
                                                      "msgId": msgs[-1]["id"], "text": "x"}),
                     4, "editing others' messages unsupported")
        expect_error(lambda: d.call("chats.pin", {"accountId": account_id, "chatId": group["id"]}),
                     4, "pin unsupported by capabilities")
        expect_error(lambda: d.call("chats.list", {"accountId": 5}), -32602, "invalid param type rejected")
        d.call("messages.markRead", {"accountId": account_id, "chatId": group["id"], "msgId": msgs[-1]["id"]})
        check(d.call("contacts.list", {"accountId": account_id}), "contacts.list")

        # group events are flagged as system messages
        full = d.call("messages.get", {"accountId": account_id, "chatId": group["id"], "limit": 50})["messages"]
        joined = [m for m in full if m["text"] == "[Joined]"]
        check(joined and all(m["system"] == "joined" for m in joined), "group event marked as system message")
        check(all(m["system"] == "" for m in full if m["text"] != "[Joined]"), "normal messages not system")

        # unread counts: Kevin has two unread messages
        kevin = wait_until(lambda: [c for c in d.call("chats.list") if c["id"] == "Kevin_0" and c["unreadCount"] == 2],
                           "unread count of Kevin's chat")[0]
        check(kevin["isUnread"] and kevin["unreadCount"] == 2, "unread count")
        check(all(c["unreadCount"] == 0 for c in d.call("chats.list") if not c["isUnread"]), "read chats count 0")
        party = [c for c in d.call("chats.list") if c["name"] == "Party Planning"][0]
        check(party["isUnread"] and party["unreadCount"] == 0 and party["unreadEvents"] == 1,
              "group events counted apart from messages")
        check(kevin["unreadEvents"] == 0, "no events in Kevin's chat")

        # marked unread in braids until read
        d.call("chats.markUnread", {"accountId": account_id, "chatId": group["id"]})
        marked = d.wait_event("chat.updated", lambda p: p["id"] == group["id"] and p["isUnread"])
        check(marked["unreadCount"] == 1, "chat marked unread")
        last = d.call("messages.get", {"accountId": account_id, "chatId": group["id"], "limit": 1})["messages"][-1]
        d.call("messages.markRead", {"accountId": account_id, "chatId": group["id"], "msgId": last["id"]})
        d.wait_event("chat.updated", lambda p: p["id"] == group["id"] and not p["isUnread"])
        check(True, "reading clears the unread mark")

        # shared media: per chat and across all chats
        img = os.path.join(confdir, "pic.png")
        with open(img, "wb") as f:
            f.write(bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d"
                                  "49444154789c6360000002000154a24f5d0000000049454e44ae426082"))
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "filePath": img})
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "see https://example.org"})
        d.wait_event("message.added", lambda p: p["text"] == "see https://example.org")
        media = d.call("media.list", {"accountId": account_id, "chatId": group["id"], "kind": "media"})
        check(len(media["items"]) == 1 and media["items"][0]["file"]["mimeType"] == "image/png"
              and media["nextBefore"] == 0, "media.list finds the image")
        links = d.call("media.list", {"kind": "links"})["items"]
        check([m["text"] for m in links] == ["see https://example.org"], "media.list links across all chats")
        check(d.call("media.list", {"accountId": account_id, "kind": "files"})["items"] == [], "no files")
        expect_error(lambda: d.call("media.list", {"kind": "photos"}), -32602, "unknown media kind rejected")
        paged = d.call("media.list", {"kind": "links", "limit": 1, "beforeTime": links[0]["time"]})
        check(paged["items"] == [], "paging past the last item")

        # pinned messages and message search
        link_msg = links[0]
        d.call("messages.pin", {"accountId": account_id, "chatId": group["id"], "msgId": link_msg["id"]})
        d.wait_event("message.updated", lambda p: p["id"] == link_msg["id"] and p["isPinned"])
        pinned = d.call("media.list", {"accountId": account_id, "chatId": group["id"], "kind": "pinned"})["items"]
        check([m["id"] for m in pinned] == [link_msg["id"]], "media.list pinned")
        found = d.call("messages.search", {"text": "EXAMPLE.org"})["items"]
        check([m["id"] for m in found] == [link_msg["id"]], "messages.search, case-insensitive, all chats")
        check(d.call("messages.search", {"text": "100%"})["items"] == [], "search escapes LIKE wildcards")
        expect_error(lambda: d.call("messages.search", {"text": ""}), -32602, "empty search rejected")

        # stickers and GIFs are marked by kind
        sticker = os.path.join(confdir, "smile.sticker.png")
        with open(sticker, "wb") as f:
            f.write(open(img, "rb").read())
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "filePath": sticker})
        added = d.wait_event("message.added", lambda p: (p.get("file") or {}).get("name") == "smile.sticker.png")
        check(added["file"]["kind"] == "sticker", "file kind sticker")
        check(all(m["file"]["kind"] != "sticker" for m in
                  d.call("media.list", {"accountId": account_id, "chatId": group["id"], "kind": "media"})["items"]),
              "stickers are not shared media")

        # a new group
        check(account["capabilities"]["createGroup"], "createGroup capability")
        d.call("chats.createGroup", {"accountId": account_id, "name": "Road trip ", "userIds": ["Jim_0", "Pam_0"]})
        created = d.wait_event("chat.groupCreated", lambda p: p["name"] == "Road trip")
        check(created["success"] and created["chatId"], "chat.groupCreated")
        wait_until(lambda: any(c["id"] == created["chatId"] and c["isGroup"] for c in d.call("chats.list")),
                   "new group listed")
        expect_error(lambda: d.call("chats.createGroup", {"accountId": account_id, "name": "x", "userIds": []}),
                     -32602, "group without members rejected")
        expect_error(lambda: d.call("chats.createGroup", {"accountId": account_id, "name": " ", "userIds": ["Jim_0"]}),
                     -32602, "group without a name rejected")

        # profile pictures: small ones for the avatars, the full-size one for the viewer
        small = d.call("avatars.get", {"accountId": account_id, "ids": ["Pam_0"]})["avatars"].get("Pam_0")
        if small is None:
            small = d.wait_event("avatar.updated", lambda p: p["id"] == "Pam_0")["path"]
        check(small and "-full" not in small, "small avatar")
        r = d.call("avatars.getFull", {"accountId": account_id, "id": "Pam_0"})
        full = r["path"] if "path" in r else d.wait_event("avatar.full", lambda p: p["id"] == "Pam_0")["path"]
        check(full.endswith("Pam_0-full.svg") and os.path.exists(full), "avatar.full with the full-size picture")
        check(d.call("avatars.getFull", {"accountId": account_id, "id": "Pam_0"}).get("path") == full,
              "full-size picture answered from the cache")
        d.call("avatars.getFull", {"accountId": account_id, "id": "Kevin_0"})
        check(d.wait_event("avatar.full", lambda p: p["id"] == "Kevin_0")["path"] == "", "no picture: empty path")

        # braids' own mute setting, kept across restarts
        check(account["capabilities"]["muteChat"], "mute capability")
        check(not group["isMuted"], "group not muted initially")
        d.call("chats.mute", {"accountId": account_id, "chatId": group["id"], "isMuted": True})
        d.wait_event("chat.updated", lambda p: p["id"] == group["id"] and p["isMuted"])
        check(d.call("chats.get", {"accountId": account_id, "chatId": group["id"]})["isMuted"], "chat muted")
        check(os.stat(os.path.join(profile_dir, "braids-chats.json")).st_mode & 0o077 == 0, "mute file private")
        expect_error(lambda: d.call("chats.mute", {"accountId": account_id, "chatId": "nope"}), 2,
                     "mute unknown chat rejected")
        # a message waiting while offline is kept across a restart, then sent
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "/offline 30"})
        d.wait_event("account.state", lambda p: p["accountId"] == account_id and p["state"] != "online")
        d.call("messages.send", {"accountId": account_id, "chatId": group["id"], "text": "kept for later"})
        outbox_file = os.path.join(profile_dir, "braids-outbox.json")
        wait_until(lambda: os.path.exists(outbox_file), "outbox written")
        check(os.stat(outbox_file).st_mode & 0o077 == 0, "outbox file private")
        check(d.close() == 0, "clean exit")

        # restart: account is loaded from disk and logs in
        d = Braidsd(args.braidsd, confdir)
        accounts = d.call("accounts.list")
        check([a["accountId"] for a in accounts] == [account_id], "account loaded after restart")
        wait_until(lambda: len(d.call("chats.list")) >= 2, "chats synced after restart")
        check(True, "chats available after restart")
        wait_until(lambda: any(m["text"] == "kept for later" and not m.get("pending") for m in
                               d.call("messages.get", {"accountId": account_id, "chatId": group["id"], "limit": 5})["messages"]),
                   "the waiting message is sent after the restart")
        wait_until(lambda: not os.path.exists(outbox_file), "outbox emptied")
        group = d.call("chats.get", {"accountId": account_id, "chatId": group["id"]})
        check(group["isMuted"], "mute kept after restart")
        d.call("chats.mute", {"accountId": account_id, "chatId": group["id"], "isMuted": False})
        d.wait_event("chat.updated", lambda p: p["id"] == group["id"] and not p["isMuted"])
        check(True, "chat unmuted")

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
