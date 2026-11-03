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
| `ControllerVibration` | `0` to `100` (`100`) | Scales guest vibration requests before they reach the host pad. |
| `ControllerLightBar` | `#RRGGBB` (empty) | Replaces the light bar colour the guest sets, from the moment the pad connects. Empty forwards the guest's colour. |
| `ControllerSpeakerVolume` | `0` to `100` (`0`) | Plays the guest's pad speaker port on the default host output at this gain. `0` keeps that port silent. |
| `MicrophoneEnabled` | boolean (`false`) | Audio input ports record the default host recording device in the port's format (16-bit mono or stereo). Off, or when the recording falls behind, the guest reads silence. At most four periods are kept for a guest that reads late. With the setting on and no recording device that opens, the port does not open and the guest gets the port-full error. |
| `PresentationScaling` | `Stretch`, `Fit`, `Integer` (`Stretch`) | How the final image fills the window; see [render-resolution.md](render-resolution.md#presentation). |

### Settings not offered

Each of these was checked against what it would change for the guest:

- **Signed in to PSN.** `NpGetState` reports signed out. Signing in would make
  titles reach for online services that do not exist offline.
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
- **Turning off occlusion queries, tessellation or image readback.** Each one
  skips work the guest asked for.
- **Trophies.** The `NpTrophy` and `NpTrophy2` calls succeed and report nothing
  unlocked. On PS5 a trophy unlocks when a UDS stat meets the condition in
  `sce_sys/trophy2/trophy00.ucp` (`tropconf.json`). Those stats come from the
  events the title posts, through the rules in `sce_sys/uds/uds00.ucp`
  (`events_definition.json`, `stats_extraction.json`). Both containers are
  plain: a 0x60-byte header with the entry count at 0x10, then 0x40-byte
  entries holding a 32-byte name and a big-endian 64-bit offset and size. Real
  trophies need the posted event values (today `PostEvent` drops them), that
  rule engine, and stored stats.

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
<title-root>/memory/user-<USER_ID>/slot-<SLOT_ID>.bin
```

There is no legacy `_SaveData` search, relative-path fallback or automatic
migration. Move existing data explicitly if the configured root changes.

SaveDataMemory has a 64 MiB limit per slot. `Setup` loads the backing file and
reports its previous size; `Get` and `Set` require that setup and enforce exact
ranges. `Sync` writes a process-unique temporary file, atomically replaces the
slot, then publishes the guest completion event.

## Focused verification

```bash
_build_linux/fc_script scripts/run_unit_tests.lua \
  --gtest_filter='EmulatorAudio.*:EmulatorSaveData.*'
```

These tests cover command edge suppression, PCM volume/queue calculations,
SaveData path validation, slot isolation, persistence and the HLE NID path.
