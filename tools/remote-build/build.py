#!/usr/bin/env python3
"""Remote build client: asks the Windows build server to compile the current branch.

Usage: ./build.py [--push] [--branch NAME] [--force] [--no-pull]
Standard library only, works with the python3 that ships with macOS.
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")


def git(*args, check=True):
    p = subprocess.run(["git", *args], cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and p.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{p.stderr.strip()}")
    return p.stdout.strip()


def request(cfg, method, path, payload=None, timeout=900):
    url = f"http://{cfg['server_host']}:{cfg['port']}{path}"
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if cfg.get("token"):
        req.add_header("Authorization", f"Bearer {cfg['token']}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except ValueError:
            return {"ok": False, "error": f"HTTP {e.code}"}
    except (urllib.error.URLError, OSError) as e:
        sys.exit(f"Could not reach build server at {url}: {e}")


def main():
    sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description="Trigger a remote PS3 build on the Windows server.")
    ap.add_argument("--branch", help="branch to build (default: current branch)")
    ap.add_argument("--push", action="store_true", help="git push the branch before building")
    ap.add_argument("--force", action="store_true", help="rebuild even if the branch tip is already a build commit")
    ap.add_argument("--no-pull", action="store_true", help="don't pull the built binaries afterwards")
    args = ap.parse_args()

    if not os.path.exists(CONFIG_PATH):
        sys.exit(f"Missing {CONFIG_PATH} - copy config.example.json to config.json and set server_host.")
    with open(CONFIG_PATH, encoding="utf-8") as f:
        cfg = json.load(f)

    branch = args.branch or git("rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        sys.exit("Detached HEAD; pass --branch.")

    if git("status", "--porcelain", "--untracked-files=no"):
        print("Note: you have uncommitted changes; the server only builds what is pushed.")
    if args.push:
        git("push", "origin", branch)

    # The server builds origin/<branch>, so make sure local commits are pushed.
    git("fetch", "origin", branch, check=False)
    ahead = git("rev-list", "--count", f"origin/{branch}..{branch}", check=False)
    if ahead and ahead != "0":
        sys.exit(f"{branch} has {ahead} unpushed commit(s). Push first or rerun with --push.")

    print(f"Building '{branch}' on {cfg['server_host']}:{cfg['port']} ...", flush=True)
    res = request(cfg, "POST", "/build", {"branch": branch, "force": args.force})

    if res.get("log"):
        print(res["log"])
    if not res.get("ok"):
        sys.exit(f"\nBUILD FAILED: {res.get('error', 'unknown error')}")

    if res.get("pushed"):
        print(f"\nBuild OK: {res['source_commit']} -> binaries committed as {res['build_commit']}")
        if not args.no_pull and git("rev-parse", "--abbrev-ref", "HEAD") == branch:
            p = subprocess.run(["git", "pull", "--ff-only", "origin", branch], cwd=HERE)
            if p.returncode != 0:
                print(f"Could not pull automatically; run: git pull origin {branch}")
    else:
        print(f"\nBuild OK: {res['source_commit']} (binaries unchanged, nothing pushed)")


if __name__ == "__main__":
    main()
