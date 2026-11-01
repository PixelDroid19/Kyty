# Game patches

A game patch file changes bytes of a loaded guest module before any guest code
runs. The files are user data: the emulator ships none and has no per-title
knowledge of them. A patch changes what the game does, so a run with one is not
a strict run and supports no compatibility or performance claim for the title.

The emulator reads the file named by the `GamePatchFile` config key.
`scripts/run_guest.lua` sets it from `KYTY_GAME_PATCH`; the launcher sets it from
the game's *Game patch file* field. Empty means no patches.

## Format

```json
{
  "id": "PPSA00000",
  "version": "01.000.000",
  "process": "eboot.bin",
  "mods": [
    {
      "name": "Example",
      "enabled": true,
      "memory": [{ "offset": "0x1000", "off": "9090", "on": "c3cc" }]
    }
  ]
}
```

- `id` and `version` must equal the title ID and content version of the game.
- `process` names the module, by file name.
- A mod with `enabled` false adds nothing, and its entries are not validated.
  Choosing mods means editing this flag; the launcher has no per-mod switches.
- `off` holds the expected bytes and `on` the new ones: hex, same size. The JSON
  reader does not accept `\u` escapes, so store names as UTF-8.

## Applying

Patches are applied once, after relocation and before modules start.

1. The first entry with a non-zero `off` is searched for in the module's file-backed
   segments. Each hit gives a candidate base that offsets count from, and the
   candidate holds when every non-zero `off` matches at its translated address.
2. An all-zero `off` declares room for added code and is not compared. Added code
   outside the image gets its own executable pages at the address the offset
   gives, registered as guest code; an address already in use stops the run.
3. New bytes are written and the instruction cache is flushed.

Any mismatch stops the run and names the offset, because the patch was requested
explicitly and would otherwise run against another build.
