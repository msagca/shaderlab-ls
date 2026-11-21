---
name: release
description: Release a new version of shaderlab-ls - bump the version in CMakeLists.txt, land it on main, and tag it so release.yml builds and publishes the binaries. Use when asked to release, cut a release, bump the version or tag a version.
---

# Releasing

The version is written in one place: `project(shaderlab_ls VERSION X.Y.Z ...)` in `CMakeLists.txt`. `release.yml`
refuses a tag that doesn't match it, and the binary reports it with `--version`.

1. Ask which part to bump (patch, minor or major) unless the user said. Read the commits since the last tag with
   `git log $(git describe --tags --abbrev=0)..main --oneline` to suggest one.
2. Change only the `VERSION` in the `project()` line, and commit it with the subject `Release X.Y.Z`.
3. Run `cmake --workflow Release`, then check that `build/shaderlab-ls --version` prints the new version.
4. Land the commit on `main`. Ask before pushing.
5. Ask before tagging, then tag `main` and push the tag:
   - `git tag vX.Y.Z origin/main`
   - `git push origin vX.Y.Z`

   Pushing the tag starts `release.yml`. It builds on Linux and Windows, tests the packaged archives, and publishes
   them with `SHA256SUMS` on the release `vX.Y.Z`. To release without pushing a tag, run the `tag` workflow on
   `main` instead: `gh workflow run tag.yml --ref main`.
6. Follow the run with `gh run watch`, and report the release URL when it finishes.
