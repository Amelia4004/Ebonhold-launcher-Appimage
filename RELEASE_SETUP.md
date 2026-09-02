# Automated Releases

This repository uses `semantic-release` to create GitHub Releases automatically from commits merged into `main`.

## Version rules

Use Conventional Commit prefixes for commits or, when using squash merge, for pull request titles:

- `fix: ...` -> patch release (`0.9.0` -> `0.9.1`)
- `feat: ...` -> minor release (`0.9.0` -> `0.10.0`)
- `perf: ...` -> patch release
- `feat!: ...` or a `BREAKING CHANGE:` footer -> major release (`0.9.0` -> `1.0.0`)
- `docs:`, `chore:`, `refactor:`, `style:`, `test:` -> no release by default

Examples:

```text
fix: handle interrupted addon downloads
feat: add official addon manager
feat!: replace legacy launcher configuration format
```

## First setup

If version `0.9.0` has already been published manually, make sure GitHub has a `v0.9.0` tag before the first automated run. Otherwise semantic-release can treat the repository as having no previous release and start at `1.0.0`.

From a local checkout at the exact commit corresponding to 0.9.0:

```bash
git tag v0.9.0
git push origin v0.9.0
```

Do this only if the tag does not already exist.

## Protected `main` branch

`semantic-release` must be able to create/push its version tag. GitHub's automatic `GITHUB_TOKEN` can be blocked by branch protection/rulesets.

If the workflow fails with `EGITNOPERMISSION`:

1. Create a fine-grained Personal Access Token for only this repository.
2. Give it **Contents: Read and write** permission.
3. Add it in `Settings -> Secrets and variables -> Actions` as `SEMANTIC_RELEASE_TOKEN`.
4. If your `main` ruleset blocks the repository owner/admin too, add the appropriate administrator/user to the ruleset bypass list.

The workflow only runs on pushes to `main` or a manual `workflow_dispatch`; pull requests from forks do not receive repository secrets. Still, treat the PAT as a high-value credential and keep its permissions/repository scope minimal.

## What the workflow does

When a release-worthy commit lands on `main`, GitHub Actions:

1. Calculates the next semantic version.
2. Builds the Qt launcher on Ubuntu.
3. Passes the calculated version into CMake using `-DEBONHOLD_VERSION=...`.
4. Builds the AppImage.
5. Creates a SHA-256 checksum.
6. Creates a Git tag such as `v0.10.0`.
7. Creates the GitHub Release and generates release notes.
8. Uploads the AppImage and checksum as release assets.

A manual workflow run does not force a release. If there are no release-worthy commits since the last tag, semantic-release exits without publishing a new version.
