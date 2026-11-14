package tool

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestNewProject(t *testing.T) {
	t.Parallel()
	parent := t.TempDir()
	directory := filepath.Join(parent, "hello-world")
	if err := Run(t.Context(), []string{"new", directory}, io.Discard, io.Discard); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{"CMakeLists.txt", "proto/greet/v1/greet.proto", "src/main.cc", ".gitignore"} {
		if _, err := os.Stat(filepath.Join(directory, path)); err != nil {
			t.Fatal(err)
		}
	}
	// Existing user work must survive a second invocation unchanged.
	sentinel := filepath.Join(directory, "src", "main.cc")
	if err := os.WriteFile(sentinel, []byte("user edits"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := Run(t.Context(), []string{"new", directory}, io.Discard, io.Discard)
	if !errors.Is(err, os.ErrExist) {
		t.Fatalf("second new error = %v, want ErrExist", err)
	}
	data, err := os.ReadFile(sentinel)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "user edits" {
		t.Fatalf("existing source overwritten: %q", data)
	}
}

func TestInvalidNames(t *testing.T) {
	t.Parallel()
	for _, name := range []string{"1project", "bad;name", "bad\nname", "-argument", "name space"} {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			directory := filepath.Join(t.TempDir(), name)
			if err := Run(t.Context(), []string{"new", directory}, io.Discard, io.Discard); err == nil {
				t.Fatal("accepted unsafe project name")
			}
			if _, err := os.Stat(directory); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("created invalid directory: %v", err)
			}
		})
	}
}

func TestCachePreservesPaths(t *testing.T) {
	t.Parallel()
	path := filepath.Join(t.TempDir(), "CMakeCache.txt")
	cache := "CMAKE_CXX_COMPILER:FILEPATH=C:/Program Files/compiler/bin/cl.exe\nCMAKE_PREFIX_PATH:STRING=/a path;/another\nCMAKE_TOOLCHAIN_FILE:FILEPATH=/tool chain/vcpkg.cmake\nVCPKG_INSTALLED_DIR:PATH=/deps\nOPENSSL_ROOT_DIR:PATH=OPENSSL_ROOT_DIR-NOTFOUND\nBUILD_TESTING:BOOL=ON\nIGNORED:STRING=secret\n"
	if err := os.WriteFile(path, []byte(cache), 0o644); err != nil {
		t.Fatal(err)
	}
	config, err := readCache(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"-DCMAKE_CXX_COMPILER=C:/Program Files/compiler/bin/cl.exe", "-DCMAKE_PREFIX_PATH=/a path;/another", "-DCMAKE_TOOLCHAIN_FILE=/tool chain/vcpkg.cmake", "-DVCPKG_INSTALLED_DIR=/deps"}
	if strings.Join(config.CMakeArgs, "\n") != strings.Join(want, "\n") {
		t.Fatalf("cache arguments = %q, want %q", config.CMakeArgs, want)
	}
}

func TestErrorsAndHelp(t *testing.T) {
	t.Parallel()
	for _, tt := range []struct {
		name      string
		args      []string
		wantError bool
	}{
		{name: "help", args: []string{"--help"}},
		{name: "setup help", args: []string{"setup", "--help"}},
		{name: "new help", args: []string{"new", "--help"}},
		{name: "unknown", args: []string{"typo"}, wantError: true},
		{name: "missing directory", args: []string{"new"}, wantError: true},
		{name: "missing SDK", args: []string{"build", "--prefix", t.TempDir()}, wantError: true},
		{name: "wrong source", args: []string{"setup", "--source", t.TempDir()}, wantError: true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			var output bytes.Buffer
			err := Run(t.Context(), tt.args, &output, &output)
			if (err != nil) != tt.wantError {
				t.Fatalf("Run(%q) = %v, wantError %v", tt.args, err, tt.wantError)
			}
		})
	}
}

func TestCommandFailure(t *testing.T) {
	t.Parallel()
	// CMake -E false works on every supported platform; errors must propagate.
	if err := run(t.Context(), io.Discard, io.Discard, "cmake", "--version"); err != nil {
		t.Fatal(err)
	}
	if err := run(t.Context(), io.Discard, io.Discard, "cmake", "-E", "false"); err == nil {
		t.Fatal("discarded child command failure")
	}
}
