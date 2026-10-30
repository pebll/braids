# braidsclient.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""Minimal client for the braidsd JSON-RPC API over stdio (test harness)."""

import json
import queue
import subprocess
import threading
import time


class ApiError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class Braidsd:
    def __init__(self, binary, confdir, verbose=True):
        args = [binary, "--stdio", "--confdir", confdir]
        if verbose:
            args.append("-d")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.events = queue.Queue()
        self.replies = {}
        self.cond = threading.Condition()
        self.next_id = 1
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.wait_event("ready", timeout=30)

    def _read(self):
        for line in self.proc.stdout:
            msg = json.loads(line)
            if "id" in msg and "method" not in msg:
                with self.cond:
                    self.replies[msg["id"]] = msg
                    self.cond.notify_all()
            else:
                self.events.put(msg)
        self.events.put(None)  # eof

    def call(self, method, params=None, timeout=30):
        with self.cond:
            req_id = self.next_id
            self.next_id += 1
        req = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        with self.cond:
            while req_id not in self.replies:
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"no reply to {method}")
                self.cond.wait(remaining)
            reply = self.replies.pop(req_id)
        if "error" in reply:
            raise ApiError(reply["error"]["code"], reply["error"]["message"])
        return reply["result"]

    def wait_event(self, method, predicate=None, timeout=30):
        """Wait for an event, discarding others (they remain available via seen)."""
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(f"no event {method}")
            try:
                msg = self.events.get(timeout=remaining)
            except queue.Empty:
                continue
            if msg is None:
                self.events.put(None)
                raise EOFError(f"braidsd exited while waiting for {method}")
            if msg.get("method") == method and (predicate is None or predicate(msg["params"])):
                return msg["params"]

    def close(self, timeout=30):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=timeout)
        return self.proc.returncode
