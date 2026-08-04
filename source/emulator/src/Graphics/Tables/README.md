# Graphics table outputs

The `TileTextureInfo_*.inc` files are generated C++ initializers consumed by
`Graphics/Tile.cpp`. Their original generator and input corpus are not part of
this checkout, so the checked-in outputs are treated as versioned artifacts for
now. `manifest.sha256` is the review boundary: every table must be listed and
must match its recorded digest.

Run the provenance check from the repository root:

```sh
python3 scripts/check_graphics_tables.py \
  source/emulator/src/Graphics/Tables/manifest.sha256
```

Run the deterministic checker fixtures with:

```sh
python3 scripts/check_graphics_tables.py --self-test
```

Changing a table requires a reviewed deterministic generator and versioned
inputs to be added before updating the manifest. The check rejects missing,
unlisted, or modified outputs, so `.inc` drift cannot pass silently.
