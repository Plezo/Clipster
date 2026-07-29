# Contributing

Clipster is a small codebase with a strict rule at the centre: **`main` is
always releasable, and nothing lands on it directly.** Every change —
including one-line fixes and documentation — goes through a branch.

See `CLAUDE.md` for the architecture tour, build commands and the
conventions the code itself follows.

## Branches

Branch off an up-to-date `main`, one topic per branch:

| Prefix     | For                                              |
| ---------- | ------------------------------------------------ |
| `feat/`    | new behaviour (`feat/clips-tab`)                 |
| `fix/`     | bug fixes (`fix/microphone-track`)               |
| `docs/`    | documentation only                               |
| `chore/`   | build, CI, tooling, dependency bumps             |
| `release/` | preparing a release (`release/v0.3.1`) — see below |

```bash
git switch main && git pull
git switch -c fix/microphone-track
```

Keep branches short-lived. Rebase on `main` rather than merging it in, so
history stays linear and the diff under review is only your own work.

## Commits

Imperative subject under ~72 characters, scoped when it helps
("Release script: find a per-user Inno Setup install"). The body is for
*why*, not *what* — the diff already says what. If a commit fixes
something subtle, the body is where the next person learns what the
symptom looked like.

Before pushing:

```bash
# anywhere, including WSL - core is dependency-light and always testable
cmake --preset linux-core && cmake --build --preset linux-core && ctest --preset linux-core
```

```powershell
# Windows, for anything touching media/, platform/win/ or apps/
cmake --build --preset windows-release --parallel
ctest --preset windows
```

WSL cannot compile `media/`, `platform/win/` or `apps/` (no MSVC or
FFmpeg), so changes there must be built on Windows before they are
pushed — CI will catch it otherwise, slowly.

## Pull requests

Open a PR against `main` and let CI go green before merging. Squash or
merge commit are both fine; releases are not tagged from a topic branch,
so nothing depends on the merge strategy.

Delete the branch after merging.

## Releasing

Releases get their own branch so `main` stays open for other work while a
version is being stabilised, and so the version bump is reviewable.

1. **Branch and bump.**

   ```bash
   git switch main && git pull
   git switch -c release/v0.3.1
   ```

   Bump `project(VERSION ...)` in the root `CMakeLists.txt` to the exact
   version you are about to tag. The release workflow refuses a tag that
   disagrees with it, because the in-app update check compares the
   running version against the latest tag — a mismatch would nag every
   user forever.

2. **Verify.** Full Windows build plus `ctest --preset windows`, then
   actually run `Clipster.exe`: record a session, save a clip, play it
   back. The unit tests only cover `core/`.

3. **Merge to `main`.** Open the PR, get CI green, merge it.

4. **Publish from `main`, never from the release branch.**

   ```powershell
   git switch main
   git pull
   .\scripts\release-local.ps1
   ```

   The script reads the version from `CMakeLists.txt`, closes a running
   Clipster (it locks its own exe), builds, runs the tests, packages the
   portable zip and the Inno Setup installer, and publishes the GitHub
   release. `gh release create` puts the tag on **the current commit** —
   which is why this step happens after the merge, on `main`. Tagging the
   release branch instead would leave the tag pointing at a commit that a
   squash-merge then discards.

5. **Check the release page has both assets** —
   `Clipster-vX.Y.Z-windows-x64.zip` *and* `Clipster-Setup-vX.Y.Z.exe`.
   The installer is skipped with only a warning when Inno Setup is
   missing (`winget install -e --id JRSoftware.InnoSetup`), and the CI
   precheck skips its own rebuild as soon as the release has *any*
   asset — so a zip-only release will not be repaired for you.

6. **Delete the release branch.**

Useful variants while iterating:

```powershell
.\scripts\release-local.ps1 -NoUpload    # build the assets, publish nothing
.\scripts\release-local.ps1 -SkipBuild   # repackage an existing build
```

### Versioning

Roughly semver: patch for fixes, minor for user-visible features, and
nothing is 1.0 until the feature set settles. Tags are `vX.Y.Z`.

### Hotfixes

Same flow with a `fix/` branch — merge to `main`, then cut a patch
release from `main`. There are no maintenance branches for older
versions; users update to the latest.
