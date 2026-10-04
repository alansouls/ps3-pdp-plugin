#!/usr/bin/env python3
"""Remote build server: runs on the Windows machine that has the PS3 SDK.

On POST /build it checks out the requested branch from origin into a dedicated
workspace clone, runs make, commits the built binaries and pushes them back to
the same branch. Standard library only.
"""
import ipaddress
import json
import os
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
BUILD_TAG = "[remote-build]"
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")

build_lock = threading.Lock()


def load_config():
    if not os.path.exists(CONFIG_PATH):
        sys.exit(f"Missing {CONFIG_PATH} - copy config.example.json to config.json and edit it.")
    with open(CONFIG_PATH, encoding="utf-8") as f:
        cfg = json.load(f)
    srv = cfg.setdefault("server", {})
    srv.setdefault("bind", "0.0.0.0")
    srv.setdefault("repo_dir", "~/.ps3-remote-build/ps3-pdp-plugin")
    srv.setdefault("make_cmd", "make")
    srv.setdefault("make_dirs", [])
    srv.setdefault("artifacts", [])
    srv.setdefault("allowed_networks", ["192.168.0.0/16", "127.0.0.0/8"])
    srv.setdefault("make_timeout", 600)
    srv["repo_dir"] = os.path.abspath(os.path.expanduser(srv["repo_dir"]))
    return cfg


class BuildError(Exception):
    pass


class Builder:
    def __init__(self, cfg):
        self.cfg = cfg["server"]
        self.log = []

    def run(self, args, cwd=None, timeout=300):
        cwd = cwd or self.cfg["repo_dir"]
        self.log.append(f"$ {' '.join(args)}")
        try:
            p = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace", timeout=timeout)
        except FileNotFoundError:
            raise BuildError(f"command not found: {args[0]}")
        except subprocess.TimeoutExpired:
            raise BuildError(f"timed out after {timeout}s: {' '.join(args)}")
        if p.stdout.strip():
            self.log.append(p.stdout.rstrip())
        if p.returncode != 0:
            raise BuildError(f"exit code {p.returncode}: {' '.join(args)}")
        return p.stdout.strip()

    def prepare_repo(self, branch):
        repo = self.cfg["repo_dir"]
        if not os.path.isdir(os.path.join(repo, ".git")):
            os.makedirs(os.path.dirname(repo), exist_ok=True)
            self.run(["git", "clone", self.cfg["remote_url"], repo], cwd=os.path.dirname(repo))
        self.run(["git", "fetch", "origin", "--prune"])
        self.run(["git", "checkout", "-f", "-B", branch, f"origin/{branch}"])
        self.run(["git", "reset", "--hard", f"origin/{branch}"])
        self.run(["git", "clean", "-fdx"])
        return self.run(["git", "rev-parse", "--short", "HEAD"])

    def build(self, branch, force=False):
        source = self.prepare_repo(branch)
        if not force and self.run(["git", "log", "-1", "--format=%s"]).endswith(BUILD_TAG):
            self.log.append("Branch tip is already a build commit, skipping (use --force to rebuild).")
            return {"source_commit": source, "build_commit": None, "pushed": False}
        for d in self.cfg["make_dirs"]:
            self.run([self.cfg["make_cmd"], "-C", d], timeout=self.cfg["make_timeout"])

        missing = [a for a in self.cfg["artifacts"]
                   if not os.path.isfile(os.path.join(self.cfg["repo_dir"], a))]
        if missing:
            raise BuildError(f"build finished but artifacts are missing: {', '.join(missing)}")

        self.run(["git", "add", "-f", "--"] + self.cfg["artifacts"])
        staged = self.run(["git", "diff", "--cached", "--name-only"])
        if not staged:
            self.log.append("Binaries unchanged, nothing to commit.")
            return {"source_commit": source, "build_commit": None, "pushed": False}

        self.run(["git", "commit", "-m", f"Build binaries for {source} {BUILD_TAG}"])
        build_commit = self.run(["git", "rev-parse", "--short", "HEAD"])
        self.run(["git", "push", "origin", f"HEAD:refs/heads/{branch}"])
        return {"source_commit": source, "build_commit": build_commit, "pushed": True}


class Handler(BaseHTTPRequestHandler):
    cfg = None

    def send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        client = ipaddress.ip_address(self.client_address[0])
        nets = [ipaddress.ip_network(n) for n in self.cfg["server"]["allowed_networks"]]
        if not any(client in n for n in nets):
            self.send_json(403, {"ok": False, "error": f"client {client} not in allowed_networks"})
            return False
        token = self.cfg.get("token", "")
        if token and self.headers.get("Authorization") != f"Bearer {token}":
            self.send_json(401, {"ok": False, "error": "bad or missing token"})
            return False
        return True

    def do_GET(self):
        if self.path != "/health":
            return self.send_json(404, {"ok": False, "error": "not found"})
        if self.authorized():
            self.send_json(200, {"ok": True, "busy": build_lock.locked()})

    def do_POST(self):
        if self.path != "/build":
            return self.send_json(404, {"ok": False, "error": "not found"})
        if not self.authorized():
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(length) or b"{}")
        except (ValueError, json.JSONDecodeError):
            return self.send_json(400, {"ok": False, "error": "invalid JSON body"})

        branch = str(req.get("branch", "")).strip()
        if not branch or not BRANCH_RE.match(branch) or ".." in branch or branch.startswith("-"):
            return self.send_json(400, {"ok": False, "error": f"invalid branch name: {branch!r}"})

        if not build_lock.acquire(blocking=False):
            return self.send_json(409, {"ok": False, "error": "a build is already running"})
        builder = Builder(self.cfg)
        try:
            print(f"[build] {branch} requested by {self.client_address[0]}", flush=True)
            result = builder.build(branch, bool(req.get("force")))
            print(f"[build] {branch} ok: {result}", flush=True)
            self.send_json(200, {"ok": True, "branch": branch, "log": "\n".join(builder.log), **result})
        except BuildError as e:
            print(f"[build] {branch} failed: {e}", flush=True)
            self.send_json(500, {"ok": False, "branch": branch, "error": str(e),
                                 "log": "\n".join(builder.log)})
        finally:
            build_lock.release()

    def log_message(self, fmt, *args):
        print(f"[http] {self.client_address[0]} {fmt % args}", flush=True)


class Server(ThreadingHTTPServer):
    # On Windows SO_REUSEADDR lets a second server bind the same port silently.
    allow_reuse_address = os.name != "nt"


def main():
    cfg = load_config()
    Handler.cfg = cfg
    srv = cfg["server"]
    if not cfg.get("token"):
        print("WARNING: no token set in config.json; any allowed LAN client can trigger builds.")
    httpd = Server((srv["bind"], int(cfg["port"])), Handler)
    print(f"Remote build server listening on {srv['bind']}:{cfg['port']}")
    print(f"Workspace: {srv['repo_dir']}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
