# Branching Strategy for your RetroArch fork

## Core Concepts

Before reading further, understand these three things:

### What is a rebase?
A rebase takes your commits and **replays them on top of a different base commit**,
keeping history linear. Instead of a merge commit, your work simply moves forward:
```
Before:  [RA v1.22.2] ── [your feature A] ── [your feature B]
After:   [RA v1.23.0] ── [your feature A'] ── [your feature B']
```
Your features travel forward automatically. Requires `git push --force-with-lease`
because history is rewritten.

### What is a merge?
A merge joins two branches with a **merge commit**, preserving both histories.
Simpler than rebase, no force-push needed, but history accumulates merge commits
over time and becomes harder to read.

### Why rebase for this fork?
Because you are the sole developer, the force-push risk is zero. Rebase keeps
`main` perfectly readable: upstream commits first, then your commits on top —
making it immediately obvious what you've added versus upstream.

---

## The Four Concerns

| Concern | Lives where |
|---|---|
| Upstream RA source snapshots (read-only) | `retroarch-releases/<tag>` |
| Early breakage detection | `octelys/upstream-sync` (robot-owned, never touch directly) |
| Your custom features | `feature/<name>` branches → PR → `main` |
| Your fork releases | `main` → tagged |

---

## Branch Map

```
libretro/RetroArch
    │
    ├── master ──────────────────────────────────────► octelys/upstream-sync
    │              daily, via upstream-sync.yml         Robot-owned canary branch.
    │                                                   Contains: upstream master
    │                                                   + your feature commits on top.
    │                                                   CI runs here.
    │                                                   NEVER develop or commit here directly.
    │                                                   NEVER merge this PR into main.
    │                                                   Force-overwritten every day.
    │
    ├── tag v1.22.2 ──► retroarch-releases/v1.22.2     Read-only upstream snapshot.
    │                   via release-branch-sync.yml     Used ONLY as a rebase target
    │                                                   for main. Never commit here.
    │
    └── tag v1.23.0 ──► retroarch-releases/v1.23.0     (same)
                                    │
                                    │  YOU manually rebase main onto this
                                    │  when ready to absorb the new release
                                    ▼
             main: [v1.23.0 code] ── [feature A'] ── [feature B']
                  │                                                │
                  │  you branch off here                     git tag v1
                  │                                                │
                  ├── feature/ws-server       ──► PR ──► main     ▼
                  ├── feature/achievement-events ► PR ──► main  GitHub Release
                  └── feature/...                                "RetroArch 1.23.0.1"
```

---

## The two automated pipelines

### Pipeline 1 — `upstream-sync.yml` (daily canary)

**Trigger:** every day at 06:00 UTC, or manually.

**What it does:**
1. Fetches the latest upstream `master`
2. Creates branch `octelys/upstream-sync` = upstream master + your feature commits from `main` cherry-picked on top
3. Force-pushes it (overwrites yesterday's canary)
4. Opens (or updates) a PR so CI runs against it

**What you do with it:**
- ✅ CI green → your features are compatible with today's upstream. Close the PR.
- ❌ CI red → upstream broke something. Fix your `feature/*` branch, merge it into `main`. The next daily run will reopen a fresh canary.
- 🚫 Never merge this PR into `main`.
- 🚫 Never commit to `octelys/upstream-sync` directly.

---

### Pipeline 2 — `release-branch-sync.yml` (release tracker)

**Trigger:** every day at 07:00 UTC, or manually (you can supply a specific tag).

**What it does:**
1. Fetches the latest upstream release tag (e.g. `v1.23.0`)
2. Checks whether `retroarch-releases/v1.23.0` already exists in your fork
3. If not, creates and pushes it at the exact release commit SHA

**What you do with it:**
- When you see a new `retroarch-releases/<tag>` branch appear, that is your signal
  that a new upstream release has shipped and you can absorb it into `main`.
- 🚫 Never commit to these branches.

---

## Step-by-step: absorbing a new upstream release into main

> Do this after a new `retroarch-releases/<tag>` branch has appeared AND the
> daily canary CI is green (your features are confirmed compatible).

```zsh
# 1. Pull the new release branch from your fork
git fetch origin retroarch-releases/v1.23.0

# 2. Switch to main
git checkout main

# 3. Rebase your feature commits on top of the new upstream release
git rebase origin/retroarch-releases/v1.23.0

# 4. If conflicts appear during rebase:
#    For .github/workflows — always keep YOUR version:
#      git checkout --ours .github/workflows/
#      git add .github/workflows/
#      git rebase --continue
#    For source files — resolve manually, then:
#      git add <resolved-file>
#      git rebase --continue

# 5. Push (force required because rebase rewrites commit SHAs)
git push origin main --force-with-lease
```

After this, `main` = `[RA v1.23.0 code] + [your features]`. ✅

---

## Step-by-step: publishing a fork release

> Do this after main has been rebased onto the desired upstream release.

```zsh
# Your tag is a version number prefixed with v.
# Build.yml combines it with the upstream RA version: <ra>.<build_number>
git tag v1
git push origin v1
# → Build.yml fires → GitHub Release created: "RetroArch 1.23.0.1"
```

To publish a second release on the same upstream version:
```zsh
git tag v2
git push origin v2
# → GitHub Release: "RetroArch 1.23.0.2"
```

---

## Step-by-step: developing a new feature

```zsh
# 1. Branch off main (which is already on top of the latest upstream release)
git checkout -b feature/my-new-thing main

# 2. Work, commit as usual
git add .
git commit -m "feat: my new thing"

# 3. Push and open a PR into main
git push origin feature/my-new-thing
# → Open PR on GitHub: feature/my-new-thing → main
```

---

## What each branch is — quick reference

| Branch | Owner | Purpose | Can I commit here? |
|---|---|---|---|
| `main` | You | Latest stable RA release + your features. Release base. | ✅ Yes (via PRs) |
| `feature/<name>` | You | Work-in-progress feature | ✅ Yes |
| `retroarch-releases/<tag>` | Robot | Upstream release snapshot. Rebase target only. | 🚫 No |
| `octelys/upstream-sync` | Robot | Daily canary. CI alarm bell. | 🚫 No |

---

## What each pipeline does — quick reference

| Workflow | Trigger | Role | Touches main? |
|---|---|---|---|
| `upstream-sync.yml` | Daily 06:00 UTC | Canary: upstream master + your features → CI | 🚫 No |
| `release-branch-sync.yml` | Daily 07:00 UTC | Creates `retroarch-releases/<tag>` on new upstream release | 🚫 No |
| `Build.yml` | Your tag push | Builds and publishes fork release. Detects RA version from git history, names release `<upstream-ver>-<your-tag>`. | 🚫 No |
| `MacOS.yml` / `Windows-x64-MXE.yml` / etc. | Called by `Build.yml` or PRs | CI builds | 🚫 No |

---

## Summary

> `retroarch-releases/<tag>` = frozen upstream snapshots — rebase targets, nothing else
>
> `octelys/upstream-sync` = daily robot canary — read CI result, never commit
>
> `main` = **your branch** — latest stable RA release + your features — develop and release from here
>
> `feature/<name>` = short-lived work branches — PR into `main` when done
>
> tags on `main` = your published releases

