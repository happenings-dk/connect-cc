// Package tool implements the Connect C++ SDK and project commands.
package tool

import (
	"context"
	"embed"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"runtime/debug"
	"strings"
)

//go:embed all:starter skill
var _assets embed.FS

// Run executes a command and returns an error without exiting the process.
func Run(ctx context.Context, args []string, out, errOut io.Writer) error {
	if len(args) == 0 {
		args = []string{"help"}
	}
	var err error
	switch args[0] {
	case "help", "--help", "-h":
		_, err = fmt.Fprintln(out, `Connect C++ SDK

  connectcpp setup --source PATH [--prefix PATH] [--build-dir PATH]
                   [--cmake-arg ARG ...]
  connectcpp new DIRECTORY
  connectcpp build [--prefix PATH] [--config Release] [DIRECTORY]
  connectcpp doctor [--prefix PATH]
  connectcpp skill install [--dir PATH]
  connectcpp version

Run a command with --help for its options. Add PREFIX/bin to PATH after setup.
Prerequisites: C++20 compiler, CMake, Ninja, pkg-config and the C++ dependencies.
See docs/INSTALLATION.md for native and pinned vcpkg setup.`)
	case "version":
		version := "devel"
		if info, ok := debug.ReadBuildInfo(); ok && info.Main.Version != "" {
			version = info.Main.Version
		}
		_, err = fmt.Fprintln(out, "connectcpp", version)
	case "new":
		err = newProject(args[1:], out, errOut)
	case "setup":
		err = setup(ctx, args[1:], out, errOut)
	case "build":
		err = build(ctx, args[1:], out, errOut)
	case "doctor":
		err = doctor(args[1:], out, errOut)
	case "skill":
		err = installSkill(args[1:], out, errOut)
	default:
		err = fmt.Errorf("unknown command %q; run connectcpp --help", args[0])
	}
	if errors.Is(err, flag.ErrHelp) {
		return nil
	}
	return err
}

func newProject(args []string, out, errOut io.Writer) error {
	flags := flag.NewFlagSet("new", flag.ContinueOnError)
	flags.SetOutput(errOut)
	flags.Usage = func() { fmt.Fprintln(errOut, "Usage: connectcpp new DIRECTORY (must not exist)") }
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 1 {
		return errors.New("usage: connectcpp new DIRECTORY")
	}
	directory := filepath.Clean(flags.Arg(0))
	name := filepath.Base(directory)
	if !regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]*$`).MatchString(name) {
		return errors.New("project directory name must start with a letter and contain only letters, digits, underscores, or hyphens")
	}
	if err := copyAssets("starter", directory, name); err != nil {
		return err
	}
	_, err := fmt.Fprintf(out, "Created %s\nNext: cd %s, then connectcpp build\n", directory, directory)
	return err
}

// copyAssets requires an absent destination; it never merges into user files.
func copyAssets(source, destination, project string) error {
	if err := os.Mkdir(destination, 0o755); err != nil {
		return fmt.Errorf("create %s (must not exist): %w", destination, err)
	}
	return fs.WalkDir(_assets, source, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == source {
			return nil
		}
		target := filepath.Join(destination, filepath.FromSlash(strings.TrimPrefix(path, source+"/")))
		if entry.IsDir() {
			return os.Mkdir(target, 0o755)
		}
		data, err := _assets.ReadFile(path)
		if err != nil {
			return fmt.Errorf("read embedded %s: %w", path, err)
		}
		data = []byte(strings.ReplaceAll(string(data), "@PROJECT_NAME@", project))
		// Embedded files are fixed paths, and destination was created exclusively.
		file, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
		if err != nil {
			return fmt.Errorf("create %s: %w", target, err)
		}
		_, writeErr := file.Write(data)
		return errors.Join(writeErr, file.Close())
	})
}

func installSkill(args []string, out, errOut io.Writer) error {
	if len(args) == 0 || args[0] != "install" {
		return errors.New("usage: connectcpp skill install [--dir PATH]")
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("skill install", flag.ContinueOnError)
	flags.SetOutput(errOut)
	dir := flags.String("dir", filepath.Join(home, ".agents", "skills", "connect-cpp"), "skill directory (must not exist)")
	if err := flags.Parse(args[1:]); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("skill install accepts flags only")
	}
	if err := os.MkdirAll(filepath.Dir(*dir), 0o755); err != nil {
		return err
	}
	if err := copyAssets("skill", *dir, ""); err != nil {
		return err
	}
	_, err = fmt.Fprintf(out, "Installed Connect C++ skill: %s\nLink this directory into your agent's skill directory if it does not discover ~/.agents/skills.\n", *dir)
	return err
}
