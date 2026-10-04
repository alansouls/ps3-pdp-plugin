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
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
BUILD_TAG = "[remote-build]"
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")

build_lock = threading.Lock()


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


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

    def note(self, msg):
        log(msg)
        self.log.append(msg)

    def run(self, args, cwd=None, timeout=300):
        """Run a command, echoing its output to the console as it arrives."""
        cwd = cwd or self.cfg["repo_dir"]
        cmd = " ".join(args)
        self.note(f"$ {cmd}")
        start = time.monotonic()
        try:
            proc = subprocess.Popen(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace")
        except FileNotFoundError:
            raise BuildError(f"command not found: {args[0]}")
        timed_out = threading.Event()

        def kill():
            timed_out.set()
            proc.kill()

        timer = threading.Timer(timeout, kill)
        timer.start()
        lines = []
        try:
            for line in proc.stdout:
                line = line.rstrip("\r\n")
                lines.append(line)
                self.log.append(line)
                log(f"    {line}")
            proc.wait()
        finally:
            timer.cancel()
        elapsed = time.monotonic() - start
        if timed_out.is_set():
            raise BuildError(f"timed out after {timeout}s: {cmd}")
        if proc.returncode != 0:
            raise BuildError(f"exit code {proc.returncode} after {elapsed:.1f}s: {cmd}")
        if elapsed >= 1:
            log(f"    (done in {elapsed:.1f}s)")
        return "\n".join(lines).strip()

    def prepare_repo(self, branch):
        repo = self.cfg["repo_dir"]
        if not os.path.isdir(os.path.join(repo, ".git")):
            self.note(f"Workspace not found, cloning {self.cfg['remote_url']} into {repo}")
            os.makedirs(os.path.dirname(repo), exist_ok=True)
            self.run(["git", "clone", self.cfg["remote_url"], repo], cwd=os.path.dirname(repo))
        self.note(f"Updating workspace to origin/{branch}")
        self.run(["git", "fetch", "origin", "--prune"])
        self.run(["git", "checkout", "-f", "-B", branch, f"origin/{branch}"])
        self.run(["git", "reset", "--hard", f"origin/{branch}"])
        self.run(["git", "clean", "-fdx"])
        return self.run(["git", "rev-parse", "--short", "HEAD"])

    def build(self, branch, force=False):
        source = self.prepare_repo(branch)
        subject = self.run(["git", "log", "-1", "--format=%s"])
        self.note(f"Source commit: {source} \"{subject}\"")
        if not force and subject.endswith(BUILD_TAG):
            self.note("Branch tip is already a build commit, skipping (use --force to rebuild).")
            return {"source_commit": source, "build_commit": None, "pushed": False}
        for d in self.cfg["make_dirs"]:
            self.note(f"Compiling {d}")
            self.run([self.cfg["make_cmd"], "-C", d], timeout=self.cfg["make_timeout"])

        missing = [a for a in self.cfg["artifacts"]
                   if not os.path.isfile(os.path.join(self.cfg["repo_dir"], a))]
        if missing:
            raise BuildError(f"build finished but artifacts are missing: {', '.join(missing)}")

        for a in self.cfg["artifacts"]:
            size = os.path.getsize(os.path.join(self.cfg["repo_dir"], a))
            self.note(f"Built {a} ({size} bytes)")

        self.note("Committing binaries")
        self.run(["git", "add", "-f", "--"] + self.cfg["artifacts"])
        staged = self.run(["git", "diff", "--cached", "--name-only"])
        if not staged:
            self.note("Binaries unchanged, nothing to commit.")
            return {"source_commit": source, "build_commit": None, "pushed": False}

        self.run(["git", "commit", "-m", f"Build binaries for {source} {BUILD_TAG}"])
        build_commit = self.run(["git", "rev-parse", "--short", "HEAD"])
        self.note(f"Pushing {build_commit} to origin/{branch}")
        self.run(["git", "push", "origin", f"HEAD:refs/heads/{branch}"])
        return {"source_commit": source, "build_commit": build_commit, "pushed": True}


class Handler(BaseHTTPRequestHandler):
    cfg = None

    def send_json(self, status, payload):
        if status >= 400:
            log(f"[http] {self.client_address[0]} rejected ({status}): {payload.get('error')}")
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
        start = time.monotonic()
        try:
            log("=" * 60)
            log(f"[build] '{branch}' requested by {self.client_address[0]}"
                f"{' (force)' if req.get('force') else ''}")
            result = builder.build(branch, bool(req.get("force")))
            log(f"[build] '{branch}' OK in {time.monotonic() - start:.1f}s: {result}")
            self.send_json(200, {"ok": True, "branch": branch, "log": "\n".join(builder.log), **result})
        except BuildError as e:
            log(f"[build] '{branch}' FAILED after {time.monotonic() - start:.1f}s: {e}")
            self.send_json(500, {"ok": False, "branch": branch, "error": str(e),
                                 "log": "\n".join(builder.log)})
        except Exception as e:
            log(f"[build] '{branch}' crashed: {e!r}")
            self.send_json(500, {"ok": False, "branch": branch, "error": f"server error: {e!r}",
                                 "log": "\n".join(builder.log)})
        finally:
            build_lock.release()
            log("[build] waiting for requests")

    def log_message(self, fmt, *args):
        log(f"[http] {self.client_address[0]} {fmt % args}")


class Server(ThreadingHTTPServer):
    # On Windows SO_REUSEADDR lets a second server bind the same port silently.
    allow_reuse_address = os.name != "nt"


def lan_addresses():
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
        return sorted({i[4][0] for i in infos})
    except OSError:
        return []


def main():
    # Git Bash pipes stdout, which Python would otherwise block-buffer (no output until exit).
    sys.stdout.reconfigure(line_buffering=True, errors="replace")
    log(f"Loading config from {CONFIG_PATH}")
    cfg = load_config()
    Handler.cfg = cfg
    srv = cfg["server"]

    cloned = os.path.isdir(os.path.join(srv["repo_dir"], ".git"))
    log(f"Workspace:        {srv['repo_dir']}{'' if cloned else ' (will be cloned on first build)'}")
    log(f"Remote:           {srv.get('remote_url')}")
    log(f"Make dirs:        {', '.join(srv['make_dirs'])}")
    log(f"Artifacts:        {', '.join(srv['artifacts'])}")
    log(f"Allowed networks: {', '.join(srv['allowed_networks'])}")
    log(f"Token:            {'set' if cfg.get('token') else 'NOT SET - any allowed LAN client can build'}")
    log(f"CELL_SDK:         {os.environ.get('CELL_SDK') or 'NOT SET - make will fail'}")
    for tool in ("git", srv["make_cmd"], "cp"):
        log(f"{tool + ':':<17} {shutil.which(tool) or 'NOT FOUND on PATH'}")
    if not shutil.which("cp"):
        log("WARNING: 'cp' not on PATH; the SDK makefiles need it. Start the server from Git Bash.")

    try:
        httpd = Server((srv["bind"], int(cfg["port"])), Handler)
    except OSError as e:
        sys.exit(f"Cannot listen on {srv['bind']}:{cfg['port']}: {e} (is another server already running?)")
    log(f"Listening on {srv['bind']}:{cfg['port']}")
    if srv["bind"] in ("0.0.0.0", ""):
        for ip in lan_addresses():
            log(f"    reachable at http://{ip}:{cfg['port']}")
    log("[build] waiting for requests (Ctrl+C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("Stopping")


if __name__ == "__main__":
    main()
