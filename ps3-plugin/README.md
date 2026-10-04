# Riffmaster → PS3 guitar plugin

Makes a **PDP Riffmaster (PS4)** guitar work as a PS3 guitar in Guitar Hero III, World Tour, GH5,
Metallica, Smash Hits, Warriors of Rock and Rock Band 1–3. It needs Evilnat CFW with Cobra and webMAN MOD.
The guitar plugs into the PS3 directly over USB.

## How it works

| Piece | Runs in | Job |
| --- | --- | --- |
| `riffmaster_loader.sprx` | VSH (`boot_plugins.txt`) | Asks VSH for the running game's process ID (`GetGameProcessID`, as webMAN MOD does) and injects the game plugin through Cobra PS3MAPI after 8 s. |
| `riffmaster_game.sprx` | Game process | Opens the Riffmaster through `cellUsbd`. Converts each report using `../riffmaster_report_map.md`. Feeds a virtual pad through `cellPadLdd`. Redirects the game's `cellPadGetInfo/GetInfo2/PeriphGetInfo/PeriphGetData` imports so that pad reports as a guitar. |

### How the game plugin starts

Cobra runs the game plugin's `module_start` on a thread its kernel code creates. Calls into liblv2 never return on
that thread (`sys_ppu_thread_get_id`, `sys_ppu_thread_create`), so `module_start` only makes direct syscalls: it
takes the loader's argument, reports to the loader, and hooks the game's `cellPad` imports (including
`cellPadGetData`, which games call every frame). The first hooked call runs on one of the game's own threads, and
that is where the plugin starts `riff_thread`, which does all the logging and USB setup.

### XMB notifications

| When | Message |
| --- | --- |
| Loader starts (after the XMB appears, or 30 s at most) | `Riffmaster loader loaded` |
| A game process is detected | `Riffmaster: game found, loading guitar plugin in 8s` |
| `ps3mapi_load_proc_module` fails | `Riffmaster: failed to load guitar plugin (0x...)` |
| Game plugin finished setup | `Riffmaster plugin loaded (Guitar Hero guitar)` / `(Rock Band guitar)`, or `... but USB setup failed (0x...)` |
| Game plugin attached the guitar | `Riffmaster guitar connected` / `Riffmaster guitar attach failed (0x...)` |
| Setup not finished 20 s after injection | `Riffmaster: guitar plugin didn't report back`, or `Riffmaster: plugin stopped after '<step>' (0x...)` |

Notifications can only be shown from VSH, which the loader runs in. It finds `vshtask_notify` in the VSH export
table the same way webMAN MOD does. The game plugin can't call it from inside the game process, so it reports to the
loader instead (see `common/status.h`):

1. When injecting, the loader passes its process ID and the address of a status struct as `module_start`'s argument.
2. After each setup step, the game plugin writes that struct into the loader's memory with PS3MAPI `SET_PROC_MEM`.
3. The loader logs every step as `game plugin: step N (...)` and shows the notifications above.

This doesn't touch the filesystem, so it works even if the game process can't write the log. The status also
carries the error code from the game plugin's last attempt to open the log file (`game log open error`).

VSH plugins reload when you quit a game, so "loader loaded" also appears each time you return to the XMB.

### Button mapping

| Riffmaster | PS3 guitar |
| --- | --- |
| Green / Red / Yellow / Blue / Orange | Cross / Circle / Square / Triangle / L1 |
| Strum up / down | D-pad up / down |
| Start / Select | Start / Select |
| Whammy (byte 44) | Right stick X |
| Tilt (byte 45) | Sensor X. Past `0x80` it also presses Select, which deploys star power or overdrive. |
| PS button | Not forwarded. Use a DS3 for the XMB. |

The guitar ID depends on the game. Titles whose `PARAM.SFO` contains "Rock Band" get `0x12BA:0x0200` (RB guitar). Every other game gets `0x12BA:0x0100` (GH guitar). Override this with `riffmaster.cfg`.

## Build

Requires the official PS3 SDK (`CELL_SDK` set, with `make_fself` on the PATH).

```sh
export CELL_SDK=/path/to/cell
make -C game
make -C vsh
```

## Install

1. Copy `game/riffmaster_game.sprx` and `vsh/riffmaster_loader.sprx` to `/dev_hdd0/plugins/`.
2. Optional: copy `riffmaster.cfg` to `/dev_hdd0/plugins/`.
3. Add this line to `/dev_hdd0/boot_plugins.txt`:
   ```
   /dev_hdd0/plugins/riffmaster_loader.sprx
   ```
4. Reboot. Plug the Riffmaster in by USB, then start the game from webMAN.

## Debugging

Both plugins log to `/dev_hdd0/tmp/riffmaster.log`. Fetch it with webMAN at
`http://<ps3-ip>/dev_hdd0/tmp/riffmaster.log`.

The game process isn't allowed to open that file (`EACCES`, `0x80010029`). The game plugin writes its lines into a
64 KB buffer in its own memory instead, and the loader copies new lines into the file on every poll (every 1–2 s),
so `game` lines can appear slightly after `loader` lines from the same moment. Nothing from the game plugin is
logged before its thread starts (see "How the game plugin starts"). The thread then logs which `sys_io` functions
the game imports and which hooks were installed. The game plugin's heartbeat counts the calls to each hook
(`hook calls: GetData N GetInfo N ...`), and each hook logs its first 5 calls, then every 1000th.

- **One log per boot.** When the loader starts within 90 s of power-on, it moves the previous
  log to `riffmaster.old.log` and starts a fresh one. After a freeze and a hard power-off, the
  log for the run that froze is in **`riffmaster.old.log`**.
- **Survives power loss.** Each line is `fsync`ed before the plugin moves on. The last line in
  the log is the last thing the plugin finished before the console died.
- **Size limit.** Once the log reaches 1 MB it is emptied and restarted with a
  `log reached size limit` marker, so the newest lines are kept.
- **Line format.** `[uptime s.us] loader|game t<thread id>: message`.
- **Heartbeats.** The loader logs once a second for 90 s after injecting. The game plugin logs
  its counters every 2 s for the first minute, then every 15 s. When both stop at the same
  time, the whole console froze. When only the game heartbeat stops, the game process died.
- Hot paths are rate-limited. The game hooks log their first 5 calls, then every 1000th. USB
  reports log the first 8 as hex dumps, then the first 300 button changes, then every 100th.

### Finding what freezes the console

Two settings in `riffmaster.cfg` help when the console freezes and the log doesn't say why:

- `debug_stage=0..2` turns the loader's work on one step at a time. `0` only logs. `1` adds notifications and
  game detection without injecting. `2` is normal operation. Reboot, launch the game, and the first stage that
  freezes is where the problem is.
- `trace=1` logs before and after every call the loader's poll makes. If the last line in `riffmaster.old.log`
  is a `trace: ...` line without its matching `returned` line, that call hung.

The loader detects games through VSH's own `GetCooperationMode` and `GetGameProcessID` exports, not PS3MAPI.
An earlier version listed processes with PS3MAPI `GET_ALL_PROC_PID` every 2 s, and that call hung the whole
console when it ran while a game was starting.

A healthy run looks roughly like this:

```
=== new boot, riffmaster_loader starting ... ===
game process found: pid 0x..., waiting 8s before injecting
calling ps3mapi_load_proc_module(pid 0x..., /dev_hdd0/plugins/riffmaster_game.sprx)...
riff_start: module_start in pid 0x...
ps3mapi_load_proc_module returned 0 (0x0)
riffmaster_game thread started
profile: gh (0x12ba:0x0100)
reading ELF header at 0x00010000... (phdrs, lib stubs, every sys_io import)
hooking nid 0x3aaad464 ... / write_u32 ... / readback ...
imports patched: 4 of 4
cellUsbdRegisterExtraLdd2 returned 0x0
probe: dev N ... attach: dev N ... set_config_done ... interrupt pipe N
report #0 ... (hex dump)
registering virtual pad ... virtual pad port N
heartbeat: ...
```

## Needs verifying on hardware

None of this has been compiled or run yet. Check these first:

1. **Riffmaster PID.** `RM_PID_MIN/MAX` in `game/riffmaster_game.c` currently accept any PDP device. Set them to the guitar's exact PID.
2. **Whammy range.** The code assumes the game wants right stick X to rest at `0x7F` and reach `0xFF` at full whammy. If sustains don't bend, adjust `whammy_rest/whammy_full` in `PROFILE_GH`/`PROFILE_RB`.
3. **Tilt range.** The code assumes sensor X goes from `0x200` (level) to `0x180` (tilted). Tilt also triggers Select, so star power works even if this range is wrong.
4. **SDK names.** The code expects `cellUsbdSetConfiguration`, `cellUsbdAllocateMemory` and the `CELL_PAD_PCLASS_*` guitar constants to exist in your SDK version. If one is missing, the compiler will report it.
5. **Import hooking.** This relies on the game's ELF header being mapped at `0x10000`. If the log says `prx info not found`, the guitar will still send input but won't be identified as a guitar.
