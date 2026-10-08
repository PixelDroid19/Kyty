# Runtime and graphics contract completion

## Scope and baseline

Start from the committed feature-branch HEAD `b208fb3e`. Work in an isolated
checkout. Preserve every uncommitted change and process in the source checkout.
Use separately scoped local implementation sessions, with the maintainer-facing
orchestrator owning integration and independent verification.

Implement general behavior only. External implementations supply investigation
leads; derive behavioral specifications from published contracts, independent
agreement and executable synthetic fixtures. Do not copy external source or
fixtures. Do not invent descriptors, ABI values, compression metadata or success
results. Runtime and gameplay acceptance remain separate from synthetic proof.

## Workstreams and dependencies

1. **Audio sampler rendering.** Extend the existing NGS2 voice lifecycle and
   mixer to standard PCM sampler blocks, source-rate resampling, gain and block
   completion/repeat semantics. Use the existing audio/video backend for a
   bounded elementary compressed-audio decoder where its contract is verified.
   Test real voice control through system rendering, malformed spans and reset.
2. **One-dimensional texture arrays.** Preserve the array coordinate through
   resource classification, layout, Vulkan view creation and SPIR-V image
   operations. Validate sampling and load including layer bounds and addressing;
   refuse instruction forms without a published contract. Preserve
   two-dimensional arrays, cube faces and volume paths.
3. **Shared global data store.** After texture emitter ownership is released,
   unify the command processor and shader address space, preserve ordered
   append/consume behavior, and implement validated memory transfers. Test
   transfers, offsets, bounds and shader visibility; never treat global storage
   as local workgroup storage.
4. **Sparse dirty-range tracking.** Replace fixed range-count admission with
   bounded scalable metadata while preserving fault-handler safety, guest
   permission ownership and alias lifetime. Verify more than 512 disjoint
   ranges and exact dirty-page queries.
5. **CPU-demand GPU publication.** Depends on dirty-range lifecycle proof.
   Propagate access type at the host fault boundary and publish completed GPU
   writes through a safe worker, outside the signal handler. Test GPU write to
   CPU read, clean neighbors, prohibited reads, remaps and aliases. Measure
   traffic before making performance claims.
6. **Shared shader control-flow analysis.** After emitter changes settle,
   introduce canonical block/edge/loop information consumed by a specific
   currently heuristic translation path. Preserve the existing lane and mask
   analyses. Test nested loops, break/continue and multiple merges, including
   strict refusal where structurization cannot be established.
7. **Indexed sampled-resource tables.** First prove a runtime-key table pattern
   is missing in the committed baseline. Implement only evidenced table plans
   with bounds and generation-aware materialization. Existing dynamic scalar
   loads must remain covered. No zero-resource fallback.
8. **Hardware arithmetic oracle.** Add an optional direct compute measurement
   tool and pinned dependency description. Reuse the existing Vulkan execution
   fixtures as the comparison endpoint. Missing supported hardware is an
   explicit unavailable result, never fabricated expected values.
9. **Offline operation dependencies.** Build a bounded analysis graph from
   owned operation records and explicit resource spans; record external inputs
   and nearest preceding overlapping writers. The graph is diagnostic evidence,
   not a substitute for execution synchronization.
10. **Exact resource-fold replay.** Capture owned evaluator inputs and ordered
    reads, then replay with strict missing, reordered and unused-read checks.
    Keep private captures outside version control and use synthetic fixtures.
11. **Compression metadata first-write qualification.** Determine whether the
    existing expanded-surface path already satisfies the observed contract.
    Change publication only with proven metadata layout, extent and completion;
    leave unproven behavior strictly rejected and record the exact evidence gap.

Audio and standalone tooling may run independently. GPU emitters, binding and
shared headers have one writer at a time. Do not run a build or heavy analysis
while a guest workload is active. The build owner uses a dedicated directory
and at most two compilation jobs.

## Validation and acceptance

- Establish an isolated baseline build before integrating semantic changes.
- Add meaningful failing contract fixtures before each behavior change and
  verify that they fail against the unchanged base, then pass with the change.
- Run focused suites and static architecture/table gates for touched domains.
- Independently review the complete diff, with particular attention to bounds,
  permissions, resource lifetime, queue completion, cache invalidation and
  unaffected texture shapes.
- Run the strongest available production-path integration fixtures. A build
  or synthetic fixture does not establish private-title compatibility.
- Reproduce affected strict guest routes when the required private inputs and
  an exclusive run slot are available; otherwise report that limitation and
  preserve existing acceptance claims.
- Record implemented contracts, observed results and unresolved prerequisites
  separately. Keep protected data, raw logs and session transcripts untracked.

## Execution record

The isolation and exact requested implementation models were verified before
starting the work. No global dependency installation is authorized by this
plan. The current host lacks a Nix executable; dependency declarations should
remain reproducible and their unavailable execution must be reported honestly.

### Qualified investigation limits

- CPU-demand publication remains conditional on a measured publication bottleneck.
  A bounded strict startup recorded about 34 MiB of publication, followed by
  zero published bytes during a 15-second interval with continued presentation.
  This observation does not establish a gameplay profile or a performance gain.
  Access-type propagation and write-permission ownership can be validated
  independently; a new read-fault worker is not justified by this measurement.
- Indexed sampled-resource tables require a consumed runtime-key pattern,
  table extent, descriptor stride, uniformity and generation lifetime. Constant
  scalar loads already have a path. A secondary implementation alone does not
  establish an additional table contract for the current strict workload.
- Raw compression metadata publication still lacks an established plane
  extent, ownership, key encoding and first-write completion contract. The
  initialized expanded-surface path is a separate existing qualification.
  Neither guessed metadata words nor a synthetic clear pattern is acceptable.

### Delivered contracts

The isolated change set implements sampler PCM/ATRAC9 rendering, sparse write
tracking with guest permission ownership, one-dimensional image layers, shared
GDS transfers, canonical structured control-flow analysis, exact evaluator replay
and bounded dependency/arithmetic tools. The dependency declaration is pinned;
its evaluation and native AMD measurements remain unavailable. The qualified
investigation limits above are deferred, rather than implemented from assumptions.
Final verification and strict guest limits are recorded in `docs/BRINGUP.md`.
