# Add-on content (DLC)

Kyty answers every add-on content query from one inventory
(`Emulator/Loader/AddcontInventory.h`). `sceAppContent*` and
`sceNpEntitlementAccess*` read the same records, so a title can never see a
label as owned through one library and unowned through the other.

The inventory is loaded by `kyty_load_addcont(app0_root, packages_root)` after
`kyty_load_param_json` (the title id comes from `sce_sys/param.json`).
`scripts/run_guest.lua` passes the guest root and `KYTY_ADDCONT_DIR`.

## Sources

### `dlc_emu.ini` next to the application

Dumps made with the add-on content emulation tool carry `/app0/dlc_emu.ini`.
Each record is a section:

```ini
[PSAL]
content_id=EP0000-PPSA00000_00-0123456789ABCDEF

[PSAC]
content_id=EP0000-PPSA00000_00-FEDCBA9876543210
mount_point=/app0/dlc1
```

- `[PSAC]` is content with data; `[PSAL]` is a license only.
- `content_id` (required) must belong to the running title: the 9 characters
  after the publisher id are the title id. The entitlement label is the last 16
  characters.
- `download_status`: `INSTALLED` (the default when absent) or `NO_EXTRA_DATA`.
- `mount_point`: a directory under `/app0/` that exists in the dump, shorter
  than 16 characters. Only `[PSAC]` records with a mount point are mountable.
- `entitlement_key`: 32 hex digits; absent keys read as 16 zero bytes.
- `np_service_label`: a 32-bit decimal service label; absent or `-1` matches
  every service label.

Unknown sections or properties, duplicate labels or mount points, malformed
values, and content ids of another title make the inventory **invalid**: the
error is logged and the queries answer `DRM_NO_ENTITLEMENT` instead of
pretending nothing is installed.

### Installed packages

With `KYTY_ADDCONT_DIR=/path/to/addcont`, every directory under
`/path/to/addcont/<TITLE_ID>/` is one installed package with data (PSAC). Its
`sce_sys/param.json` must declare the `contentId`. Packages are numbered in
name order and mounted at `/addcont0`, `/addcont1`, ... while the title holds
them mounted.

## Guest contract

| Function | Result |
|---|---|
| `sceAppContentGetAddcontInfoList` | `hit_num` = every entry of the service label; the list receives as many as fit |
| `sceAppContentGetAddcontInfo` | label + download status, or `DRM_NO_ENTITLEMENT` (0x80D90007) |
| `sceAppContentGetEntitlementKey` | 16-byte key, or `DRM_NO_ENTITLEMENT` |
| `sceAppContentAddcontMount` | 16-byte zero-padded mount point; `NOT_FOUND` (0x80D90005) for unknown, license-only or not installed content; `BUSY` (0x80D90003) while already mounted |
| `sceAppContentAddcontUnmount` | releases the claim; `NOT_FOUND` for a mount point that is not mounted |
| `sceNpEntitlementAccessGetAddcontEntitlementInfo[List]` | label, package type (PSAC 2, PSAL 3) and download status; `NO_ENTITLEMENT` (0x817D0007) |
| `sceNpEntitlementAccessGetEntitlementKey` | 16-byte key, or `NO_ENTITLEMENT` |

Null pointers and labels that are empty or not NUL terminated within 17 bytes
are `PARAMETER` errors.

## Status

A license-only inventory loads and is reported by a dumped title of the
regression set. No title in the local library mounts add-on data yet, so the
mount path (`/app0/` data and `/addcontN` packages) is implemented from the
published contract but not yet exercised by a guest.
