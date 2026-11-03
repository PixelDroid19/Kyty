# Host runtime

This document defines the host-facing window, input, audio-pause and save-data
contracts. They are general runtime behavior, not per-title compatibility
rules.

## Window and input

The SDL window is resizable and uses borderless desktop fullscreen. The
following commands are edge-triggered; keyboard repeat cannot toggle state:

| Input | Host action |
| --- | --- |
| `F11` | Toggle fullscreen |
| `Alt+Enter` | Toggle fullscreen without forwarding Enter to the guest |
| Primary-button double click | Toggle fullscreen, unless the mouse is captured |
| `F9` | Pause or resume guest presentation and host audio |
| `F2` | Toggle text entry for a guest IME keyboard |
| `F3` | Capture or release the mouse for camera look |
| `Escape` | Request a clean runtime exit |

Fullscreen hides the cursor after two seconds of inactivity and restores it
on motion, focus loss or exit from fullscreen. Minimized windows suspend the
render loop. Resize, restore and focus events update runtime state instead of
being inferred from frame failures. Focus loss releases the mouse capture and
every key and mouse button held on the virtual pad.

### Keyboard virtual pad

Outside text entry the keyboard and the captured mouse drive a connected
virtual pad, and gamepads drive their own. The active
[input profile](input-profiles.md) maps both: keys by physical position, mouse
buttons, mouse look on the right stick, gamepad buttons, stick deadzones and the
gamepad to use. Its default profile is:

| Key | Pad input |
| --- | --- |
| `Z`, `Space`, `Enter` | Cross |
| `X` | Circle |
| `C` | Square |
| `V` | Triangle |
| Arrow keys | D-pad |
| `Q` / `E` | L1 / R1 |
| `1`, right mouse button / `3`, left mouse button | L2 / R2 |
| `F` / `H` | L3 / R3 |
| `Tab`, `Backspace` | Options |
| `T` | Touchpad |
| `W`, `A`, `S`, `D` | Left stick |
| `I`, `J`, `K`, `L`, mouse motion | Right stick |

A held L2 or R2 key reports the trigger button and full analog travel (255), so
titles that read either the button bit or the analog value see a pressed
trigger.

### Keyboard virtual pad

Outside text entry the keyboard drives a connected virtual pad:

| Key | Pad input |
| --- | --- |
| `Z`, `Space`, `Enter` | Cross |
| `X` | Circle |
| `C` | Square |
| `V` | Triangle |
| Arrow keys | D-pad |
| `Q` / `E` | L1 / R1 |
| `1` / `3` | L2 / R2 |
| `Tab`, `Backspace` | Options |

A held `1` or `3` reports the trigger button and full analog travel (255), so
titles that read either the button bit or the analog value see a pressed
trigger. `Escape` is also mapped to Circle, but the exit command above consumes
it first.

### Text entry

Text entry sends keys to the guest IME instead of the keyboard's virtual pad,
so letters bound to pad buttons type instead of pressing them. Held pad keys
are released whenever the mode changes. Text entry is on while `F2` has turned
it on, and always while a guest IME dialog runs:

- An IME keyboard the title opened receives each key's USB HID usage and the
  character it types, through the handler the title registered. Delivery runs
  on the title's thread that calls `sceImeUpdate`.
- An IME dialog edits the text the title placed in its buffer, bounded by the
  dialog's maximum length. Backspace deletes the last character and `Enter`
  confirms the text. The dialog then finishes with an OK result. No system
  keyboard is drawn, so the window title shows the text being typed. Only a
  host keyboard can confirm the dialog; pad input does not reach it.

The typed character comes from the host key code: letters follow Shift and
Caps Lock, and Latin-1 letters are typed too. Keys whose shifted character
depends on the host layout type nothing while Shift is held. Combinations with
Ctrl, Alt or GUI type nothing, so AltGr characters are not typed, and no key
composes with the next one.
`Escape` keeps requesting a runtime exit in text entry.

Controller add, remove and remap events are routed through the same event
pipeline as axis/button input. Removal closes only the matching SDL controller
instance; remap does not fabricate a disconnect.

## Host and console settings

The launcher writes these keys to the run script; each has one consumer in the
runtime, and a value out of range ends the start with an error.

| Key | Values (default) | Effect |
| --- | --- | --- |
| `GpuDevice` | Vulkan device name (empty) | Runs on the device with exactly that name; a name that is absent or lacks what the renderer needs ends the start with an error. Empty prefers a discrete device. Two identical cards share a name, so the choice between them is not controllable. |
| `PresentMode` | `Mailbox`, `Immediate`, `Fifo` (`Mailbox`) | `Fifo` always holds. `Mailbox` falls back to `Immediate`, and `Immediate` to `Fifo`, when the surface lacks the mode. |
| `FullscreenEnabled` | boolean (`false`) | Enters full screen when the window opens. |
| `HideCursorEnabled` | boolean (`false`) | Hides the cursor in windowed mode too. |
| `PerfOverlayEnabled` | boolean (`false`) | Shows the performance overlay from the first frame. |
| `ShaderCacheEnabled` | boolean (`true`) | Off neither reads nor writes the SPIR-V, pipeline and compute pipeline recipe caches on disk. |
| `ConsoleLanguage` | `0` to `29` (`1`) | The value of the system language parameter guests read. |
| `UserName` | 1 to 16 UTF-8 bytes (`Kyty`) | The name of the user service and of the online id. It is not part of save paths. |
| `NoticeScreenSkipEnabled` | boolean (`false`) | The answer guests get when they ask whether a start notice may be skipped. |
| `NetworkConnected` | boolean (`true`) | Off reports the network state as disconnected and fails network info requests with the not-connected code. |
| `PsnSignedIn` | boolean (`false`) | On, user `1` is signed in to a local PSN account: `NpGetState` reports signed in, and `GetAccountIdA` returns one fixed account id, which `GetUserIdByAccountId` maps back to user `1`. Reachability, authorization and every online request still fail. It is a per-game comfort setting for titles that post trophy events only while signed in (see [Trophies](#trophies)); runs with it on are not strict runs for compatibility claims. |
| `ControllerVibration` | `0` to `100` (`100`) | Scales guest vibration requests before they reach the host pad. |
| `ControllerLightBar` | `#RRGGBB` (empty) | Replaces the light bar colour the guest sets, from the moment the pad connects. Empty forwards the guest's colour. |
| `ControllerSpeakerVolume` | `0` to `100` (`0`) | Plays the guest's pad speaker port on the default host output at this gain. `0` keeps that port silent. |
| `MicrophoneEnabled` | boolean (`false`) | Audio input ports record the default host recording device in the port's format (16-bit mono or stereo). Off, or when the recording falls behind, the guest reads silence. At most four periods are kept for a guest that reads late. With the setting on and no recording device that opens, the port does not open and the guest gets the port-full error. |
| `PresentationScaling` | `Stretch`, `Fit`, `Integer` (`Stretch`) | How the final image fills the window; see [render-resolution.md](render-resolution.md#presentation). |

### Settings not offered

Each of these was checked against what it would change for the guest:

- **User id.** The guest sees user `1`. The number only names the
  save-memory folder (`user-1`), so another value would hide existing saves
  and change nothing else.
- **Guest TSC frequency.** `KernelGetTscFrequency` reports the frequency of the
  host counter that `KernelReadTsc` reads. Another value would no longer match
  the counter.
- **HDR output.** `VideoOutGetOutputStatus` reports SDR. Reporting HDR needs a
  presentation path that writes HDR10 to an HDR swapchain first.
- **Guest refresh rate.** Vblank events follow the host present loop
  (`game_show_window`), not a fixed clock. A rate setting needs that clock
  first.
- **Turning off occlusion queries, tessellation or image readback.** There is
  no such work to turn off. Tessellation is not implemented (no tessellation
  state in `GraphicsRenderPipeline.cpp`), occlusion queries already report a
  synthetic always-visible result (`CommandProcessor::TriggerEvent`), and on
  PS5 render targets are never written back to guest memory
  (`write_back = false` in `GraphicsRenderAttachments.cpp`). Only storage
  buffers are written back, and they carry the labels the guest waits on.
  A memory wait already proceeds at once when the value in memory meets its
  condition (`CommandProcessor::WaitRegMem32`).
- **Keeping textures as first uploaded.** Measured on 2026-11-03 with a
  per-game switch that skipped the re-hash and upload of textures (Linux,
  60 fps limit, 80 s runs). `PPSA03706 - My Friend Peppa Pig` re-hashed
  textures 5169 times, 5147 of them with new content, for 265 ms in the first
  minute and none in the next 20 s. `PPSA21402 - Balatro` re-hashed 19 times
  for 3 ms, and when they were skipped the textures it fills after creating
  them stayed blank. The work is real content, paid while loading, so skipping it
  only shows wrong images. The switch was not kept.

A game's "Environment variables" field in the launcher holds `NAME=value`
pairs separated by `;`, so a value cannot contain `;`. The launcher adds them
to its own environment when it starts the emulator for that game, and refuses
to start when a pair is malformed.

## Audio pause and queueing

`F9` closes the audio producer gate before pausing SDL devices. Resume starts
devices before releasing producers, preventing the first resumed grain from
being queued behind a paused device. Queue capacity is time-based (60 ms), and
one producer waits at most 250 ms for a saturated host queue.

See [AUDIO.md](AUDIO.md) for PCM formats, volume and device conversion.

## SaveData paths

The default root is resolved relative to the executable:

```text
<executable>/user/savedata/<NORMALIZED_TITLE_ID>/
```

`KYTY_SAVEDATA_DIR` replaces `<executable>/user/savedata` and must be an
absolute path. The title identifier is normalized to uppercase ASCII letters,
digits, `-` and `_`; other characters become `_`.

Mounted slots retain their exact validated guest name. SaveDataMemory uses:

```text
<root>/.kyty-memory/<NORMALIZED_TITLE_ID>/memory/user-<USER_ID>/slot-<SLOT_ID>.bin
```

There is no legacy `_SaveData` search, relative-path fallback or automatic
migration. Move existing data explicitly if the configured root changes.

SaveDataMemory has a 64 MiB limit per slot. `Setup` loads the backing file and
reports its previous size; `Get` and `Set` require that setup and enforce exact
ranges. `Sync` writes a process-unique temporary file, atomically replaces the
slot, then publishes the guest completion event.

## Trophies

A title unlocks a trophy by posting Universal Data System (UDS) events. The
emulator keeps the scalar properties a title sets on an event (integers of
every width, bools as 0 or 1, strings) and, on `PostEvent`, applies the title's
own rules:

- `sce_sys/uds/uds00.ucp` holds the rules (`stats_extraction.json`) that turn
  an event into a stat value and the stats (`stats_definition.json`).
- `sce_sys/trophy2/trophy00.ucp` holds the trophies (`tropconf.json`); a
  trophy unlocks when its stat reaches its target.
- Both containers are plain: a big-endian header with the magic `b228c60a`,
  the entry count at 0x10 and the entry size 0x40 at 0x14, then from 0x60
  entries of a 32-byte name and a 64-bit offset and size. Only the JSON
  entries are read. A container over 256 MiB, or with more than 16 MiB of JSON
  in total, is rejected. Of the 28 sets checked on 2026-11-03, the largest
  container is 29 MB and none holds more than 450 KB of JSON.

Only rules that feed a stat a trophy waits for are kept. They match one
top-level property with `==` or `>` and update a `count`, `max` or `latest`
stat; the platinum unlocks once every trophy of its set is unlocked. A rule in
another form is skipped and counted in one warning when the set loads, and its
trophy cannot unlock. Every rule of the 28 sets checked on 2026-11-03 uses the
kept forms.

Some titles post these events only while user `1` is signed in.
`PPSA21402 - Balatro` retries its PSN start every second until `NpGetState`
reports signed in and `GetAccountIdA` succeeds; only then does it post
`_UnlockTrophy`, and it checks its career trophies when the main menu loads.
Its trophies therefore unlock only with `PsnSignedIn` on.

Stats and unlock times (Unix seconds) are kept for user `1`, written
atomically after each change; a write that fails is tried again on the next
event:

```text
<root>/.kyty-trophies/<NORMALIZED_TITLE_ID>/user-1.json
{"stats":{"<stat id>":<value>},"unlocked":{"<trophy id>":<time>}}
```

A progress file that cannot be read turns trophies off for that run, so it is
never overwritten. The launcher's game page lists the set in the interface
language when the set has it, with each unlock time and the progress of
progressive trophies; a locked hidden trophy shows neither its name nor its
description. The page reads the progress file again every time it opens.

The game itself still sees nothing unlocked: the `NpTrophy2` query layouts and
the unlock callback are not sourced, so those calls keep returning empty
results. Trophies with a server unlock type are evaluated locally from the same
stats.

## Focused verification

```bash
_build_linux/fc_script scripts/run_unit_tests.lua \
  --gtest_filter='EmulatorAudio.*:EmulatorSaveData.*'
```

These tests cover command edge suppression, PCM volume/queue calculations,
SaveData path validation, slot isolation, persistence and the HLE NID path.
