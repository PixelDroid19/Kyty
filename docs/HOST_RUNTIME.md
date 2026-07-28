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
| Primary-button double click | Toggle fullscreen |
| `F9` | Pause or resume guest presentation and host audio |
| `Escape` | Request a clean runtime exit |

Fullscreen hides the cursor after two seconds of inactivity and restores it
on motion, focus loss or exit from fullscreen. Minimized windows suspend the
render loop. Resize, restore and focus events update runtime state instead of
being inferred from frame failures.

Controller add, remove and remap events are routed through the same event
pipeline as axis/button input. Removal closes only the matching SDL controller
instance; remap does not fabricate a disconnect.

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
