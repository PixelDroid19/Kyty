# Input profiles

An input profile maps the host keyboard, mouse and gamepads to the guest pad.
One JSON file holds one profile. The emulator reads the file named by the
`InputProfileFile` config key; `scripts/run_guest.lua` sets it from
`KYTY_INPUT_PROFILE`. An empty or missing key selects the built-in profile,
which is identical to the launcher's `Default` profile below.

The launcher owns the profile list. It stores each profile as
`input-profiles/<id>.json` next to its settings file, names the file with an
id limited to `[A-Za-z0-9._-]`, and writes the absolute path of the active
profile into `kyty_run.lua`. `Default` always exists and cannot be deleted.

An invalid file stops the emulator at startup with an error that names the
field. Nothing falls back to defaults silently.

## Editing profiles in the launcher

Settings › Controls edits the profiles. One profile is active for every game;
there is no per-title profile. Changes are saved as they are made, and a game
uses the active profile from its next start.

- **Profile** chooses the active profile. `Default` is read-only; **Duplicate
  profile** copies the shown profile under a new name, **Delete profile** asks
  for a second selection before it removes the file, and **Restore defaults**
  rewrites the shown profile with the `Default` bindings. A file that does not
  follow the format is listed as unusable with the field at fault until it is
  restored or removed.
- **Controller** lists every connected gamepad and updates on hot-plug.
  Choosing one stores its GUID in `gamepad.device`; **Any controller** stores
  an empty string.
- Under the profile, four tabs share one drawing of the guest controller. Each
  control sits beside it with a line to its part; the focused control's part
  is outlined, and the parts the connected pad holds down light up. L1 and R1
  (Page Up and Page Down) change the tab.
- **Controller** and **Keyboard and mouse** hold one box per control: the 16
  buttons around the drawing and, on the keyboard tab, the stick directions as
  two crosses under it. Selecting a box waits 5 s for a key, mouse button or
  controller button; Escape cancels, the reserved keys are refused, and
  Delete or □ clears the box. A box holds one name; lists of several names
  are kept until the box is changed.
- **Sticks and triggers** has a card per stick with its dead zone, X and Y
  invert, and a live preview of the connected pad that shows the raw position
  and what the game sees after the dead zone; the trigger press point sits
  under the drawing. While a control has focus, the left stick does not move
  between controls so it can be tried; the D-pad still does.
- **Mouse** turns mouse look on and sets its sensitivity and Y invert.

## Format

```json
{
  "schema": "kyty_input_profile_v1",
  "name": "Default",
  "keyboard": { "<control>": ["<key>", ...] },
  "mouse": { "look": true, "sensitivity": 1.0, "invert_y": false },
  "gamepad": {
    "device": "",
    "buttons": { "<control>": ["<button>", ...] },
    "left_stick":  { "deadzone": 0.10, "invert_x": false, "invert_y": false },
    "right_stick": { "deadzone": 0.10, "invert_x": false, "invert_y": false },
    "trigger_deadzone": 0.0
  }
}
```

Every field is required, unknown fields are errors, and each field appears
once. `name` is the profile's display name and must not be empty.

### Controls

`keyboard` lists every control below; `gamepad.buttons` lists the first
sixteen (the stick directions bind only on the keyboard). An empty list leaves
a control unbound. One input may press several controls.

`cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right`, `l1`,
`r1`, `l2`, `r2`, `l3`, `r3`, `options`, `touchpad`, `left-stick-left`,
`left-stick-right`, `left-stick-up`, `left-stick-down`, `right-stick-left`,
`right-stick-right`, `right-stick-up`, `right-stick-down`.

The names match the actions of [pad routes](input-replay.md).

### Keys

Keys are SDL 3 scancode names (`SDL_GetScancodeName`), such as `Z`, `Space`,
`Return`, `Tab`, `Backspace`, `Up`, `Left Shift` or `1`. A scancode is a
physical key position, so a profile keeps its layout under AZERTY, QWERTZ and
other keyboard layouts. `Mouse Left`, `Mouse Middle`, `Mouse Right`,
`Mouse X1` and `Mouse X2` bind mouse buttons, which act only while the mouse
is captured.

`Escape`, `F1`, `F2`, `F3`, `F9` and `F11` run host commands
([host runtime](HOST_RUNTIME.md#window-and-input)) and cannot be bound.

### Mouse

`F3` captures the mouse for the guest and releases it. While captured, the
cursor is hidden and stays in the window, mouse buttons press their bound
controls, and window clicks do nothing. Focus loss releases the capture.

With `look` on, mouse motion drives the right stick. A speed of 1000 pixels per
second at `sensitivity` 1 deflects it fully; `sensitivity` ranges from 0.1 to
10. Any motion deflects the stick past the deadzone the pad reports to the
guest (about 31 of 127), so slow turns still move the camera. `invert_y`
inverts the vertical axis. A held right-stick key overrides mouse look on its
axis.

### Gamepads

Button names are SDL 3 gamepad button names (`SDL_GetGamepadStringForButton`):
`a`, `b`, `x`, `y`, `back`, `guide`, `start`, `leftstick`, `rightstick`,
`leftshoulder`, `rightshoulder`, `dpup`, `dpdown`, `dpleft`, `dpright`,
`misc1`, `touchpad`, `paddle1` to `paddle4`. The names follow SDL's
positional layout: `a` is the bottom face button on every controller.

Analog sticks and triggers keep their roles in v1: the left stick drives the
left stick, the right stick the right stick, and the triggers L2 and R2.

- `device` is empty to use every connected gamepad, or a GUID from
  `SDL_GUIDToString(SDL_GetGamepadGUIDForID(id))` to use only that model.
  Bytes 2-3 of the GUID, a CRC of the name the driver reports, are ignored when
  comparing. Other gamepads are not opened; when the selected one is missing,
  the keyboard and mouse still work.
- `deadzone` (0 to 0.9) is radial. Inside it the stick rests; outside it the
  remaining travel is rescaled, so full tilt still reaches the edge.
  `invert_x` and `invert_y` invert an axis.
- `trigger_deadzone` (0 to 0.9) ignores trigger travel below it and rescales
  the rest.

The keyboard and a gamepad work at the same time. Their buttons are combined,
and each stick or trigger axis follows the source moved farthest from rest.

## Default profile

| Control | Keyboard | Gamepad |
| --- | --- | --- |
| cross | `Z`, `Space`, `Return` | `a` |
| circle | `X` | `b` |
| square | `C` | `x` |
| triangle | `V` | `y` |
| up, down, left, right | arrow keys | `dpup`, `dpdown`, `dpleft`, `dpright` |
| l1 / r1 | `Q` / `E` | `leftshoulder` / `rightshoulder` |
| l2 / r2 | `1`, `Mouse Right` / `3`, `Mouse Left` | triggers |
| l3 / r3 | `F` / `H` | `leftstick` / `rightstick` |
| options | `Tab`, `Backspace` | `start` |
| touchpad | `T` | `back`, `touchpad` |
| left stick | `W`, `A`, `S`, `D` | left stick |
| right stick | `I` (up), `J` (left), `K` (down), `L` (right), mouse look | right stick |

`guide` is unbound. Both stick deadzones are 0.10, the trigger deadzone is 0,
mouse look is on with sensitivity 1 and no inversion, and `device` is empty.

A keyboard L2 or R2 reports the trigger button and full analog travel (255).

## Gamepad feedback and sensors

These follow the active gamepad and need no profile settings.

- **Vibration.** `scePadSetVibration` drives the gamepad motors: the large
  motor runs the low-frequency (heavy) motor and the small one the
  high-frequency motor, with 0..255 scaled onto 0..0xFFFF. A strength holds
  until the guest's next call.
- **Light bar.** `scePadSetLightBar` sets the gamepad LED. Opening the pad and
  `scePadResetLightBar` show the user 1 colour, blue.
- **Motion.** Opening a gamepad enables its accelerometer and gyroscope when it
  has them. The pad record reports acceleration in G and angular velocity in
  rad/s, in the gamepad's own axes. The orientation quaternion integrates the
  gyroscope; `scePadResetOrientation` restarts it from identity. A source
  without sensors, such as the keyboard, reads as a motionless pad.
- **Touchpad.** Up to two fingers on the gamepad touchpad reach the record on
  the 1920 x 943 surface that `scePadGetControllerInformation` reports. Each
  new contact takes the next touch id in 1..127. The touchpad click is the
  `touchpad` button.

## Limits

- Pad routes record a stick only as full travel past half deflection, so a
  recorded mouse-look turn replays as a full-speed turn, and slower turns are
  not recorded ([input replay](input-replay.md)).
- Sticks and triggers cannot be remapped to buttons, and buttons cannot drive
  sticks on a gamepad.
- Vibration that the guest holds unchanged for more than about 65 s stops:
  the host gamepad layer caps one rumble request at that length.
- Motion has no accelerometer tilt correction and no angular-velocity
  deadband; `scePadSetTiltCorrectionState` and
  `scePadSetAngularVelocityDeadbandState` are accepted and have no effect, so
  the integrated orientation can drift.
- The axes of a motionless keyboard pad (gravity on +Z) are not verified
  against a physical gamepad, whose resting reading follows its own axes.
- Adaptive-trigger effects are accepted and ignored.
- Vibration, light bar, motion and touchpad are verified with SDL virtual
  gamepads only, not with a physical gamepad.
