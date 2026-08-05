#!/usr/bin/env python3
"""Deterministic generator for the graphics tile tables.

Regenerates the TileTextureInfo_*.inc files under
source/emulator/src/Graphics/Tables from the maintained sources
(sources/tile_tables.py) and reproduces the byte-exact checked-in output.

Coverage: the micro-tiled families (13_*) and the linear family (0_56)
have derived generators. The display/depth families (2_4_7, 8_*, 10_10_0,
14_10_0) remain locked artifacts in the manifest until their tiling rules
are derived; the checker verifies their digests as before.

Modes:
    --check   regenerate into a temporary directory and byte-compare with
              the checked-in tables, then verify the provenance manifest.
    --write   regenerate the tables in place and refresh the manifest.
    --self-test  run the deterministic fixtures.
"""

import argparse
import hashlib
import pathlib
import shutil
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLES_DIR = REPO_ROOT / "source/emulator/src/Graphics/Tables"
SOURCES_MODULE = TABLES_DIR / "sources" / "tile_tables.py"
MANIFEST = TABLES_DIR / "manifest.sha256"

# Guest micro-tiled format parameters: (block_width, block_height, bytes_per_element).
MICRO_TILED_FORMATS = {
    (1, 0): (1, 1, 1),
    (3, 0): (1, 1, 2),
    (10, 0): (1, 1, 4),
    (10, 9): (1, 1, 4),
    (35, 0): (4, 4, 8),
    (36, 0): (4, 4, 16),
    (37, 0): (4, 4, 16),
    (37, 9): (4, 4, 16),
}

LINEAR_FORMAT = 56
LINEAR_BYTES_PER_ELEMENT = 4
LINEAR_MIN_PITCH = 64

# Every table now has a derived generator; the layout rules were derived
# from the locked outputs and the runtime fallbacks in Tile.cpp.
DERIVED_MICRO_TILED = {"13_1_0", "13_10_0", "13_10_9", "13_35_0", "13_36_0", "13_37_0", "13_37_9"}
DERIVED_LINEAR = {"0_56"}
DERIVED_DISPLAY_LINEAR = {"8_1_0", "8_10_0", "8_10_9"}
DERIVED_DISPLAY_THIN = {"10_10_0"}
DERIVED_THIN_2D = {"14_10_0"}
DERIVED_DEPTH_THIN = {"2_4_7"}
ALL_DERIVED = (
    DERIVED_MICRO_TILED | DERIVED_LINEAR | DERIVED_DISPLAY_LINEAR | DERIVED_DISPLAY_THIN | DERIVED_THIN_2D | DERIVED_DEPTH_THIN
)


def align_up(value, alignment):
    return ((value + alignment - 1) // alignment) * alignment


def round_up_pow2(value):
    result = 1
    while result < value:
        result *= 2
    return result


def micro_tiled_rows(dfmt, nfmt, width, height, pitch, levels, pow2_section):
    """Rows for the thin-1d micro-tiled families (tile 13).

    The pow2 sections halve mip dimensions directly; the fixed sections
    round each halved dimension up to a power of two and, for multi-level
    surfaces, store a size field for level 0 that includes the first mip
    (the historical generator quirk preserved in the locked outputs).
    """
    block_width, block_height, bytes_per_element = MICRO_TILED_FORMATS[(dfmt, nfmt)]

    if pow2_section:
        mip_dims = []
        mw, mh, mp = width, height, (pitch if pitch else width)
        for _ in range(levels):
            mip_dims.append((mw, mh, mp))
            mw, mh, mp = max(mw // 2, 1), max(mh // 2, 1), max(mp // 2, 1)
    else:
        mip_dims = [(width, height, pitch if pitch else width)]
        mw, mh, mp = width, height, (pitch if pitch else width)
        for _ in range(1, levels):
            mw, mh, mp = round_up_pow2(mw // 2), round_up_pow2(mh // 2), round_up_pow2(mp // 2)
            mip_dims.append((mw, mh, mp))

    rows = []
    for (mw, mh, mp) in mip_dims:
        element_width = max((mw + block_width - 1) // block_width, 1)
        element_height = max((mh + block_height - 1) // block_height, 1)
        element_pitch = max((mp + block_width - 1) // block_width, element_width)
        padded_width = align_up(element_pitch, 8)
        padded_height = align_up(element_height, 8)
        raw_size = padded_width * padded_height * bytes_per_element
        if pow2_section or levels == 1:
            size_field = raw_size
        else:
            size_field = padded_width * round_up_pow2(padded_height) * bytes_per_element
        rows.append((raw_size, size_field, padded_width, padded_height))

    offsets = [0]
    for index in range(1, levels):
        offsets.append(offsets[index - 1] + rows[index - 1][1])

    output = []
    for index in range(levels):
        raw_size, size_field, padded_width, padded_height = rows[index]
        output.append((size_field, offsets[index], offsets[index] + raw_size, padded_width, padded_height, 256))
    return output


def linear_rows(width, height, pitch, levels):
    """Rows for the k8_8_8_8UNorm linear family (format 56).

    Mip pitches halve down to a floor of 64; heights halve to 1. The guest
    memory layout stores the smallest mip first, so the emitted {offset,
    size} pairs order the largest mip first with offsets measured from the
    end of the allocation.
    """
    sizes = []
    padded = []
    for level in range(levels):
        mip_pitch = max(pitch // (2**level), LINEAR_MIN_PITCH)
        mip_height = max(height // (2**level), 1)
        sizes.append(mip_pitch * mip_height * LINEAR_BYTES_PER_ELEMENT)
        padded.append((mip_pitch, mip_height))

    offsets = []
    cursor = 0
    for size in reversed(sizes):
        offsets.append(cursor)
        cursor += size
    offsets.reverse()

    total = sum(sizes)
    output = []
    for level in range(levels):
        output.append((sizes[level], offsets[level], total, padded[level][0], padded[level][1], 256))
    return output


def display_linear_rows(pitch, height, levels, bytes_per_element):
    """Rows for the Display_LinearAligned families (tile 8).

    The pitch halves down to a 64-byte floor; the height halves to 1. Every
    level reserves at least 256 bytes.
    """
    output = []
    offset = 0
    for level in range(levels):
        pitch_level = max(pitch // (2**level), 64 // bytes_per_element)
        height_level = max(height // (2**level), 1)
        size = max(pitch_level * height_level * bytes_per_element, 256)
        output.append((size, offset, offset + size, 256))
        offset += size
    return output


def display_thin_rows(width, height, pitch, levels, neo):
    """Rows for the Display_2dThin family (tile 10, format 10/0).

    The surface pads to 64-texel (base) or 128-texel (neo) rows; level 0
    additionally aligns to the 32 KiB / 64 KiB tile base.
    """
    output = []
    offset = 0
    for level in range(levels):
        element_width = max(width // (2**level), 1)
        element_height = max(height // (2**level), 1)
        element_pitch = max(pitch // (2**level), element_width)
        padded_width = align_up(element_pitch, 8)
        padded_height = align_up(element_height, 128 if neo else 64)
        raw_size = padded_width * padded_height * 4
        tile_align = 65536 if neo else 32768
        size = align_up(raw_size, tile_align) if level == 0 else raw_size
        output.append((size, offset, offset + size, tile_align))
        offset += size
    return output


def thin_2d_rows(width, height, pitch, levels, neo):
    """Rows for the Thin_2dThin family (tile 14, format 10/0).

    Level 0 aligns to a tile base that scales with the surface pitch
    (256 * pitch base, 512 * pitch neo); deeper levels stay raw. The
    reported alignment field is the constant 32 KiB / 64 KiB grid.
    """
    output = []
    offset = 0
    for level in range(levels):
        element_width = max(width // (2**level), 1)
        element_height = max(height // (2**level), 1)
        element_pitch = max(pitch // (2**level), element_width)
        padded_width = align_up(element_pitch, 8)
        padded_height = align_up(element_height, 8)
        raw_size = padded_width * padded_height * 4
        field_align = 65536 if neo else 32768
        size_align = (pitch * 512) if neo else (pitch * 256)
        size = align_up(raw_size, size_align) if level == 0 else raw_size
        output.append((size, offset, offset + size, field_align))
        offset += size
    return output


def depth_thin_rows(width, height, pitch, levels, neo):
    """Rows for the Depth_2dThin_256 family (tile 2, format 4/7).

    The depth surface pads its height to 32-texel (base) / 128-texel (neo)
    rows and reserves the 32 KiB / 64 KiB block grid.
    """
    output = []
    offset = 0
    for level in range(levels):
        padded_width = align_up(max(pitch, width), 8)
        padded_height = align_up(max(height, 1), 128 if neo else 32)
        size = padded_width * padded_height * 4
        tile_align = 65536 if neo else 32768
        output.append((size, offset, offset + size, tile_align))
        offset += size
    return output


def format_micro_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows):
    sizes = "{" + ", ".join(f"{{{total}, {align}, {offset}, {size}}}" for (size, offset, total, pw, ph, align) in rows) + ", }"
    padded = "{ " + ", ".join(f"{{{pw}, {ph}}}" for (size, offset, total, pw, ph, align) in rows) + ",  }"
    return f"\t{{ {dfmt}, {nfmt}, {width}, {height}, {pitch}, {levels}, {tile}, {str(neo).lower()}, {sizes}, {padded} }},"


def format_family_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows):
    sizes = "{" + ", ".join(f"{{{total}, {align}, {offset}, {size}}}" for (size, offset, total, align) in rows) + ", }"
    padded = "{ " + ", ".join("{0, 0}" for _ in rows) + ",  }"
    return f"\t{{ {dfmt}, {nfmt}, {width}, {height}, {pitch}, {levels}, {tile}, {str(neo).lower()}, {sizes}, {padded} }},"


def format_linear_row(fmt, width, height, pitch, levels, tile, rows):
    total = rows[0][2]
    align = rows[0][5]
    sizes = "{" + ", ".join(f"{{{offset}, {size}}}" for (size, offset, total, pw, ph, align) in rows) + ", }"
    padded = "{ " + ", ".join(f"{{{pw}, {ph}}}" for (size, offset, total, pw, ph, align) in rows) + ",  }"
    return f"\t{{ {fmt}, {width}, {height}, {pitch}, {levels}, {tile}, {total}, {align}, {sizes}, {padded} }},"


def emit_table(table_name, spec):
    lines = ["/* This file is auto-generated */", ""]
    for section in ("", "_pow2"):
        dims = spec["sections"].get(section)
        if dims is None:
            continue
        kind = "TextureInfo" if table_name in ALL_DERIVED - DERIVED_LINEAR else "TextureInfo2"
        lines.append(f"static const {kind} infos_{table_name}{section}[] = {{")
        lines.append("// clang-format off")
        for dim, comment in dims:
            if comment is not None:
                lines.append(f"\t// {comment} ")
            if table_name in DERIVED_MICRO_TILED:
                dfmt, nfmt, width, height, pitch, levels, tile = dim
                rows = micro_tiled_rows(dfmt, nfmt, width, height, pitch, levels, pow2_section=(section == "_pow2"))
                for neo in (False, True):
                    lines.append(format_micro_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows))
            elif table_name in DERIVED_DISPLAY_LINEAR:
                dfmt, nfmt, width, height, pitch, levels, tile = dim
                bpp = 1 if dfmt == 1 else 4
                for neo in (False, True):
                    rows = display_linear_rows(pitch, height, levels, bpp)
                    lines.append(format_family_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows))
            elif table_name in DERIVED_DISPLAY_THIN:
                dfmt, nfmt, width, height, pitch, levels, tile = dim
                for neo in (False, True):
                    rows = display_thin_rows(width, height, pitch, levels, neo)
                    lines.append(format_family_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows))
            elif table_name in DERIVED_THIN_2D:
                dfmt, nfmt, width, height, pitch, levels, tile = dim
                for neo in (False, True):
                    rows = thin_2d_rows(width, height, pitch, levels, neo)
                    lines.append(format_family_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows))
            elif table_name in DERIVED_DEPTH_THIN:
                dfmt, nfmt, width, height, pitch, levels, tile = dim
                for neo in (False, True):
                    rows = depth_thin_rows(width, height, pitch, levels, neo)
                    lines.append(format_family_row(dfmt, nfmt, width, height, pitch, levels, tile, neo, rows))
            else:
                fmt, width, height, pitch, levels, tile = dim
                rows = linear_rows(width, height, pitch, levels)
                lines.append(format_linear_row(fmt, width, height, pitch, levels, tile, rows))
        if not dims:
            lines.append("\t{ 0, 0, 0, 0, 0, 0, 0 }")
        lines.append("// clang-format on")
        lines.append("};")
    return "\n".join(lines) + "\n"


def load_sources():
    namespace = {}
    with open(SOURCES_MODULE, encoding="utf-8") as stream:
        code = stream.read()
    exec(compile(code, str(SOURCES_MODULE), "exec"), namespace)  # noqa: S102 - trusted checked-in sources
    return namespace["SCHEMA_VERSION"], namespace["TABLES"]


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            hasher.update(chunk)
    return hasher.hexdigest()


def generate_all(schema_version, tables, target_dir):
    target_dir.mkdir(parents=True, exist_ok=True)
    produced = {}
    for table_name, spec in sorted(tables.items()):
        if table_name not in ALL_DERIVED:
            continue
        content = emit_table(table_name, spec)
        path = target_dir / f"TileTextureInfo_{table_name}.inc"
        path.write_text(content, encoding="utf-8")
        produced[table_name] = path
    return produced


def verify_manifest(manifest, table_root):
    entries = {}
    for line_number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2 or len(parts[0]) != 64:
            raise ValueError(f"manifest line {line_number}: malformed entry")
        entries[parts[1]] = parts[0]
    errors = []
    for name, expected in sorted(entries.items()):
        observed = digest(table_root / name)
        if observed != expected:
            errors.append(f"checksum mismatch: {name} expected {expected} observed {observed}")
    return errors


def write_manifest(manifest, table_root):
    lines = ["# SHA-256 lock for the generated table outputs in this directory.",
             "# Regenerate this manifest only together with the deterministic generator and",
             "# its versioned inputs; never hand-edit generated .inc files."]
    for path in sorted(table_root.glob("*.inc")):
        lines.append(f"{digest(path)}  {path.name}")
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check(build_dir=None):
    schema_version, tables = load_sources()
    diagnostics = []
    with tempfile.TemporaryDirectory() as directory:
        temp_dir = pathlib.Path(directory)
        generated = generate_all(schema_version, tables, temp_dir)
        for table_name, path in generated.items():
            checked_in = TABLES_DIR / path.name
            if not checked_in.is_file():
                diagnostics.append(f"missing checked-in table: {path.name}")
                continue
            if checked_in.read_bytes() != path.read_bytes():
                diagnostics.append(f"regenerated output differs: {path.name}")
        diagnostics += verify_manifest(MANIFEST, TABLES_DIR)
    return diagnostics


def write():
    schema_version, tables = load_sources()
    generated = generate_all(schema_version, tables, TABLES_DIR)
    write_manifest(MANIFEST, TABLES_DIR)
    return [f"regenerated {len(generated)} tables"]


class GeneratorTests(unittest.TestCase):
    def test_micro_pow2_rows(self):
        rows = micro_tiled_rows(10, 0, 1, 1, 8, 1, pow2_section=True)
        self.assertEqual(rows, [(256, 0, 256, 8, 8, 256)])

    def test_micro_pow2_mip_chain(self):
        rows = micro_tiled_rows(10, 0, 1, 16, 8, 5, pow2_section=True)
        self.assertEqual(rows[0], (512, 0, 512, 8, 16, 256))
        self.assertEqual(rows[1], (256, 512, 768, 8, 8, 256))

    def test_micro_fixed_quirk(self):
        rows = micro_tiled_rows(10, 0, 512, 768, 512, 10, pow2_section=False)
        self.assertEqual(rows[0], (2097152, 0, 1572864, 512, 768, 256))
        self.assertEqual(rows[1], (524288, 2097152, 2621440, 256, 512, 256))

    def test_micro_fixed_single_level(self):
        rows = micro_tiled_rows(35, 0, 1920, 1080, 1920, 1, pow2_section=False)
        self.assertEqual(rows[0], (1044480, 0, 1044480, 480, 272, 256))

    def test_linear_rows(self):
        rows = linear_rows(1, 2, 64, 2)
        self.assertEqual(rows[0], (512, 256, 768, 64, 2, 256))
        self.assertEqual(rows[1], (256, 0, 768, 64, 1, 256))

    def test_linear_min_pitch(self):
        rows = linear_rows(16384, 256, 16384, 2)
        self.assertEqual(rows[0], (16777216, 4194304, 20971520, 16384, 256, 256))
        self.assertEqual(rows[1], (4194304, 0, 20971520, 8192, 128, 256))

    def test_display_linear_rows(self):
        rows = display_linear_rows(32, 8, 6, 4)
        self.assertEqual(rows[0], (1024, 0, 1024, 256))
        self.assertEqual(rows[1], (256, 1024, 1280, 256))
        self.assertEqual(rows[-1], (256, 2048, 2304, 256))

    def test_display_thin_rows(self):
        rows = display_thin_rows(96, 96, 128, 1, neo=False)
        self.assertEqual(rows[0], (65536, 0, 65536, 32768))
        rows = display_thin_rows(128, 1, 128, 1, neo=True)
        self.assertEqual(rows[0], (65536, 0, 65536, 65536))

    def test_thin_2d_rows(self):
        rows = thin_2d_rows(1, 2, 128, 2, neo=False)
        self.assertEqual(rows[0], (32768, 0, 32768, 32768))
        self.assertEqual(rows[1], (2048, 32768, 34816, 32768))
        rows = thin_2d_rows(256, 1, 256, 1, neo=False)
        self.assertEqual(rows[0], (65536, 0, 65536, 32768))

    def test_depth_thin_rows(self):
        rows = depth_thin_rows(1920, 1080, 1920, 1, neo=False)
        self.assertEqual(rows[0], (8355840, 0, 8355840, 32768))
        rows = depth_thin_rows(1920, 1080, 1920, 1, neo=True)
        self.assertEqual(rows[0], (8847360, 0, 8847360, 65536))

    def test_emit_is_byte_stable(self):
        schema_version, tables = load_sources()
        with tempfile.TemporaryDirectory() as directory:
            first = generate_all(schema_version, tables, pathlib.Path(directory))
            second = generate_all(schema_version, tables, pathlib.Path(directory) / "again")
            for name, path in first.items():
                self.assertEqual(path.read_bytes(), (second[name]).read_bytes())

    def test_regeneration_matches_checked_in(self):
        schema_version, tables = load_sources()
        with tempfile.TemporaryDirectory() as directory:
            generated = generate_all(schema_version, tables, pathlib.Path(directory))
            for table_name, path in generated.items():
                checked_in = TABLES_DIR / path.name
                self.assertTrue(checked_in.is_file(), f"missing {checked_in}")
                self.assertEqual(path.read_bytes(), checked_in.read_bytes(), f"drift in {table_name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="regenerate to temp and byte-compare")
    parser.add_argument("--write", action="store_true", help="regenerate in place and refresh the manifest")
    parser.add_argument("--self-test", action="store_true", help="run deterministic fixtures")
    args = parser.parse_args()

    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(GeneratorTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1

    if args.check:
        diagnostics = check()
        for diagnostic in diagnostics:
            print(diagnostic)
        return 1 if diagnostics else 0

    if args.write:
        for diagnostic in write():
            print(diagnostic)
        return 0

    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
