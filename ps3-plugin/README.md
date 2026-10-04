# Riffmaster PS3 plugins: technical reference

This is how the two plugins work, and how to debug them. For what the project does, which games it works with,
how to install it and how to build it, see the [repository README](../README.md).

## The two plugins

| Plugin | Runs in | Job |
| --- | --- | --- |
| `riffmaster_loader.sprx` (`vsh/`) | VSH, from `boot_plugins.txt` | Detects games, injects the game plugin, shows XMB notifications, and writes both plugins' logs. |
| `riffmaster_game.sprx` (`game/`) | The game process | Hooks the game's `cellPad` imports, drives the Riffmaster over `cellUsbd`, and hands the game guitar input. |

`common/` holds code both use: Cobra PS3MAPI calls (`ps3mapi.h`), libc-free helpers and logging (`util.h`), and
the status and log-buffer formats the two plugins share (`status.h`).

## Loader

1. **Game detection.** Every 2 s, the loader calls VSH's own exports `GetCooperationMode` (`vshmain_EB757101`,
   0 on the XMB) and `GetGameProcessID` (`vshmain_0624D3AE`), like webMAN MOD. It finds them, and `vshtask_notify`
   and `paf`'s `View_Find`, in the VSH export table at `*(0x1008C) + 0x984`, the same lookup as webMAN MOD's
   `getNIDfunc`.

   Don't use PS3MAPI's process list for this: an earlier version called `GET_ALL_PROC_PID` every 2 s, and that
   call hung the whole console when it ran while a game was starting.
2. **Injection.** 8 s after a game appears, the loader calls PS3MAPI `LOAD_PROC_MODULE` with
   `/dev_hdd0/plugins/riffmaster_game.sprx` and a `riff_arg_t`: its own process ID and the address of a
   `riff_status_t` in its memory.
3. **Watching.** Every poll, it logs any change to that status, copies new game-plugin log lines into the log file
   and shows notifications. If setup hasn't finished 20 s after injection, it says where it stopped. It also lists
   the game's modules, reads the game plugin's status straight from game memory, and checks that the game plugin's
   imports are linked.

## Game plugin

### Starting

Cobra runs `module_start` on a thread created by its kernel code. On that thread, calls into liblv2 never return
(seen on hardware with `sys_ppu_thread_get_id` and `sys_ppu_thread_create`), so `riff_start` only makes direct
syscalls:

1. It takes the loader's argument. Cobra passes it as the **first** parameter (`args`); `argp` holds `0xc8`. It is
   read through PS3MAPI `GET_PROC_MEM`, so a bad pointer returns an error instead of crashing.
2. It reports step 1 to the loader.
3. It hooks the game's `sys_io` imports (`patch_imports`). It finds the game executable's import stubs through the
   `sys_process_prx_info` segment of the ELF mapped at `0x10000`, and writes each hook's address into the import
   slot with PS3MAPI `SET_PROC_MEM`.

The first time the game calls a hooked function, the call runs on one of the game's own threads. `bootstrap()`
creates the log lock and starts `riff_thread` there, once. Logging is off until then.

### Hooks

| Game import | Hook does |
| --- | --- |
| `cellPadGetData` | Starts the plugin on the first call. On the guitar's port, hands the game the current guitar state whenever it changed since the game's last read; otherwise reports `len 0`, as the pad library does. |
| `cellPadGetInfo` | Reports the guitar's port as `12BA:0100` (GH) or `12BA:0200` (RB). Older games such as GH3 still import this, so it uses the legacy `CellPadInfo` layout. |
| `cellPadGetInfo2` | Reports the guitar's port as a standard pad with press and sensor modes. |
| `cellPadPeriphGetInfo`, `cellPadPeriphGetData` | Report the guitar's port as a guitar peripheral with its fret, strum, whammy and tilt values. None of the tested games import these. |

A game that doesn't import a function simply doesn't get that hook. The tested games import `cellPadGetData`,
`cellPadGetInfo` and `cellPadGetInfo2` (3 of 5).

### USB and the virtual pad

`riff_thread` loads the USBD module, calls `cellUsbdInit` and registers an extra USB driver for vendor `0E6F` with
`cellUsbdRegisterExtraLdd2`. When the Riffmaster (`0E6F:024A`) attaches, it opens the first HID interrupt-IN
endpoint and keeps one 64-byte read pending. Each report is decoded as described in
[`../riffmaster_report_map.md`](../riffmaster_report_map.md).

On the first report it registers a virtual pad with `cellPadLddRegisterController`, which gives the guitar its own
port. It also passes each change to `cellPadLddDataInsert`. In GH Metallica that data never reached the game's
`cellPadGetData` (every read of the port returned `len 0`), so the `cellPadGetData` hook hands the game the frames
itself.

### Button mapping

| Riffmaster | Sent as | Source in the report |
| --- | --- | --- |
| Green / Red / Yellow / Blue / Orange | Cross / Circle / Square / Triangle / L1 | byte 46 bits `0x01`–`0x10` |
| Strum up / down | D-pad up / down | byte 5 low nibble (hat) `0` / `4` |
| Start / Select | Start / Select | byte 6 `0x20` / `0x10` |
| Whammy | Right stick X, `0x7F` at rest to `0xFF` | byte 44 |
| Tilt | Sensor X, `0x200` level to `0x180` tilted; also presses Select above `0x80` (released below `0x50`) | byte 45 |
| PS button | Not forwarded | byte 7 `0x01` |

A real PS3 Guitar Hero guitar reports Yellow as Square and Blue as Triangle; this was confirmed in GH Metallica.
The whammy and tilt ranges are set per profile in `PROFILE_GH` and `PROFILE_RB`.

## Status, notifications and logs

### Progress reports

`riff_status_t` (`common/status.h`) carries the last setup step and its result, plus debugging fields. After each
step, the game plugin writes its copy into the loader's memory with PS3MAPI `SET_PROC_MEM`. That needs no files and
no liblv2, so it works from `module_start` too. The game plugin's own copy starts with the signature `RIFF` `STAT`,
so the loader can also find and read it in game memory if reports stop arriving.

Steps: 1 `module_start` → 2 thread started → 3 profile selected → 4 imports hooked → 5 USBD module loaded →
6 `cellUsbdInit` → 7 setup done → 8 guitar attached → 9 first input report.

### XMB notifications

Only VSH can show notifications, so the loader shows them, including the ones about the game plugin's progress.
`notifications=` in `riffmaster.cfg` chooses which ones appear: `2` = all (default), `1` = only failures, `0` =
none. Every message is logged either way, with `(not shown, ...)` when it was suppressed.

| When | Message | Shown at |
| --- | --- | --- |
| Loader started (once the XMB is up, or after 30 s) | `Riffmaster loader loaded` | 2 |
| Game detected | `Riffmaster: game found, loading guitar plugin in 8s` | 2 |
| Game plugin finished setup | `Riffmaster plugin loaded (Guitar Hero guitar)` / `(Rock Band guitar)` | 2 |
| Guitar attached | `Riffmaster guitar connected` | 2 |
| VSH exports for game detection not found | `Riffmaster loader: can't detect games on this firmware` | 1, 2 |
| `LOAD_PROC_MODULE` failed | `Riffmaster: failed to load guitar plugin (0x...)` | 1, 2 |
| USB setup failed | `Riffmaster plugin loaded, but USB setup failed (0x...)` | 1, 2 |
| Guitar attach failed | `Riffmaster guitar attach failed (0x...)` | 1, 2 |
| Setup not finished 20 s after injection | `Riffmaster: guitar plugin didn't report back` / `Riffmaster: plugin stopped after '<step>' (0x...)` | 1, 2 |

### Game plugin log relay

The game process isn't allowed to open files in `/dev_hdd0/tmp` (`EACCES`, `0x80010029`). The game plugin
overrides `rm_logf`'s output (`RIFF_LOG_SINK`): each line goes into a 64 KB ring buffer (`riff_logbuf_t`) in its
memory, and the buffer's address goes out with the status. On every poll, the loader reads new bytes with PS3MAPI
`GET_PROC_MEM` and appends them to the log file.

## Debugging

Both plugins log to `/dev_hdd0/tmp/riffmaster.log` (with webMAN: `http://<ps3-ip>/dev_hdd0/tmp/riffmaster.log`).

- **Line format.** `[uptime s.us] loader|game t<thread id>: message`. Game lines reach the file 1–2 s after they
  were written, so they can come after loader lines from the same moment.
- **One log per boot.** When the loader starts within 90 s of power-on, it moves the previous log to
  `riffmaster.old.log`. After a freeze and a hard power-off, the run that froze is in `riffmaster.old.log`.
- **Power loss.** The loader `fsync`s every line it writes, including the game lines it copies. Game lines still
  in the game plugin's buffer (the last 1–2 s) are lost if the console dies.
- **Size limit.** At 1 MB the log is emptied and restarted with a `log reached size limit` marker.
- **Heartbeats.** The loader logs once a second for 90 s after injecting, then every 30 s. The game plugin logs its
  counters every 2 s for a minute, then every 15 s: USB reads and errors, reports, state changes, the calls to each
  hook, and `game reads of the guitar port N, new data from the pad library N, frames handed to the game N`. If
  both stop at the same time, the whole console froze. If only the game's stops, the game process died.
- **Rate limits.** Hooks log their first 5 calls, then every 1000th. The first 60 frames handed to the game are
  logged (`guitar port N -> game #N: ...`), then every 100th. USB reports: the first 8 as hex dumps, then the first
  300 button changes, then every 100th.

### Finding what freezes the console

- `debug_stage=0..2` in `riffmaster.cfg` turns the loader's work on one step at a time:
  - `0`: only logs; no VSH or PS3MAPI calls, no notifications
  - `1`: adds notifications and game detection, without injecting
  - `2`: normal operation

  The first stage that freezes is where the problem is.
- `trace=1` logs before and after every call the loader's poll makes. If the last line in `riffmaster.old.log` is
  a `trace: ...` line without its `returned` line, that call hung.

### A healthy run

Abbreviated, from GH Metallica:

```
loader: game process found: pid 0x01030200, waiting 8s before injecting
loader: calling ps3mapi_load_proc_module(pid 0x01030200, /dev_hdd0/plugins/riffmaster_game.sprx), arg: ...
loader: ps3mapi_load_proc_module returned 0 (0x0)
loader: game plugin: step 1 (module_start), result 0x0, profile g, mark line ... result 0x3, log buffer 0x00000000
game:   riffmaster_game thread started, loader arg ok, ...
game:   profile: gh (0x12ba:0x0100)
game:   the game imports 12 sys_io functions: ...
game:   hook cellPadGetData (nid 0x8b72cda1): installed, ...
game:   imports patched: 3 of 5
game:   cellUsbdRegisterExtraLdd2 returned 0x0
loader: game plugin: step 7 (setup done), ...
loader: notify: 'Riffmaster plugin loaded (Guitar Hero guitar)'
game:   probe: dev 8 accepted / attach: dev 8 / interrupt pipe 1
loader: notify: 'Riffmaster guitar connected'
game:   registering virtual pad ... / GetInfo as the game sees it: port 1 vid 0x12ba pid 0x0100 ...  <- guitar
game:   guitar port 1 -> game #1: len 24, digital ...
```

## Open items

- `RM_PID_MIN/MAX` in `game/riffmaster_game.c` accept any PDP device. The Riffmaster is `0E6F:024A`.
- `riff_stop` still logs. If Cobra runs it on the same kind of kernel thread as `module_start`, unloading the game
  plugin while the game runs would hang.
- `cellPadLddDataInsert` data doesn't reach the game (see above). The virtual pad is still needed for the port it
  provides.
