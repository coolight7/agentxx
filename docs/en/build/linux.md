# Linux Executable / Dynamic Library Build Guide

- OS Environment: Linux
- C++ Standard: Requires C++26+.
- Recommended Compiler: Linux / GCC 16.1. Previously when compiling with GCC 13.2, certain coroutine functions would cause internal compiler errors (ICE).

---
- There are two ways to start compilation:
  - [Automated Build Script](#automated-build-script): Handles most operations and dependencies automatically.
  - [Manual Compilation](#manual-compilation): Manually control versions, flags, and build steps for Boost, OpenSSL, and dependencies.
---

## Automated Build Script

- Simply execute the build script (`agent/script/linux_debug_build.sh` / `linux_release_build.sh`).
- The script automatically prepares the build environment and dependencies without manual `apt` packages, handling:
>
> 1. **Prerequisite Environment Checks**: Checks for `cmake` / `ninja` / `make` / `curl|wget` / `tar` / `python3` and a C++26 compiler (`g++>=14` or `clang++>=18`), exiting with installation instructions if missing.
> 2. **Automatic Dependency Building** (compiles all libraries from source, avoiding system or apt packages):
>    `Boost 1.92` (debug and release variants) and `OpenSSL 4.0.1` are automatically compiled from source to `agent/third_party/boost-linux-build-{debug,release}/` and `agent/third_party/OpenSSL-linux-build/`; existing artifacts are automatically reused.
>    `ragel` (Hyperscan code generator) prioritizes system-installed versions, falling back to downloading source and building locally if absent.
>
- Related Environment Variables (Optional):

| Variable | Description |
| --- | --- |
| `AGENTXX_SKIP_AUTO_DEPS=1` | Skips automatic dependency building (fails immediately if dependency directories are missing) |
| `AGENTXX_DEPS_FORCE=1` | Bypasses artifact reuse, forcing a complete rebuild of dependencies (except ragel, which reuses system version) |
| `BOOST_ROOT` / `OPENSSL_ROOT_DIR` | Manually specifies paths to existing installed libraries, taking precedence over automatic building |
| `AGENTXX_ENABLE_HYPERSCAN=OFF` | Disables Hyperscan; ragel is no longer required |
| `AGENTXX_BUILD_PARALLEL=N` | Number of parallel compilation jobs (debug defaults to 4; release defaults to CPU core count) |

## Manual Compilation

- Prepare build prerequisites using `apt` or equivalent package manager: `cmake`, `ninja-build`, `make`, `curl|wget`, `tar`, `python3`, and optionally `ccache` / `patchelf` / `ragel` (required by Hyperscan):
```sh
sudo apt-get install -y cmake ninja-build patchelf ccache zip ragel \
    libtool autoconf automake make bzip2 xz-utils unzip curl wget
```
- `g++>=14` (or `clang++>=18`), GCC 16.1 is recommended.

### Compiling Boost 1.92

- Installation can be done directly via your system package manager, but pay attention to the version; matching our development version `1.92` is recommended.
- Compiling manually:
```sh
# https://github.com/boostorg/boost/releases/
# Download release/boost-xxx-cmake.tar.gz and extract to agent/third_party/boost/
cd boost/
./bootstrap.sh

# Create third_party/boost-linux-build-debug and third_party/boost-linux-build-release directories
boost_source_dir=$PWD

boost_install_debug_dir="${boost_source_dir}/../boost-linux-build-debug/"
mkdir -p "$boost_install_debug_dir"
boost_install_debug_dir=$(cd "$boost_install_debug_dir" && pwd)

boost_install_release_dir="${boost_source_dir}/../boost-linux-build-release/"
mkdir -p "$boost_install_release_dir"
boost_install_release_dir=$(cd "$boost_install_release_dir" && pwd)

cd "$boost_source_dir"

export CXXFLAGS="-fPIC"
export CFLAGS="-fPIC"

./b2 install --layout=system --prefix="${boost_install_debug_dir}" cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared runtime-debugging=on address-model=64 variant=debug 

./b2 install --layout=system --prefix="${boost_install_release_dir}" cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

# If you modified certain parameters and want to rebuild, you can clean first:
# ./b2 --clean-all
# rm -rf bin.v2
```

### Compiling OpenSSL from Source
- Compilation:
```sh
cd {PROJECT_ROOT}/agent/third_party/
wget https://github.com/openssl/openssl/releases/download/openssl-4.0.1/openssl-4.0.1.tar.gz
tar -xzvf openssl-4.0.1.tar.gz
cd openssl-4.0.1

openssl_source_dir=$PWD
openssl_build_dir="$openssl_source_dir/../OpenSSL-linux-build"
mkdir -p "$openssl_build_dir"
openssl_build_dir=$(cd "$openssl_build_dir" && pwd)

cd "$openssl_source_dir"

./Configure no-shared --pic --prefix="$openssl_build_dir" --openssldir="$openssl_build_dir" '-Wl,-rpath,$(LIBRPATH)'
make
make install
```

### Compiling agentxx
- Launch agentxx compilation. Other dependencies will be downloaded automatically. After successful compilation, the command-line client will run automatically:
```sh
cd {PROJECT_ROOT}
./agent/script/linux_debug_build.sh
./agent/build/linux-debug/exec/agentxx_cli
```
- For release compilation, run:
```sh
cd {PROJECT_ROOT}
./agent/script/linux_release_build.sh
./agent/build/linux-release/exec/agentxx_cli
```

## Artifact Layout

- Executables: `agent/build/{platform}-{mode}/exec/agentxx_cli` / `agentxx_test` / `agentxx_benchmark`
- Plugin Shared Libraries (Standalone Dynamic Library Mode): `agent/build/{platform}-{mode}/exec/plugins/<plugin_name>/` (dispatched by directory when containing a `plugin.yaml` manifest)
- Shared Library (FFI): `agent/build/{platform}-{mode}/lib/libagentxx_shared.so` (Exports C symbols; see `agent/lib/ffi_symbols.map`)

## Debug Build Acceleration

The Debug build script (`script/linux_debug_build.sh`) has the following speedup techniques enabled by default:

| Technique | Description |
| --- | --- |
| ccache | Caches compilation results, significantly accelerating incremental, repeated, or cross-branch builds. Automatically skipped if not installed (`apt-get install ccache`). Cache directory defaults to `~/.cache/ccache-agentxx`, customizable via `CCACHE_DIR` / `CCACHE_MAXSIZE` (default 3G). |
| Fast Linker | Automatically detects and uses mold (>25x faster than default bfd, especially noticeable on large binaries), followed by gold; falls back to default if neither is present. Can be forced with `-DAGENTXX_LINKER=mold\|gold\|bfd`. |
| PCH | Precompiles stable third-party headers (std/fmt/asio/boost.exception, etc.). The project's own headers are not included, so modifying project headers won't trigger a full rebuild. Can be disabled with `-DAGENTXX_ENABLE_PCH=OFF`. |
| Parallelism | Defaults to the number of CPU cores (previously 4). Can be overridden using the environment variable `AGENTXX_BUILD_PARALLEL`, for example when low on RAM or avoiding compiler ICE: `AGENTXX_BUILD_PARALLEL=4 ./script/linux_debug_build.sh`. |
| Single-pass Source Compilation | Under GCC/Clang, the 62 source files in lib are compiled only once via an OBJECT library; both dynamic and static libraries share the same set of `.o` files (previously compiled twice), cutting compile time nearly in half. |

### Faster Optional Configurations

The default Debug build preserves AddressSanitizer and debug symbols (`-g`). If your daily iteration does not require ASAN or breakpoint symbols, you can significantly accelerate compilation and linking via CMake options (artifacts shrink to ~1/3 size):

```sh
cmake -B build/linux-debug -S agent \
    -DAGENTXX_ENABLE_ASAN=OFF \        # Disable AddressSanitizer
    ...other arguments match linux_debug_build.sh
```

- `AGENTXX_ENABLE_ASAN=OFF`: Removes `-fsanitize=address`, speeding up both compilation and linking significantly (retains `-fno-omit-frame-pointer` for debugging/profiling).
- Recommendation: Keep OFF for daily development, and switch back to ON for full verification before commits or when diagnosing tricky issues.

## Common Issues
- [FAQ for more issues](FAQ.md)

### Network-related code hangs or is very slow on Linux?
- This could be an asio version issue. In certain Boost versions (such as 1.91), asio may have bugs when io_uring is enabled. You can try switching to a different Boost version.
