package tool

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

type sdk struct {
	Format        int      `json:"format"`
	CMakeArgs     []string `json:"cmake_args"`
	Configuration string   `json:"configuration"`
}

type arguments []string

func (a *arguments) String() string         { return strings.Join(*a, " ") }
func (a *arguments) Set(value string) error { *a = append(*a, value); return nil }

func defaultPrefix() (string, error) {
	if root := os.Getenv("CONNECTCPP_ROOT"); root != "" {
		return filepath.Abs(root)
	}
	executable, err := os.Executable()
	if err == nil {
		if resolved, resolveErr := filepath.EvalSymlinks(executable); resolveErr == nil {
			executable = resolved
		}
		prefix := filepath.Dir(filepath.Dir(executable))
		if _, err := os.Stat(metadataPath(prefix)); err == nil {
			return prefix, nil
		}
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".local"), nil
}
func metadataPath(prefix string) string {
	return filepath.Join(prefix, "share", "connectcpp", "sdk.json")
}

func run(ctx context.Context, out, errOut io.Writer, name string, args ...string) error {
	fmt.Fprintf(out, "+ %s %q\n", name, args)
	command := exec.CommandContext(ctx, name, args...)
	command.Stdout, command.Stderr = out, errOut
	if err := command.Run(); err != nil {
		return fmt.Errorf("%s: %w", name, err)
	}
	return nil
}

func setup(ctx context.Context, args []string, out, errOut io.Writer) error {
	prefix, err := defaultPrefix()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("setup", flag.ContinueOnError)
	flags.SetOutput(errOut)
	source := flags.String("source", ".", "Connect C++ source checkout")
	flags.StringVar(&prefix, "prefix", prefix, "SDK installation prefix")
	buildDir := flags.String("build-dir", "", "CMake build directory (default SOURCE/build/sdk)")
	configuration := flags.String("config", "Release", "SDK build configuration")
	var extra arguments
	flags.Var(&extra, "cmake-arg", "additional CMake configure argument; repeat for multiple arguments")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("setup accepts flags only")
	}
	prefix, err = filepath.Abs(prefix)
	if err != nil {
		return err
	}
	*source, err = filepath.Abs(*source)
	if err != nil {
		return err
	}
	if _, err := os.Stat(filepath.Join(*source, "include", "connectrpc", "rpc.h")); err != nil {
		return fmt.Errorf("--source must be a Connect C++ checkout: %w", err)
	}
	if *buildDir == "" {
		*buildDir = filepath.Join(*source, "build", "sdk")
	}
	*buildDir, err = filepath.Abs(*buildDir)
	if err != nil {
		return err
	}
	configure := []string{"-S", *source, "-B", *buildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + *configuration, "-DBUILD_TESTING=OFF", "-DCONNECTCPP_BUILD_GENERATOR=ON", "-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON", "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_PREFIX=" + filepath.ToSlash(prefix)}
	if paths := os.Getenv("CMAKE_PREFIX_PATH"); paths != "" {
		configure = append(configure, "-DCMAKE_PREFIX_PATH="+strings.Join(filepath.SplitList(paths), ";"))
	}
	// Homebrew installs keg-only OpenSSL/nghttp2 outside the default search path.
	if runtime.GOOS == "darwin" && os.Getenv("CMAKE_PREFIX_PATH") == "" {
		brew := exec.CommandContext(ctx, "brew", "--prefix")
		if result, err := brew.Output(); err == nil {
			root := strings.TrimSpace(string(result))
			configure = append(configure, "-DCMAKE_PREFIX_PATH="+root+";"+filepath.Join(root, "opt", "openssl@3")+";"+filepath.Join(root, "opt", "libnghttp2"))
		}
	}
	configure = append(configure, extra...)
	if err := run(ctx, out, errOut, "cmake", configure...); err != nil {
		return err
	}
	if err := run(ctx, out, errOut, "cmake", "--build", *buildDir, "--config", *configuration, "--parallel", "3"); err != nil {
		return err
	}
	if err := run(ctx, out, errOut, "cmake", "--install", *buildDir, "--config", *configuration, "--prefix", prefix); err != nil {
		return err
	}
	config, err := readCache(filepath.Join(*buildDir, "CMakeCache.txt"))
	if err != nil {
		return err
	}
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return err
	}
	path := metadataPath(prefix)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	// Publish metadata only after the install succeeds.
	if err := os.WriteFile(path, append(data, '\n'), 0o644); err != nil {
		return err
	}
	_, err = fmt.Fprintf(out, "SDK installed in %s\nAdd %s to PATH. Then: connectcpp new hello; cd hello; connectcpp build\n", prefix, filepath.Join(prefix, "bin"))
	return err
}

func readCache(path string) (sdk, error) {
	config := sdk{Format: 1, Configuration: "Release"}
	file, err := os.Open(path)
	if err != nil {
		return config, err
	}
	defer file.Close()
	keys := map[string]bool{
		"CMAKE_TOOLCHAIN_FILE": true, "CMAKE_PREFIX_PATH": true,
		"CMAKE_CXX_COMPILER": true, "CMAKE_MSVC_RUNTIME_LIBRARY": true,
		"VCPKG_INSTALLED_DIR": true, "VCPKG_TARGET_TRIPLET": true, "VCPKG_HOST_TRIPLET": true,
		"VCPKG_OVERLAY_TRIPLETS": true,
		"ASIO_INCLUDE_DIR":       true, "Protobuf_DIR": true, "absl_DIR": true, "llhttp_DIR": true,
		"OPENSSL_ROOT_DIR": true, "PKG_CONFIG_EXECUTABLE": true,
	}
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		keyAndType, value, ok := strings.Cut(scanner.Text(), "=")
		key, _, typed := strings.Cut(keyAndType, ":")
		if ok && typed && key == "CMAKE_BUILD_TYPE" && value != "" {
			config.Configuration = value
		}
		if ok && typed && keys[key] && value != "" && !strings.HasSuffix(value, "-NOTFOUND") {
			config.CMakeArgs = append(config.CMakeArgs, "-D"+key+"="+value)
		}
	}
	if err := scanner.Err(); err != nil {
		return config, err
	}
	return config, nil
}

func loadSDK(prefix string) (sdk, error) {
	var config sdk
	data, err := os.ReadFile(metadataPath(prefix))
	if err != nil {
		return config, fmt.Errorf("read SDK in %s; run connectcpp setup first or use --prefix: %w", prefix, err)
	}
	if err := json.Unmarshal(data, &config); err != nil {
		return config, fmt.Errorf("parse SDK metadata: %w", err)
	}
	if config.Format != 1 {
		return config, fmt.Errorf("unsupported SDK metadata format %d", config.Format)
	}
	return config, nil
}

func build(ctx context.Context, args []string, out, errOut io.Writer) error {
	prefix, err := defaultPrefix()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("build", flag.ContinueOnError)
	flags.SetOutput(errOut)
	flags.StringVar(&prefix, "prefix", prefix, "SDK installation prefix")
	configuration := flags.String("config", "", "CMake build configuration (default SDK configuration)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() > 1 {
		return errors.New("usage: connectcpp build [--prefix PATH] [DIRECTORY]")
	}
	prefix, err = filepath.Abs(prefix)
	if err != nil {
		return err
	}
	config, err := loadSDK(prefix)
	if err != nil {
		return err
	}
	if *configuration == "" {
		*configuration = config.Configuration
		if *configuration == "" {
			*configuration = "Release"
		}
	}
	source := "."
	if flags.NArg() == 1 {
		source = flags.Arg(0)
	}
	source, err = filepath.Abs(source)
	if err != nil {
		return err
	}
	buildDir := filepath.Join(source, "build")
	configure := append([]string{"-S", source, "-B", buildDir, "-G", "Ninja"}, config.CMakeArgs...)
	// A consumer reuses installed vcpkg dependencies rather than creating a manifest.
	for _, argument := range config.CMakeArgs {
		if strings.HasPrefix(argument, "-DVCPKG_INSTALLED_DIR=") {
			configure = append(configure, "-DVCPKG_MANIFEST_MODE=OFF")
			break
		}
	}
	configure = append(configure, "-Dconnectcpp_DIR="+filepath.ToSlash(filepath.Join(prefix, "lib", "cmake", "connectcpp")), "-DCONNECTCPP_PROTOC_GEN="+filepath.ToSlash(filepath.Join(prefix, "bin", "protoc-gen-connect-cpp"+exeSuffix())), "-DCMAKE_BUILD_TYPE="+*configuration)
	if err := run(ctx, out, errOut, "cmake", configure...); err != nil {
		return err
	}
	if err := run(ctx, out, errOut, "cmake", "--build", buildDir, "--config", *configuration, "--parallel", "3"); err != nil {
		return err
	}
	return run(ctx, out, errOut, "ctest", "--test-dir", buildDir, "-C", *configuration, "--output-on-failure")
}
func exeSuffix() string {
	if runtime.GOOS == "windows" {
		return ".exe"
	}
	return ""
}

func doctor(args []string, out, errOut io.Writer) error {
	prefix, err := defaultPrefix()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("doctor", flag.ContinueOnError)
	flags.SetOutput(errOut)
	flags.StringVar(&prefix, "prefix", prefix, "SDK installation prefix")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("doctor accepts flags only")
	}
	if _, err := loadSDK(prefix); err != nil {
		return err
	}
	for _, name := range []string{"cmake", "ctest", "ninja"} {
		path, err := exec.LookPath(name)
		if err != nil {
			return fmt.Errorf("required tool %s: %w", name, err)
		}
		fmt.Fprintf(out, "%s: %s\n", name, path)
	}
	for _, path := range []string{"include/connectrpc/rpc.h", "lib/cmake/connectcpp/connectcppConfig.cmake", "bin/protoc-gen-connect-cpp" + exeSuffix()} {
		if _, err := os.Stat(filepath.Join(prefix, filepath.FromSlash(path))); err != nil {
			return fmt.Errorf("incomplete SDK: %w", err)
		}
	}
	_, err = fmt.Fprintf(out, "SDK: %s\nFiles and tools are present. Run connectcpp build to verify compilation and dependencies.\n", prefix)
	return err
}
