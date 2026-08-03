# Compute storage-image seed path

Compute storage images are normally initialized from their guest backing before
dispatch. That upload is required when the shader reads an existing texel or
when the dispatch leaves part of the target untouched.

The renderer now proves the safe fast path from the guest shader and dispatch:

- the storage descriptor is written by an image instruction and is never read;
- the global dispatch extent covers the complete image in every dimension;
- only then is the guest-to-image seed skipped and the image transitioned from
  `UNDEFINED` to `GENERAL` for the dispatch.

Writeback remains enabled, so the guest receives the newly written image. Any
shader read, partial coverage, sampled alias, or non-generation-5 path keeps the
original seed/upload path.

For a bounded runtime check, set `KYTY_DUMP_STORAGE_SEED_SKIP=1` and inspect the
limited `KYTY_STORAGE_SEED_SKIP` records in the process log. The diagnostic is
disabled by default and capped at 32 records per process.
