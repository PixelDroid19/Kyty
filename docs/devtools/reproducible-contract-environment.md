# Reproducible contract environment

`flake.nix` at the repository root declares the development toolchain. It pins
one nixpkgs commit, so every attribute resolves to the same package set.

```bash
nix develop            # default toolchain for the C++ emulator and the Python tools
nix develop .#oracle   # adds the public ROCm HIP tools for the optional oracle
```

Three things are kept separate, and none of them implies the others:

1. **Attribute verification.** The attribute names below were checked by reading
   the nixpkgs sources at the pinned commit. This is read-only.
2. **Nix evaluation.** The flake has not been evaluated. The host where this was
   written has no `nix` executable, so no `nix develop` or `nix flake check` has run.
3. **An AMD GPU for the oracle.** Installing `hipcc`, `clr` and `rocminfo` does not
   provide hardware. The oracle needs an AMD device exposed through `/dev/kfd`.
   Without it, `scripts/kyty_hardware_oracle.py` reports `unavailable`.

## Pin

- Input: `github:NixOS/nixpkgs/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4`.
  This is an exact commit, dated 2026-10-06. No branch, tag or placeholder hash is used.
- `flake.lock` is not committed. Generating it needs a host with Nix and network
  access. Until it exists, Nix resolves the exact commit above on first use, and
  the content is still determined by that commit. Commit the generated lock file
  after the first `nix flake lock` run and verify it against the same revision.

## Shells

`devShells.x86_64-linux.default`: `cmake`, `ninja`, `pkg-config`, `gcc`, `python3`,
`gdb`, `SDL2`, `vulkan-loader`, `vulkan-headers`, `spirv-tools`, `glslang`, `ffmpeg`.
These match the packages the C++ build expects. The bundled third-party sources
in `source/3rdparty` are still used, so no extra library is added.

`devShells.x86_64-linux.oracle`: the default shell plus `rocmPackages.hipcc`,
`rocmPackages.clr` and `rocmPackages.rocminfo`.

Only `x86_64-linux` is declared. Other systems are not covered.

## Attribute verification

Read at the pinned commit:

| Attribute | Where it was read | Result |
| --- | --- | --- |
| `rocmPackages` | top-level package set | `callPackage ../development/rocm-modules` |
| `rocmPackages.hipcc` | `development/rocm-modules/default.nix` | defined |
| `rocmPackages.clr` | `development/rocm-modules/clr/default.nix` | defined, version 7.2.3 |
| `rocmPackages.rocminfo` | `development/rocm-modules/default.nix` | defined |
| `cmake`, `ninja`, `vulkan-loader`, `vulkan-headers`, `spirv-tools`, `glslang` | `pkgs/by-name/<prefix>/<name>/package.nix` (fetched) | defined |
| `gdb` | `pkgs/by-name/gd/gdb/package.nix` (fetched) | defined |
| `SDL2` | top-level package set | alias of `sdl2-compat` |
| `pkg-config`, `gcc`, `python3`, `ffmpeg` | top-level package set | defined |

`gcc` and `python3` are aliases in the top-level set and resolve to a default
version. A version change in the pinned input changes them as well.

## Not claimed

- That `nix develop` builds or enters the shell on any host.
- That the ROCm toolchain can compile the oracle kernel for any particular GPU.
- That any device measurement exists. See `docs/devtools/offline-contract-tools.md`
  for what the oracle reports.
