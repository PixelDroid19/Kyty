# Contributing to Kyty

Thank you for helping improve Kyty. Contributions may include code,
documentation, tests, platform support, diagnostics, and carefully documented
research.

By contributing, you confirm that you have the right to submit your work and
that it may be distributed under the repository's MIT License.

## Before you begin

- Search existing issues and pull requests before starting duplicate work.
- Keep changes small and focused on one problem.
- Discuss major architecture changes in an issue or draft pull request first.
- Preserve existing behavior unless the pull request clearly explains why a
  change is necessary.
- Prefer general implementations over game-specific workarounds.
- Never include secrets, personal paths, private dumps, or protected content.

## Legal and clean-room requirements

Contributions must be original work or derived from material whose license is
compatible with this repository.

Do not submit:

- proprietary console source code, SDK code, headers, documentation, or tools;
- firmware, BIOS files, encryption keys, certificates, decrypted executables,
  game assets, shaders, or other copyrighted platform content;
- code copied from a project with an incompatible license;
- instructions or features whose purpose is piracy, unauthorized access, or
  circumvention of access controls;
- private reverse-engineering artifacts that you are not authorized to share.

Reverse engineering must rely on lawful clean-room techniques, public
information, your own original research, or sources that can be cited and used
under compatible terms. Record the observable contract being implemented, not
private identifying details from the test material.

## Development workflow

Kyty uses two long-lived integration branches:

- `main` contains ongoing development. Create `feature/*`, `refactor/*`, and
  other evolutionary branches from `main`, then open the pull request back to
  `main`.
- `release` contains the stable release line. Create `fix/*` branches from
  `release`, then open the pull request back to `release`.

Do not merge new features into `release`. A bug fix needed by ongoing
development should be integrated into `release` first and then brought forward
to `main` without rewriting either branch's published history.

Suggested commands for a feature:

```sh
git fetch origin
git switch --create feature/short-description origin/main
```

Suggested commands for a bug fix:

```sh
git fetch origin
git switch --create fix/short-description origin/release
```

The contribution flow is:

1. Fork the repository and select the correct base branch.
2. Make one coherent change per branch.
3. Add or update tests and documentation where appropriate.
4. Build the affected targets locally.
5. Review the complete diff for generated files, secrets, private paths, and
   unrelated formatting changes.
6. Open a pull request with clear verification evidence.

## Versioning and releases

Kyty follows Semantic Versioning:

- `major`: incompatible or foundational change from `main`;
- `minor`: backward-compatible functionality from `main`;
- `patch`: backward-compatible bug fix from `release`.

Maintainers use the **Create Version** workflow in GitHub Actions and choose the
version component to increment. The workflow selects the source branch, creates
an immutable `vMAJOR.MINOR.PATCH` tag, and starts the multiplatform release
build. Do not create release tags manually unless recovering the automation.

The build workflow runs for pull requests into `main` or `release`, version
tags, and explicit manual requests. It does not build every push to every topic
branch.

Use descriptive commit messages. A concise conventional form is preferred:

```text
fix(graphics): preserve attachment state across queue reset
docs(build): clarify Linux Vulkan dependencies
test(loader): cover malformed relocation metadata
```

Avoid commit messages that describe private workloads, disclose local paths, or
claim compatibility without reproducible evidence.

## Building

The GitHub Actions workflow defines the currently tested Windows, Linux, and
macOS environments. A release build can be configured from the repository root
with:

```sh
cmake -S source -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKYTY_PROJECT_NAME=Emulator \
  -DKYTY_WARNINGS_ARE_ERRORS=OFF \
  -DKYTY_BUILD_LAUNCHER=OFF \
  -DKYTY_BUILD_DEVTOOLS=OFF \
  -DKYTY_BUILD_UNIT_TESTS=OFF \
  -DKYTY_GENERATE_MAP_CSV=OFF

cmake --build build --target fc_script --parallel 4
```

If your change affects optional components, enable and build those components
as part of your verification.

## Testing

Run the narrowest relevant tests first, followed by a build of every affected
target. When embedded unit tests are available in your configuration, they can
be invoked through `fc_script`:

```sh
build/fc_script '{kyty_run_tests()}' --gtest_filter='RelevantSuite.*'
```

A pull request should state exactly what was tested. Include:

- operating system and architecture;
- compiler and build type;
- CPU, GPU, driver, and Vulkan implementation when graphics are involved;
- commands and test filters;
- result, limitations, and any tests that could not be run.

Do not describe a workload as playable or supported solely because it boots,
opens a window, or renders a frame. Compatibility claims require repeatable
evidence and should mention known regressions or limitations.

## Coding guidelines

- Match the style of the surrounding C and C++ code.
- Preserve the existing tab and alignment conventions in source files.
- Keep platform-specific behavior behind clear platform boundaries.
- Prefer explicit ownership, bounded resource use, and deterministic cleanup.
- Validate guest-controlled sizes, offsets, counts, and pointers.
- Keep diagnostics actionable and avoid noisy per-frame logging.
- Comments should explain contracts and design decisions rather than restating
  the code.
- Avoid broad formatting or mechanical rewrites in functional pull requests.
- Do not commit build directories, binaries, generated captures, logs, caches,
  IDE state, or local configuration.

Treat warnings, sanitizer findings, validation-layer messages, races, and
resource leaks as correctness signals. Do not suppress them without documenting
the underlying reason.

## Pull requests

Every pull request should contain:

- a short description of the problem;
- the reasoning behind the chosen solution;
- the affected subsystems and platforms;
- exact validation commands and results;
- compatibility or performance impact;
- screenshots only when they materially demonstrate a visual change and contain
  no protected or private content.

Reviewers may request smaller commits, additional tests, clearer provenance, or
changes to protect portability and maintainability. Keep the branch current
with `main` and resolve review comments without rewriting unrelated code.

## AI-assisted contributions

AI-assisted work is allowed, but the contributor remains fully responsible for
the result. You must understand, review, test, explain, debug, and maintain every
submitted line.

Do not rely on generated claims about APIs, licenses, hardware behavior, or
compatibility without verifying them. Disclose meaningful AI assistance when it
affected implementation or research decisions, and describe the human
verification performed. Large generated changes without clear ownership or
evidence may be rejected.

## Reporting security issues

Do not publicly disclose secrets, exploit chains, private dumps, or instructions
that enable unauthorized access. Report vulnerabilities in Kyty itself privately
to the maintainers when possible and provide a minimal reproduction containing
no protected third-party material.

## Getting help

Use GitHub Issues for reproducible defects and narrowly scoped feature requests.
Use a draft pull request when early design feedback would be useful. Keep all
support requests lawful, technical, and free of copyrighted attachments.
