# Remote build

Build the plugins from a Mac (or any machine) using the Windows PC that has the PS3 SDK.

```
Mac: build.py  --POST /build {branch}-->  Windows: server.py
                                            git fetch + reset --hard origin/<branch>
                                            make -C ps3-plugin/game, make -C ps3-plugin/vsh
                                            commit *.sprx "[remote-build]" + push to <branch>
Mac: git pull  <--------------------------  JSON {ok, log, build_commit}
```

The server builds from its own clone (default `~/.ps3-remote-build/ps3-pdp-plugin`), never from
your working copy, so it only sees what has been **pushed**. Python 3 standard library only.

## Setup

1. Copy `config.example.json` to `config.json` on **both** machines (it is gitignored).
   - `server_host`: the Windows PC's LAN IP (`ipconfig`, e.g. `192.168.1.195`).
   - `token`: any shared secret; must match on both sides
     (`python3 -c "import secrets; print(secrets.token_hex(16))"`).
   - The `server` section is only read by the server.
2. On Windows, allow the port through the firewall (admin PowerShell, once):
   ```powershell
   New-NetFirewallRule -DisplayName "PS3 remote build" -Direction Inbound -Protocol TCP -LocalPort 8765 -RemoteAddress LocalSubnet -Action Allow
   ```
3. On Windows, git must be able to push to `origin` without prompting (credential manager).

## Run

Windows, from **Git Bash** (the SDK makefiles need `cp`/`rm` on PATH and `CELL_SDK` set):

```sh
python tools/remote-build/server.py
```

Mac:

```sh
python3 tools/remote-build/build.py           # build the current branch
python3 tools/remote-build/build.py --push    # push local commits first, then build
```

Other flags: `--branch NAME`, `--force` (rebuild even if the branch tip is already a build
commit), `--no-pull` (don't pull the binaries afterwards).

## Why it exists

The PS3 SDK is installed on a Windows PC, but the code is edited on a Mac. Instead of copying sources over and
running `make` there by hand, `build.py` asks the Windows PC to build whatever is pushed, and the built `.sprx` files
come back as a normal commit. That also means the repository always holds plugins built from its own sources, so
installing them needs no SDK.

## Notes

- `GET /health` (same token and network checks) returns `{"ok": true, "busy": ...}`, to check the server is up.
- Only one build runs at a time; a second request gets HTTP 409.
- Requests are accepted only from `allowed_networks` (default `192.168.0.0/16` and localhost)
  and must carry the token.
- `make_fself` output is not byte-identical between runs, so every build produces a new
  binaries commit. To avoid pointless commits, the server skips the build when the branch
  tip is already a `[remote-build]` commit.
- After a build, pull before committing on the Mac, or you'll need to merge/rebase over the
  binaries commit.
