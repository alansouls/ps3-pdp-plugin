# Riffmaster → PS3 guitar plugin

Makes a **PDP Riffmaster (PS4)** guitar work as a PS3 guitar in Guitar Hero III, World Tour, GH5,
Metallica, Smash Hits, Warriors of Rock and Rock Band 1–3. It needs Evilnat CFW with Cobra and webMAN MOD.
The guitar plugs into the PS3 directly over USB.

## How it works

| Piece | Runs in | Job |
| --- | --- | --- |
| `riffmaster_loader.sprx` | VSH (`boot_plugins.txt`) | Watches for a game (`EBOOT.BIN`) process and injects the game plugin through Cobra PS3MAPI after 8 s. |
| `riffmaster_game.sprx` | Game process | Opens the Riffmaster through `cellUsbd`. Converts each report using `../riffmaster_report_map.md`. Feeds a virtual pad through `cellPadLdd`. Redirects the game's `cellPadGetInfo/GetInfo2/PeriphGetInfo/PeriphGetData` imports so that pad reports as a guitar. |

### Button mapping

| Riffmaster | PS3 guitar |
| --- | --- |
| Green / Red / Yellow / Blue / Orange | Cross / Circle / Triangle / Square / L1 |
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

Both plugins append to `/dev_hdd0/tmp/riffmaster.log`. Fetch it with webMAN at
`http://<ps3-ip>/dev_hdd0/tmp/riffmaster.log`. A healthy run looks like this:

```
riffmaster_loader started
injected into pid 0x...
load result 0x00000000
riffmaster_game started
profile: gh (0x12BA:0x0100)
hooked nid 0x3aaad464 ... (one line per hooked function)
imports patched 0x00000004
cellUsbdRegisterExtraLdd2 0x00000000
probe ok, dev 0x...
attach, SET_CONFIGURATION 0x00000000
interrupt pipe 0x...
cellPadLddRegisterController 0x...
virtual pad port 0x...
```

## Needs verifying on hardware

None of this has been compiled or run yet. Check these first:

1. **Riffmaster PID.** `RM_PID_MIN/MAX` in `game/riffmaster_game.c` currently accept any PDP device. Set them to the guitar's exact PID.
2. **Whammy range.** The code assumes the game wants right stick X to rest at `0x7F` and reach `0xFF` at full whammy. If sustains don't bend, adjust `whammy_rest/whammy_full` in `PROFILE_GH`/`PROFILE_RB`.
3. **Tilt range.** The code assumes sensor X goes from `0x200` (level) to `0x180` (tilted). Tilt also triggers Select, so star power works even if this range is wrong.
4. **SDK names.** The code expects `cellUsbdSetConfiguration`, `cellUsbdAllocateMemory` and the `CELL_PAD_PCLASS_*` guitar constants to exist in your SDK version. If one is missing, the compiler will report it.
5. **Import hooking.** This relies on the game's ELF header being mapped at `0x10000`. If the log says `prx info not found`, the guitar will still send input but won't be identified as a guitar.
