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

### Portable completion follow-up (2026-10-08)

The follow-up explicitly requires capability-based behavior across GPU vendors.
The available machine has one physical Vulkan device; hardware coverage on other
vendors must remain distinct from tested feature-selection contracts.

1. Remove the coherent-only memory admission in compute probes. Select a
   buffer-compatible host-visible memory type, prefer coherence, and otherwise
   flush host writes before submission and invalidate device writes after the
   existing host barrier and successful fence wait. Map the entire dedicated
   allocation so whole-allocation maintenance obeys atom alignment.
2. Preserve the identity of recorded guest destinations until GDS publication.
   Capturing only a virtual address allows an old submission to overwrite a new
   writable mapping at the same address. Capture bounded mapping identities and
   validate them under the same VM transaction as the copy. Protection changes
   retain identity; replacing backing or unmapping revokes it. Cover partial
   replacement and unchanged neighbours with meaningful regression fixtures.
3. Audit subgroup query and enablement for promoted core functionality as well
   as advertised extensions. Do not infer enabled features from a vendor name or
   default subgroup width. Preserve strict width and operation admission.
4. Refuse unsatisfied production memory-type requests before native allocation
   or allocator-state changes, and stop after buffer creation, allocation or
   binding failures. Keep requested property flags explicit.
5. Recheck runtime-indexed resources and compression metadata against consumed
   guest evidence. An SGPR descriptor width establishes neither table stride nor
   extent. Any remaining unestablished contract stays explicitly unresolved.

Memory probe and VM publication writers own disjoint files. Core feature routing
is integrated only after its query, enablement and emitted-module requirements
are established. Final builds and GPU executions remain serialized and bounded.

The four demonstrated follow-up defects are implemented. Core-only subgroup
admission, noncoherent probe cache maintenance and allocation error ordering have
compiled behavioral comparisons or native-call models. Mapping identities have
unit and actual GDS completion fixtures, including same-address replacement and
read-only-at-record destinations. The final Linux build and focused/integration
results are recorded in `docs/BRINGUP.md`; other vendor and platform hardware
coverage remains unavailable. Evidence-limited resource-table, compression and
CPU-demand work remains an investigation rather than a guessed implementation.

### Incremental mapping and standard-library audit (2026-10-08)

1. Reproduce a retained flexible mapping imported while CPU-read-only, followed
   by a successful writable protection change. Read distinct markers through
   the production device-address table before and after the transition. Include
   partial ranges, unchanged neighbours and a CPU-only transition on an owner
   whose GPU cleanup obligation is retained. Establish the cache invalidation
   and submission-quiescence ordering before altering the kernel path. Failed
   protection changes must not expose new rights or discard a valid import.
2. Complete the missing double-precision standard distance-function export from
   the established C ABI and independently confirmed symbol identity. Resolve
   and call it through the real symbol database; cover finite scaling, signed
   zero, infinity and NaN rather than an overflow-prone squared-sum formula.
3. Keep audio timestamp/null-drain semantics, unknown mastering parameters and
   conflicting zero-count save-memory rules unresolved until guest evidence
   establishes their contract. Existing atomic save persistence and internal
   flexible-memory hints do not need duplicate implementations.

The GPU fixture and library fixture have distinct file ownership. Reproduce
failures first, then implement the smallest proven fixes in separate sessions.
Build and execute the affected contracts serially, obtain independent review,
then perform bounded strict runs on workloads sharing the changed kernel path.

The reproduced snapshot defect is scoped to previously GPU-visible flexible
owners becoming CPU-writable. Keep the owner lock through pending-unmap checks,
protection and metadata changes, then drop only the affected device-address
imports while submission admission stays closed. Do not detach VideoOut buffers
or ordinary GPU resources for this snapshot-policy change. Split copied imports
by CPU write eligibility, so mixed protected spans retain ongoing write tracking.
The partial-range fixture must include another CPU write after its first refresh.
Physical aliases and broader GPU permission semantics remain outside this fix.

Deferred protection must retain the original mapping identity across GPU
quiescence, then validate the full requested interval atomically with the host
rights change. A first-byte token capture cannot replace full-range validation;
protection ranges are not subject to the bounded deferred-copy byte budget.
Completed unmap/reuse must preserve the new owner's rights and data and return
ENOENT without snapshot invalidation; an unmap already pending returns EBUSY.

Native fixed allocations must also obey that identity transaction: hosts without
no-replace mapping support reserve the exact free interval before MAP_FIXED
commits only over that reservation. Compare frozen production backends with
native no-replace disabled using full and partial collisions, plus identity
and write-lease cases. Keep this compilation control and artifacts outside Git.

This audit is implemented and independently reviewed within its single-owner
scope. Its executed red fixtures cover stale Vulkan markers, absent double
exports, both same-address replacement races and fixed-map partial collisions.
The final focused suite, forced native-backend suite, six integrations and
architecture/provenance gates pass; bounded strict runs retain the recorded
frontiers. Commands, results and untested platform, vendor, visual and
multi-owner limits are recorded in `docs/BRINGUP.md`. No new dependency is added.
