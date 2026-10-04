# Riffmaster for PS3

Use a **PDP Riffmaster (PS4)** guitar in PS3 Guitar Hero and Rock Band games. The guitar plugs into the PS3 by USB,
and a pair of Cobra plugins makes the game see it as an official PS3 guitar.

## Compatibility

**Tested and working:**

- Guitar Hero: Metallica
- Guitar Hero: Greatest Hits
- Guitar Hero: Warriors of Rock

**Should work, not tested yet:** the other PS3 Guitar Hero games (Guitar Hero III, World Tour, Guitar Hero 5,
Van Halen, Band Hero) and Rock Band 1–3. The plugin doesn't contain anything game-specific. It works with any game
that reads controllers through the PS3's standard pad library (`cellPadGetData`) from its main executable, which
the three tested games do. Rock Band titles automatically get the Rock Band guitar ID; see `profile` under
[Configuration](#configuration). If you try another game, the log shows whether it was hooked; see
[Troubleshooting](#troubleshooting).

**Requirements:**

- A PS3 on **Evilnat CFW with Cobra**. The plugins rely on Cobra's PS3MAPI to load the game plugin into the game.
- **webMAN MOD** is recommended. The plugins don't need it, but it is the easiest way to launch games, copy files
  over FTP and read the log.
- A DualShock 3 for the XMB. The guitar's PS button is not forwarded.

## Install

The built plugins are committed to this repository, so you don't need to build anything to install them.

1. Copy both plugins to `/dev_hdd0/plugins/` on the PS3 (e.g. over webMAN's FTP):
   - `ps3-plugin/vsh/riffmaster_loader.sprx`
   - `ps3-plugin/game/riffmaster_game.sprx`
2. Optional: copy `ps3-plugin/riffmaster.cfg` to `/dev_hdd0/plugins/riffmaster.cfg` and edit it (see
   [Configuration](#configuration)).
3. Add this line to `/dev_hdd0/boot_plugins.txt`, keeping the lines already there (such as webMAN's
   `webftp_server.sprx`). `ps3-plugin/boot_plugins.txt` is an example.
   ```
   /dev_hdd0/plugins/riffmaster_loader.sprx
   ```
4. Reboot. "Riffmaster loader loaded" appears once the XMB is up.
5. Plug the Riffmaster into a USB port and start the game. After about 8 seconds you should see
   "Riffmaster plugin loaded (Guitar Hero guitar)" and then "Riffmaster guitar connected".

The game plugin only needs to stay in `/dev_hdd0/plugins/`. Don't add it to `boot_plugins.txt`; the loader puts it
into each game itself.

## Configuration

`/dev_hdd0/plugins/riffmaster.cfg` is optional. `profile` is read each time a game starts. The other settings are
read when the loader starts at boot, so reboot after changing them. Lines starting with `#` are comments.

| Setting | Values | Default |
| --- | --- | --- |
| `profile` | `auto`: Rock Band titles (any game whose `PARAM.SFO` contains "Rock Band") get the Rock Band guitar ID `12BA:0200`, every other game the Guitar Hero guitar ID `12BA:0100`. `gh` or `rb`: always use that ID. | `auto` |
| `notifications` | `2`: show all XMB notifications. `1`: only show failures. `0`: show none. Every message is written to the log either way. | `2` |
| `debug_stage` | `0`–`2`: for tracking down a console freeze, see [`ps3-plugin/README.md`](ps3-plugin/README.md#finding-what-freezes-the-console). | `2` (normal) |
| `trace` | `1`: log every call the loader makes while polling, for the same purpose. | `0` |

The sample `ps3-plugin/riffmaster.cfg` sets `profile=gh`. Change it to `auto` (or remove the line) before playing
Rock Band.

### Button mapping

| Riffmaster | Sent to the game as |
| --- | --- |
| Green / Red / Yellow / Blue / Orange | Cross / Circle / Square / Triangle / L1 |
| Strum up / down | D-pad up / down |
| Start / Select | Start / Select |
| Whammy | Right stick X |
| Tilt | Motion sensor X. Tilting past the threshold also presses Select, which deploys star power or overdrive. |
| PS button | Not forwarded |

## How it works

There are two plugins. `riffmaster_loader.sprx` is a VSH plugin that runs all the time from boot.
`riffmaster_game.sprx` is loaded into each game by the loader.

1. **Game detection.** The loader asks the XMB system software (VSH) whether a game is running and gets its process
   ID, the same way webMAN MOD does.
2. **Injection.** Eight seconds after a game starts, the loader loads the game plugin into the game with Cobra's
   PS3MAPI. It passes the game plugin the address of a status block in the loader's own memory.
3. **Hooking.** The game plugin redirects the game's calls to the pad library (`cellPadGetData`,
   `cellPadGetInfo`, `cellPadGetInfo2`) to its own functions. The first time the game calls one of them, the game
   plugin starts its worker thread.
4. **USB.** The worker thread registers a USB driver for the Riffmaster, reads its 64-byte input reports and decodes
   frets, strum, whammy and tilt (the report layout is in [`riffmaster_report_map.md`](riffmaster_report_map.md)).
   It also registers a virtual pad, which gives the guitar its own controller port.
5. **Input.** When the game asks the pad library what is connected, the hooks report the virtual pad as an
   official PS3 guitar. When the game reads that port, the hook hands it the guitar's current state.
6. **Status and logs.** The game plugin can't write files or show notifications from inside the game, so it
   writes its progress and log lines to memory. The loader reads them with PS3MAPI, writes the log file and shows
   the XMB notifications.

[`ps3-plugin/README.md`](ps3-plugin/README.md) explains each part in detail, including why it is done this way.

## Build

The plugins are built with the official PS3 SDK. If you don't change the code, you don't need to build: the
current `.sprx` files are committed.

### With the PS3 SDK on the same machine

The SDK makefiles need `make`, `cp` and `rm` on the `PATH` (on Windows, use Git Bash), `CELL_SDK` pointing at the
SDK, and `make_fself` on the `PATH`. From the repository root:

```sh
export CELL_SDK=/path/to/cell
make -C ps3-plugin/game    # -> ps3-plugin/game/riffmaster_game.sprx
make -C ps3-plugin/vsh     # -> ps3-plugin/vsh/riffmaster_loader.sprx
```

### With the remote build server

Here the PS3 SDK is installed on a Windows PC, while development happens on a Mac. The remote build server in
[`tools/remote-build/`](tools/remote-build/) lets you build from the Mac without copying files around:

1. On the Windows PC that has the SDK, `server.py` listens on the LAN (port 8765).
2. On the development machine, `build.py` asks it to build the current branch.
3. The server builds from its own clone of the repository (not your working copy), so it only sees **pushed**
   commits. It updates that clone to the branch, runs `make` for both plugins, commits the two `.sprx` files as
   `Build binaries for <commit> [remote-build]` and pushes that commit to the same branch.
4. `build.py` prints the build log and pulls the binaries commit.

```sh
python3 tools/remote-build/build.py          # build the current branch (push first)
python3 tools/remote-build/build.py --push   # push, then build
```

Requests need a shared token and must come from the LAN. Setup (config, firewall rule, starting the server from
Git Bash) and the remaining options are in [`tools/remote-build/README.md`](tools/remote-build/README.md).

## Troubleshooting

Both plugins log to `/dev_hdd0/tmp/riffmaster.log`. With webMAN you can open it at
`http://<ps3-ip>/dev_hdd0/tmp/riffmaster.log`. Each boot starts a new log; the previous one is kept as
`riffmaster.old.log`, so after a freeze and a hard power-off, look there.

- **No "Riffmaster loader loaded":** check the `boot_plugins.txt` line and that the file is in
  `/dev_hdd0/plugins/`. With `notifications=0` or `1`, this notification is not shown.
- **"guitar plugin didn't report back" or "plugin stopped after '...'":** the game plugin didn't finish starting.
  The log lists each step the game plugin reached.
- **Plugin loaded, but the game doesn't react to the guitar:** in the log, `hook ... installed` lines show which
  pad functions the game uses, and the game plugin's heartbeat shows `frames handed to the game N`. If no hook is
  installed, the game reads controllers in a way the plugin doesn't handle yet.

[`ps3-plugin/README.md`](ps3-plugin/README.md#debugging) describes the log in detail.

## Known limitations

- The PS button is not forwarded.
- The USB driver accepts any PDP device (vendor `0E6F`). The Riffmaster is `0E6F:024A`; if you have other PDP
  devices plugged in, unplug them while playing.
- Unloading the game plugin while the game is still running (e.g. from webMAN's Game Plugins page) is not supported
  and may hang the game. Quitting the game normally is fine.

## Repository layout

| Path | Contents |
| --- | --- |
| `ps3-plugin/vsh/` | The loader (VSH plugin) and its makefile |
| `ps3-plugin/game/` | The game plugin and its makefile |
| `ps3-plugin/common/` | Code shared by both: PS3MAPI calls, logging, the status and log formats |
| `ps3-plugin/riffmaster.cfg`, `ps3-plugin/boot_plugins.txt` | Example config and boot plugin list |
| `tools/remote-build/` | The remote build server and client |
| `riffmaster_report_map.md` | The Riffmaster's USB input report, byte by byte |
| `hid_inspect.py`, `dump.txt` | The tool used to capture the report on a PC (`pip install hidapi`), and a capture |
