package main

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/exec"
	"strings"
)

// ── config ────────────────────────────────────────────────────────────────────

const (
	sourceURL     = "https://github.com/carbon-os/ui"
	portName      = "ui"
	portDir       = "ports/ui"
	portfilePath  = "ports/ui/portfile.cmake"
	vcpkgJsonPath = "ports/ui/vcpkg.json"
	baselinePath  = "versions/baseline.json"
	uiVersionPath = "versions/u-/ui.json"
)

// ── templates ─────────────────────────────────────────────────────────────────

func portfileCmake(ref string) string {
	return fmt.Sprintf(`vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL             %s
    REF             %s
    HEAD_REF        main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUI_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    CONFIG_PATH lib/cmake/ui
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
`, sourceURL, ref)
}

func portVcpkgJson(version string) map[string]any {
	return map[string]any{
		"name":    portName,
		"version": version,
		"dependencies": []any{
			"vcpkg-cmake",
			"vcpkg-cmake-config",
			map[string]any{
				"name":     "webview2",
				"platform": "windows",
			},
		},
	}
}

func baselineJson(version string) map[string]any {
	return map[string]any{
		"default": map[string]any{
			portName: map[string]any{
				"baseline":     version,
				"port-version": 0,
			},
		},
	}
}

// ── helpers ───────────────────────────────────────────────────────────────────

func mustGit(args ...string) string {
	cmd := exec.Command("git", args...)
	cmd.Stderr = os.Stderr
	out, err := cmd.Output()
	if err != nil {
		log.Fatalf("git %s: %v", strings.Join(args, " "), err)
	}
	return strings.TrimSpace(string(out))
}

func writeText(path, content string) {
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		log.Fatalf("write %s: %v", path, err)
	}
}

func writeJSON(path string, v any) {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		log.Fatal(err)
	}
	if err := os.WriteFile(path, append(b, '\n'), 0o644); err != nil {
		log.Fatalf("write %s: %v", path, err)
	}
}

func readJSON(path string, v any) {
	b, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("read %s: %v", path, err)
	}
	if err := json.Unmarshal(b, v); err != nil {
		log.Fatalf("parse %s: %v", path, err)
	}
}

func bumpPatch(v string) string {
	parts := strings.Split(v, ".")
	if len(parts) != 3 {
		log.Fatalf("unexpected version %q", v)
	}
	var patch int
	fmt.Sscanf(parts[2], "%d", &patch)
	return fmt.Sprintf("%s.%s.%d", parts[0], parts[1], patch+1)
}

// ── main ──────────────────────────────────────────────────────────────────────

func main() {
	log.SetFlags(0)
	log.SetPrefix("vcpkg-update: ")

	// 1. latest commit on source repo
	fmt.Printf("→ resolving HEAD of %s\n", sourceURL)
	raw, err := exec.Command("git", "ls-remote", sourceURL, "HEAD").Output()
	if err != nil {
		log.Fatalf("ls-remote: %v", err)
	}
	fields := strings.Fields(string(raw))
	if len(fields) == 0 {
		log.Fatal("ls-remote returned nothing")
	}
	newRef := fields[0]
	fmt.Printf("  ref: %s\n", newRef)

	// 2. bump version from existing ui.json (source of truth)
	var uiVersions struct {
		Versions []struct {
			Version string `json:"version"`
		} `json:"versions"`
	}
	readJSON(uiVersionPath, &uiVersions)
	if len(uiVersions.Versions) == 0 {
		log.Fatal("ui.json has no versions")
	}
	currentVer := uiVersions.Versions[0].Version
	newVer := bumpPatch(currentVer)
	fmt.Printf("→ bumping version %s → %s\n", currentVer, newVer)

	// 3. write all port files from templates
	fmt.Println("→ writing port files")
	writeText(portfilePath, portfileCmake(newRef))
	writeJSON(vcpkgJsonPath, portVcpkgJson(newVer))
	writeJSON(baselinePath, baselineJson(newVer))

	// 4. temp commit to derive the port tree hash
	fmt.Println("→ staging (temp commit to derive tree hash)")
	mustGit("add", portfilePath, vcpkgJsonPath, baselinePath)
	mustGit("commit", "-m", "chore: vcpkg-update temp")
	treeHash := mustGit("rev-parse", "HEAD:"+portDir)
	fmt.Printf("  tree hash: %s\n", treeHash)

	// 5. prepend new entry to ui.json
	fmt.Println("→ updating ui.json")
	var uiRaw struct {
		Versions []map[string]any `json:"versions"`
	}
	readJSON(uiVersionPath, &uiRaw)
	uiRaw.Versions = append(
		[]map[string]any{{"version": newVer, "git-tree": treeHash}},
		uiRaw.Versions...,
	)
	writeJSON(uiVersionPath, uiRaw)

	// 6. amend commit to include ui.json
	fmt.Println("→ amending commit")
	mustGit("add", uiVersionPath)
	mustGit("commit", "--amend", "--no-edit", "-m",
		fmt.Sprintf("chore: bump %s → %s", portName, newVer))

	// 7. push
	fmt.Println("→ pushing")
	mustGit("push")

	registryBaseline := mustGit("rev-parse", "HEAD")

	// 8. print snippet
	snippet, _ := json.MarshalIndent(map[string]any{
		"registries": []map[string]any{{
			"kind":       "git",
			"repository": sourceURL,
			"baseline":   registryBaseline,
			"packages":   []string{portName},
		}},
	}, "", "  ")

	fmt.Printf("\n✓ done — paste into vcpkg-configuration.json:\n\n%s\n", snippet)
}