**Step 1 — Make your source change** (e.g. fix `cmake/ui-config.cmake.in`) in `carbon-os/ui`, commit and push:
```bash
git add cmake/ui-config.cmake.in
git commit -m "fix: add REQUIRED to find_dependency for webview2"
git push
```

**Step 2 — Get the REF** (this commit's hash, goes into `portfile.cmake`):
```bash
git rev-parse HEAD
# e.g. 441599f0b5c17c3dcd412a395019ffc851506d4a
```

**Step 3 — Update `portfile.cmake`** with that hash:
```cmake
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL             https://github.com/carbon-os/ui
    REF             441599f0b5c17c3dcd412a395019ffc851506d4a
    HEAD_REF        main
)
```

**Step 4 — Commit the portfile change** and push:
```bash
git add ports/ui/portfile.cmake
git commit -m "port: bump ui REF to 441599f"
git push
```

**Step 5 — Get the two hashes needed for the terminal repo:**
```bash
git rev-parse HEAD:ports/ui   # → git-tree, goes into versions/u-/ui.json
git rev-parse HEAD            # → baseline, goes into vcpkg-configuration.json
```

**Step 6 — Update `versions/u-/ui.json`** in the `carbon-os/ui` repo, adding a new entry at the top:
```json
{
  "versions": [
    {
      "git-tree": "<git rev-parse HEAD:ports/ui result>",
      "version": "0.1.0",
      "port-version": 1
    },
    ...
  ]
}
```

**Step 7 — Commit and push that versions file:**
```bash
git add versions/u-/ui.json
git commit -m "versions: bump ui"
git push
```

**Step 8 — Update `vcpkg-configuration.json`** in `carbon-terminal` with the hash from step 5:
```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/carbon-os/ui",
      "baseline": "<git rev-parse HEAD result from step 5>",
      "packages": ["ui"]
    }
  ]
}
```

**Step 9 — Rebuild:**
```bat
rmdir /s /q build
build.bat
```