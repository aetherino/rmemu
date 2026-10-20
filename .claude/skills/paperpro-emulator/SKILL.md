---
name: paperpro-emulator
description: Drive the reMarkable Paper Pro emulator in this repo — boot it, look at the screen, tap/draw/swipe, press hardware buttons, dock the Marker, SSH in, deploy and test software, and debug the emulator itself. Use whenever a task involves running, testing, screenshotting or interacting with the tablet, xochitl, or software meant for the Paper Pro.
---

# Paper Pro emulator harness

The emulator runs the **stock reMarkable Paper Pro firmware** (xochitl, Qt 6.8, firmware 3.27.1.0 by default) natively in an arm64 Docker container, with a preload shim faking the hardware. Everything is driven through `bin/rmemu` from the repo root. The user may be watching the same device live in the simulator UI at http://127.0.0.1:7777, so act deliberately.

## 1. Get to a known state

```bash
bin/rmemu status           # container / UI server / images
bin/rmemu start            # boots container + UI server; returns once the UI is up (first ever run downloads ~200 MB firmware and builds the image)
bin/rmemu skip-setup       # fresh device -> skips onboarding, lands on "My files"
bin/rmemu reset            # factory reset: wipes the rmemu-home + rmemu-data volumes (ask the user first — it deletes their notebooks)
```

State persists across `stop`/`start` (documents in `/home/root/.local/share/remarkable/xochitl/`, settings in `/home/root/.config/remarkable/xochitl.conf` and `/data/xochitl.conf`).

## 2. Look → act → look

```bash
bin/rmemu screenshot /tmp/s.png --scale 0.5   # prints "810x1080; multiply by 2 for device coordinates"
```
Then Read the PNG. `--scale 0.5` is the right default (legible, cheap); use `0.25` for "what screen am I on", `1` only for fine detail.

**All input coordinates are device pixels**: portrait 1620×2160, origin top-left. Multiply what you measured on a scaled screenshot by the printed factor. Never pass screenshot coordinates directly.

```bash
bin/rmemu tap X Y                    # finger tap — use for buttons, menus, navigation
bin/rmemu tap X Y --pen              # pen tap (also activates UI)
bin/rmemu longpress X Y [--ms 900]   # context actions on files ("press and hold")
bin/rmemu swipe X0 Y0 X1 Y1 [--ms 300] [--pen]
bin/rmemu draw 300,700 500,600 700,800 [--pressure 0.6] [--eraser]   # pen stroke through points (write/sketch)
bin/rmemu pinch CX CY R0 R1          # two-finger pinch (zoom in: R0 < R1)
bin/rmemu wait --idle 800            # until screen + e-ink panel stop changing
```

Every input command **blocks until the screen and panel settle**, then prints `ok (screen changed)` or `ok (no screen change)`.
- "no screen change" usually means you missed the target — re-screenshot and re-measure rather than repeating blindly.
- Some screens update seconds later (after a full e-ink refresh, e.g. onboarding's "Refresh again", page loads). If a result looks stale, `bin/rmemu wait --idle 800` and screenshot again before acting.
- Chain several taps in one shell call only when you're sure of the screens in between; otherwise one action per screenshot.

## 3. Hardware

```bash
bin/rmemu power                 # power button: sleep ("Paper tablet is sleeping") / wake
bin/rmemu folio close|open      # Folio cover hall sensor
bin/rmemu marker dock|undock    # attach Marker: real marker-manager reports Docked + charging
bin/rmemu usb connect|disconnect
bin/rmemu battery 15            # battery percent
```

Avoid the in-OS **Settings → Restart / Turn off** buttons; restart the app with `bin/rmemu restart` instead.

## 4. Developing / deploying software

The emulator behaves like a tablet in developer mode:
```bash
bin/rmemu ssh 'cmd'             # like `ssh root@10.11.99.1`; interactive if no cmd
bin/rmemu push ./thing /home/root/thing
bin/rmemu pull /home/root/.local/share/remarkable/xochitl ./out
bin/rmemu restart               # = systemctl restart xochitl (works inside ssh too)
bin/rmemu logs [-f]             # xochitl + shim + rmemud output
```
- Direct SSH for scripts written against a real device: `ssh -i .rmemu/id_ed25519 -p 2222 root@127.0.0.1` (swap for `root@10.11.99.1` on hardware).
- Root fs is the real rootfs image; changes outside `/home` and `/data` are lost when the container is recreated (same as OTA on device).
- Apps reading `/dev/input/event*` get live input: event0 power key, event1 Elan pen (ABS_X 0..16200, ABS_Y 0..21600, pressure 0..4095), event2 touchscreen (MT, 0..1619 × 0..2159), event3 hall sensors (SW_LID, SW_PEN_INSERTED).
- `systemctl` only knows `xochitl`, `marker-manager`, `rmemud` (start/stop/restart/status).
- Testing on other firmware: `bin/rmemu versions`, `bin/rmemu start <version>`.

## 5. UI map (firmware 3.27.1.0 — verify with a screenshot)

| Where | Target | Device coords |
|---|---|---|
| Home (My files) | side menu ☰ | 56, 60 |
| Home | `+` new (bottom pill) → menu: Notebook / Folder / Quick sheet | 810, 2004 → Notebook ≈ 750, 1600 |
| Home | search · calendar (bottom pill) | 698, 2004 · 922, 2004 |
| Side menu | Settings · Trash | 150, 2098 · 130, 1856 |
| Settings | Back · General · Wi-Fi · Security · Display · Help | 128, 56 · 150, 168 · 130, 280 · 150, 504 · 145, 616 · 125, 840 |
| New notebook dialog | Create (top right) | 1540, 56 |
| Open notebook | close document (⊗ top right) · toolbar column at x≈56 | 1564, 56 |
| Onboarding | most "Next" buttons | 810, 1916 |

The open-notebook toolbar sits on the left edge at x ≈ 56 (y ≈ 168 pen, 280 secondary pen, 392 text, 504 eraser, 616 select, 728 layers, 840 undo, 952 redo); tapping the active pen opens its style/color panel. Keep drawing coordinates at x > 250 to avoid it. First visits show tooltips — close them before drawing.

## 6. When something's off

- `bin/rmemu status` says not connected → `bin/rmemu logs | tail -50`; a crashed xochitl restarts on its own within ~1s.
- Screen frozen / black → `bin/rmemu restart`.
- UI server down (CLI says "emulator is not running" but container runs) → `bin/rmemu start` restarts just the server.
- Emulating new hardware or fixing the shim: `bin/rmemu restart --debug` then `bin/rmemu logs` shows every hardware path/ioctl xochitl touches (`FAIL` lines are what's missing). Read `CLAUDE.md` and `README.md` ("How it works") before changing `shim/`, `daemon/` or `docker/`; rebuild with
  `docker build -q -t rmemu:3.27.1.0 -f docker/Dockerfile . && bin/rmemu stop && docker rm -f rmemu && bin/rmemu start`.

## Not emulated

E-ink refresh visuals (screen shows the final image instantly; no flash/ghosting), Wi-Fi/cloud sync/pairing, auto-rotation, Type Folio keyboard, Bluetooth. Don't report UI bugs in those areas as real-device behavior.
