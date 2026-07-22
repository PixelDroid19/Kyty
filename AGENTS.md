# Kyty engineering guide

This file defines the repository-wide rules for maintainers, contributors, and
automated coding agents. More specific instructions may exist inside a
subdirectory; when they do, the closest applicable file takes precedence.

## Commit messages

Kyty follows [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/).
Every commit and squash-merge title must use this form:

```text
<type>[optional scope][optional !]: <description>

[optional body]

[optional footer(s)]
```

Use lowercase types and an imperative, concise description. The primary types
are:

- `feat`: a new user-visible or emulator capability; normally implies a minor
  release;
- `fix`: a bug fix; normally implies a patch release;
- `perf`: a measurable performance improvement;
- `refactor`: an internal change without new behavior or a bug fix;
- `test`: test-only changes;
- `docs`: documentation-only changes;
- `build`: build-system or dependency changes;
- `ci`: automation and release-pipeline changes;
- `chore`: repository maintenance that fits no more specific type;
- `revert`: reverts a previous change and identifies the reverted commit.

Scopes should identify a stable subsystem, for example `graphics`, `loader`,
`audio`, `kernel`, `build`, `ci`, or `docs`:

```text
feat(graphics): add attachment resolve tracking
fix(loader): reject truncated relocation entries
docs(contributing): clarify patch branch policy
```

Breaking changes must use `!` in the prefix or a `BREAKING CHANGE:` footer:

```text
feat(runtime)!: replace the guest module lifecycle contract

BREAKING CHANGE: modules now initialize through the lifecycle coordinator.
```

Do not include private workload names, local paths, secrets, or unsupported
compatibility claims in commit messages. Keep unrelated changes in separate
commits. Pull-request titles must follow the same rules because Kyty uses squash
merges to produce changelog-ready history.

## Branch model

- Create `feature/*`, `refactor/*`, `perf/*`, and documentation branches from
  `main`; merge them back into `main`.
- Create `fix/*` and `hotfix/*` branches from `release`; merge them back into
  `release`.
- Do not merge new features directly into `release`.
- Bring accepted release fixes forward to `main` without rewriting published
  history.
- Never force-push `main`, `release`, or version tags during normal development.

## Versioning and builds

Kyty uses Semantic Versioning:

- `patch` versions are created from `release`;
- `minor` and `major` versions are created from `main`;
- breaking changes require a major version.

Ordinary pushes and pull requests do not create multiplatform binaries. The
full Windows, Linux, and macOS build runs only for an explicit manual request or
an immutable `vMAJOR.MINOR.PATCH` release tag. Use the **Create Version**
workflow to calculate and publish a release; do not create release tags by hand
unless recovering the automation.

## Engineering expectations

- Prefer general, evidence-backed implementations over title-specific hacks.
- Use lawful clean-room research and license-compatible public sources.
- Never commit proprietary SDK code, firmware, keys, decrypted assets, game
  files, private dumps, or other protected material.
- Validate guest-controlled sizes, offsets, counts, handles, and pointers.
- Keep platform-specific behavior behind explicit platform boundaries.
- Bound caches, captures, logs, and other host resource use.
- Add focused tests for corrected contracts and regression-prone behavior.
- Keep diagnostics actionable and avoid noisy per-frame logging.
- Preserve unrelated user changes and generated local artifacts.

## Verification

Before opening or merging a pull request:

1. review the complete diff for scope, secrets, private paths, and generated
   files;
2. run the narrowest relevant tests;
3. build every affected target locally;
4. request a manual hosted build when platform risk justifies it;
5. document exact commands, environment, results, and untested limitations.

Do not claim compatibility from a boot, window, or isolated frame alone.
Compatibility reports must identify the commit, host, GPU, driver, workload,
duration, and known limitations.
