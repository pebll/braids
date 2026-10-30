# braidsclient.py
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

"""Minimal client for the braidsd JSON-RPC API, over stdio (braidsd started
as child process) or over the Unix socket of a running braidsd."""

import json
import os
import queue
import socket
import subprocess
import threading
import time


class ApiError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class Braidsd:
    """braidsd started as child process. With socket=None it talks over stdio,
    otherwise it is started with --socket and connected to like any client."""

    def __init__(self, binary, confdir, verbose=True, socket_path=None, args=()):
        cmd = [binary, "--confdir", confdir] + list(args)
        cmd += ["--socket", socket_path] if socket_path else ["--stdio"]
        if verbose:
            cmd.append("-d")
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE if not socket_path else subprocess.DEVNULL,
                                     stdout=subprocess.PIPE if not socket_path else None, text=True)
        if socket_path:
            self.client = Client.connect(socket_path, timeout=30, proc=self.proc)
        else:
            self.client = Client(self.proc.stdout, self.proc.stdin)
        self.events = self.client.events

    def call(self, method, params=None, timeout=30):
        return self.client.call(method, params, timeout)

    def wait_event(self, method, predicate=None, timeout=30):
        return self.client.wait_event(method, predicate, timeout)

    def close(self, timeout=30):
        if self.proc.poll() is None:
            if self.client.sock:
                try:
                    self.client.call("daemon.shutdown")
                except (OSError, EOFError, ApiError):
                    pass
                self.client.close()
            else:
                self.proc.stdin.close()
            self.proc.wait(timeout=timeout)
        return self.proc.returncode


class Client:
    """JSON-RPC over a pair of line streams."""

    def __init__(self, rfile, wfile, sock=None):
        self.rfile = rfile
        self.wfile = wfile
        self.sock = sock
        self.events = queue.Queue()
        self.replies = {}
        self.cond = threading.Condition()
        self.wlock = threading.Lock()
        self.next_id = 1
        self.eof = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.wait_event("ready", timeout=30)

    @classmethod
    def connect(cls, path, timeout=0, proc=None):
        """Connect to a braidsd socket, retrying up to timeout seconds."""
        deadline = time.time() + timeout
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                break
            except OSError:
                sock.close()
                if time.time() >= deadline or (proc is not None and proc.poll() is not None):
                    raise
                time.sleep(0.1)
        f = sock.makefile("rw", encoding="utf-8", newline="\n")
        return cls(f, f, sock)

    @staticmethod
    def default_socket_path(confdir=None):
        if confdir:
            return os.path.join(confdir, "braidsd.sock")
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        if runtime:
            return os.path.join(runtime, "braids", "braidsd.sock")
        return os.path.expanduser("~/.config/braids/braidsd.sock")

    def _read(self):
        try:
            for line in self.rfile:
                msg = json.loads(line)
                if "id" in msg and "method" not in msg:
                    with self.cond:
                        self.replies[msg["id"]] = msg
                        self.cond.notify_all()
                else:
                    self.events.put(msg)
        except (OSError, ValueError):
            pass
        with self.cond:
            self.eof = True
            self.cond.notify_all()
        self.events.put(None)  # eof

    def close(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()

    def call(self, method, params=None, timeout=30):
        with self.cond:
            req_id = self.next_id
            self.next_id += 1
        req = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}
        with self.wlock:
            self.wfile.write(json.dumps(req) + "\n")
            self.wfile.flush()
        deadline = time.time() + timeout
        with self.cond:
            while req_id not in self.replies:
                if self.eof:
                    raise EOFError(f"connection closed during {method}")
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
            methods = method if isinstance(method, tuple) else (method,)
            if msg.get("method") in methods and (predicate is None or predicate(msg["params"])):
                return msg["params"]
