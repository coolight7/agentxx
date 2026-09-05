# Cross-compiling Android Shared Libraries on Linux

- OS Environment: Linux
- C++ Standard: Requires C++26+.
- Recommended Compiler: Linux / NDK r29 / Clang 21.0.0

---
- There are two ways to start compilation:
  - [Automated Build Script](#automated-build-script): Handles dependencies and compilation automatically.
  - [Manual Compilation](#manual-compilation): Manually control versions, flags, and build steps for Boost, OpenSSL, and dependencies.
---

## Automated Build Script

- Simply execute the build script (`agent/script/cross_android_release_build.sh`).
- The build script automatically prepares the build environment and dependencies without manual `apt` packages, handling:
>
> 1. **Prerequisite Environment Checks**: Checks for `cmake`, `ninja`, `make`, `git`, and `ANDROID_NDK_ROOT` (must contain `build/cmake/android.toolchain.cmake`), terminating with clear error messages if any are missing.
> 2. **Automatic Dependency Cross-Compilation** (compiles all libraries from source, avoiding system or host apt packages):
>    Cross-compiles `Boost 1.92` via `third_party/boost-android` (automatically clones if missing) to `third_party/boost-android-build-release/<abi>/`;
>    Cross-compiles `OpenSSL 4.0.1` using NDK Clang to `third_party/OpenSSL-android-build/<abi>/`;
>    Automatically reuses existing build artifacts (Android defaults to `hyperscan OFF`, so ragel is not required).
>
- Related Environment Variables (Optional):

| Variable | Description |
| --- | --- |
| `ANDROID_NDK_ROOT` | **Required** (unless `BOOST_ROOT` + `OPENSSL_ROOT_DIR` are explicitly specified), points to NDK root directory |
| `AGENTXX_SKIP_AUTO_DEPS=1` | Skips automatic dependency building (fails immediately if dependency directories are missing) |
| `AGENTXX_DEPS_FORCE=1` | Bypasses artifact reuse, forcing a complete rebuild of dependencies |
| `BOOST_ROOT` / `OPENSSL_ROOT_DIR` | Manually specifies paths to existing pre-cross-compiled artifacts (takes precedence over auto-build) |
| `AGENTXX_BUILD_PARALLEL=N` | Number of parallel compilation jobs (default: 4) |

## Manual Compilation

- Prepare the build environment by installing `cmake`, `ninja-build`, `make`, and `git`, and set `ANDROID_NDK_ROOT` to your Android NDK directory.

### Compiling Boost
- Manually compile Boost 1.92:
```sh
# https://github.com/boostorg/boost/releases/
# Download release/boost-xxx-cmake.tar.gz and extract to agent/third_party/boost/

cd agent/third_party/
git clone https://github.com/coolight7/Boost-for-Android boost-android
cd boost-android

# Create third_party/boost-android-build-debug and third_party/boost-android-build-release directories
boost_source_dir=$PWD

boost_install_debug_dir="${boost_source_dir}/../boost-android-build-debug/"
mkdir -p "$boost_install_debug_dir"
boost_install_debug_dir=$(cd "$boost_install_debug_dir" && pwd)

boost_install_release_dir="${boost_source_dir}/../boost-android-build-release/"
mkdir -p "$boost_install_release_dir"
boost_install_release_dir=$(cd "$boost_install_release_dir" && pwd)

cd "$boost_source_dir"

export CXXFLAGS="-fPIC"
export CFLAGS="-fPIC"

./build-android.sh --boost=1.92.0 \
    --prefix=$boost_install_release_dir \
    --toolchain=llvm \
    --layout=system \
    --arch=arm64-v8a,armeabi-v7a,x86,x86_64 \
    --target-version=21
```

### Compiling OpenSSL from Source
- Compilation:
```sh
cd {PROJECT_ROOT}/agent/third_party/
wget https://github.com/openssl/openssl/releases/download/openssl-4.0.1/openssl-4.0.1.tar.gz
tar -xzvf openssl-4.0.1.tar.gz
cd openssl-4.0.1

openssl_source_dir=$PWD
openssl_build_dir="$openssl_source_dir/../OpenSSL-android-build"
mkdir -p "$openssl_build_dir"
openssl_build_dir=$(cd "$openssl_build_dir" && pwd)

cd "$openssl_source_dir"

# Change [ANDROID_NDK_ROOT] to your actual NDK path
export ANDROID_NDK_ROOT="/home/coolight/app/android_sdk/ndk/29.0.14206865"
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"

./Configure --release android-arm64 -fpic no-shared no-apps no-docs no-tests no-ocsp --prefix="$openssl_build_dir/arm64-v8a" --openssldir="$openssl_build_dir/arm64-v8a" '-Wl,-rpath,$(LIBRPATH)' -D__ANDROID_API__=21
make -j
make install

./Configure --release android-armeabi-v7a -fpic no-shared no-apps no-docs no-tests no-ocsp --prefix="$openssl_build_dir/armeabi-v7a" --openssldir="$openssl_build_dir/armeabi-v7a" '-Wl,-rpath,$(LIBRPATH)' -D__ANDROID_API__=21
make -j
make install
```

### Compiling agentxx
- Start compiling agentxx. For a release build, run:
```sh
cd {PROJECT_ROOT}/agent
./script/cross_android_release_build.sh
```

## Artifact Layout

- Executables: `agent/build/{platform}-{mode}/exec/agentxx_cli` / `agentxx_test` / `agentxx_benchmark`
- Plugin Shared Libraries (Standalone Dynamic Library Mode): `agent/build/{platform}-{mode}/exec/plugins/<plugin_name>/` (dispatched by directory when containing a `plugin.yaml` manifest)
- Shared Library (FFI): `agent/build/{platform}-{mode}/lib/libagentxx_shared.so` (Exports C symbols; see `agent/lib/ffi_symbols.map`)

## Common Issues
- [FAQ for more issues](FAQ.md)
