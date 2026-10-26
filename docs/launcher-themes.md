# Launcher themes

The desktop launcher accepts themes as add-ons. A theme is data only: colours,
pictures, a font and sounds. It cannot run code, so a theme is safe to share
and works the same on every host.

## Where themes live

Each theme is a folder `themes/<id>/` with a `theme.ini` inside, under the
user data folder:

| Host | Themes folder |
| --- | --- |
| Linux | `~/.local/share/Kyty/themes` (or `$XDG_DATA_HOME/Kyty/themes`) |
| Windows | `%LOCALAPPDATA%\Kyty\themes` |

Settings › Interface shows the exact folder. The launcher looks for themes
when it starts. `<id>` may hold only letters, digits, dots, dashes and
underscores; at most 64 themes are read.

## Choosing a theme

Settings › Interface › Appearance › Style lists the launcher's own look, **Kyty**, and
each theme found, marked **Add-on**. A theme with a fault is listed as
**Can't be used** and cannot be chosen. If the chosen theme fails when the
launcher applies it (removed, damaged file, unreadable font or sound), the
launcher goes back to its own look and shows a notice with the reason.

## theme.ini

```ini
[Theme]
api = 1                 ; required, the format version
name = Midnight         ; required
author = Someone
license = CC0-1.0
version = 1.0
screenshot = shot.png   ; optional files, inside the theme's folder
font = face.ttf
background = wall.png
layout = Console        ; optional: Sidebar or Console

[Dark]
accent = #ff9f43
background = #0a0e28
glass = #22ffffff

[Light]
accent = #d35400

[Sounds]
move = tick.wav
accept = ok.wav
back = back.wav
start = start.wav
```

- `[Dark]` and `[Light]` set colours for each colour scheme, as `#rrggbb` or
  `#aarrggbb`. A colour left out keeps the launcher's own. The keys are
  `background`, `surface`, `panel`, `accent`, `text_primary`,
  `text_secondary`, `text_muted`, `text_on_art`, `glass`, `glass_strong`,
  `glass_border`, `scrim`, `art_edge`, `glow`, `danger`, `ok`, `glitch_red`
  and `glitch_cyan`. A theme's accent wins over the accent setting.
- `font` is a `.ttf` or `.otf` file used for the whole interface.
- `background` is a `.png`, `.jpg` or `.webp` picture drawn behind the pages;
  `screenshot` takes the same formats. Pictures may be at most 8192 × 8192.
- `layout` picks the launcher's layout while the theme is in use, over the
  Layout setting.
- `[Sounds]` replaces any of the interface sounds with a `.wav` file; the
  others keep the launcher's own.
- Unknown sections or keys, files outside the theme's folder, a `theme.ini`
  over 64 KB, files over 16 MB (4 MB for sounds) and texts over 64 characters
  make the theme unusable.

## Layouts

- **Sidebar** (default): the sidebar with Kit moves between the pages, and the
  library shows its games as a grid or a table.
- **Console**: the pages are tabs on the top bar, the games are one row of
  icons with the selected one large at the left, and the selected game's page
  sits below the row with its artwork behind.

Both layouts share the top bar: emulator state, controller battery, search,
clock and the power menu, also opened by holding the controller's back button
(○ on a PlayStation controller).
