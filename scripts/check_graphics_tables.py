#!/usr/bin/env python3
"""Verify that checked-in graphics table outputs match their provenance lock."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import tempfile
import unittest


CHUNK_SIZE = 1024 * 1024
TABLE_SUFFIX = ".inc"


def parse_manifest(path: pathlib.Path) -> dict[str, str]:
	entries: dict[str, str] = {}
	try:
		lines = path.read_text(encoding="utf-8").splitlines()
	except OSError as error:
		raise ValueError(f"cannot read manifest {path}: {error}") from error
	for line_number, line in enumerate(lines, start=1):
		line = line.strip()
		if not line or line.startswith("#"):
			continue
		parts = line.split()
		if len(parts) != 2 or len(parts[0]) != 64 or any(c not in "0123456789abcdef" for c in parts[0]):
			raise ValueError(f"manifest line {line_number}: expected lowercase sha256 and filename")
		name = parts[1]
		if pathlib.PurePath(name).name != name or not name.endswith(TABLE_SUFFIX):
			raise ValueError(f"manifest line {line_number}: unsafe table filename {name!r}")
		if name in entries:
			raise ValueError(f"manifest line {line_number}: duplicate table filename {name!r}")
		entries[name] = parts[0]
	return entries


def digest(path: pathlib.Path) -> str:
	hasher = hashlib.sha256()
	with path.open("rb") as stream:
		while chunk := stream.read(CHUNK_SIZE):
			hasher.update(chunk)
	return hasher.hexdigest()


def verify(manifest: pathlib.Path) -> list[str]:
	entries = parse_manifest(manifest)
	table_root = manifest.parent
	actual = {path.name for path in table_root.glob(f"*{TABLE_SUFFIX}") if path.is_file()}
	errors: list[str] = []
	for name in sorted(set(entries) - actual):
		errors.append(f"missing generated table: {name}")
	for name in sorted(actual - set(entries)):
		errors.append(f"unlisted generated table: {name}")
	for name in sorted(set(entries) & actual):
		observed = digest(table_root / name)
		if observed != entries[name]:
			errors.append(f"checksum mismatch: {name} expected {entries[name]} observed {observed}")
	return errors


class GraphicsTableCheckerTests(unittest.TestCase):
	def write_table(self, root: pathlib.Path, name: str, contents: bytes = b"{ 1, 2, 3 }\n") -> str:
		path = root / name
		path.write_bytes(contents)
		return digest(path)

	def write_manifest(self, root: pathlib.Path, lines: list[str]) -> pathlib.Path:
		manifest = root / "manifest.sha256"
		manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
		return manifest

	def test_accepts_matching_outputs(self) -> None:
		with tempfile.TemporaryDirectory() as directory:
			root = pathlib.Path(directory)
			checksum = self.write_table(root, "TileTextureInfo_test.inc")
			manifest = self.write_manifest(root, [f"{checksum}  TileTextureInfo_test.inc"])
			self.assertEqual(verify(manifest), [])

	def test_rejects_checksum_mismatch(self) -> None:
		with tempfile.TemporaryDirectory() as directory:
			root = pathlib.Path(directory)
			self.write_table(root, "TileTextureInfo_test.inc")
			manifest = self.write_manifest(root, [f"{'0' * 64}  TileTextureInfo_test.inc"])
			self.assertEqual(len(verify(manifest)), 1)
			self.assertIn("checksum mismatch", verify(manifest)[0])

	def test_rejects_missing_and_unlisted_outputs(self) -> None:
		with tempfile.TemporaryDirectory() as directory:
			root = pathlib.Path(directory)
			checksum = self.write_table(root, "TileTextureInfo_actual.inc")
			manifest = self.write_manifest(
				root,
				[
					f"{checksum}  TileTextureInfo_actual.inc",
					f"{'1' * 64}  TileTextureInfo_missing.inc",
				],
			)
			errors = verify(manifest)
			self.assertIn("missing generated table: TileTextureInfo_missing.inc", errors)

			self.write_table(root, "TileTextureInfo_unlisted.inc")
			self.assertIn("unlisted generated table: TileTextureInfo_unlisted.inc", verify(manifest))

	def test_rejects_unsafe_and_duplicate_manifest_entries(self) -> None:
		with tempfile.TemporaryDirectory() as directory:
			root = pathlib.Path(directory)
			for contents, expected in (
				(f"{'0' * 64}  ../TileTextureInfo_escape.inc\n", "unsafe table filename"),
				(
					f"{'0' * 64}  TileTextureInfo_test.inc\n{'1' * 64}  TileTextureInfo_test.inc\n",
					"duplicate table filename",
				),
			):
				manifest = root / "manifest.sha256"
				manifest.write_text(contents, encoding="utf-8")
				with self.assertRaisesRegex(ValueError, expected):
					parse_manifest(manifest)


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("manifest", type=pathlib.Path, nargs="?")
	parser.add_argument("--self-test", action="store_true", help="run deterministic checker fixtures")
	args = parser.parse_args()
	if args.self_test:
		suite = unittest.defaultTestLoader.loadTestsFromTestCase(GraphicsTableCheckerTests)
		result = unittest.TextTestRunner(verbosity=2).run(suite)
		return 0 if result.wasSuccessful() else 1
	if args.manifest is None:
		parser.error("the following arguments are required: manifest")
	manifest = args.manifest.resolve()
	try:
		errors = verify(manifest)
	except ValueError as error:
		print(f"graphics table provenance failed: {error}", file=sys.stderr)
		return 1
	if errors:
		for error in errors:
			print(f"graphics table provenance failed: {error}", file=sys.stderr)
		return 1
	print(f"graphics table provenance passed: {len(parse_manifest(manifest))} tables")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
