# Frequently Asked Questions (FAQ)

## Compiler Crashes with GCC Internal Compiler Error (ICE) During Build
- GCC itself crashed. Simply restarting the build will usually resolve it.
- Alternatively, modify the build script to reduce the number of parallel compilation jobs. This is often caused by insufficient memory or actual memory hardware instability (frequently seen with compatibility issues when mixing different RAM modules).
- If multiple builds continue to fail, there may indeed be an issue in the code. You can try switching to Windows with MSVC, or using Clang and other compilers. You can also have an AI inspect the specific file being compiled when the crash occurred.

## No Observable Effect at Runtime After AI Modifies Code
- Some AI assistants invoke `make` on a specific target directly during build without running `install`, so the executable used at runtime might still be the old binary. You can manually run the build script in `agent/script/` and test again.

## Enabling fPIC for Boost Static Library Build
- Simply add `cflags=-fPIC cxxflags=-fPIC`:
```sh
cd boost/

# Clean build cache
./b2 --clean-all
rm -rf bin.v2

./b2 cflags=-fPIC cxxflags=-fPIC {original_parameters}
```

## Whether CMake Should Use ExternalProject or FetchContent When Adding Dependencies
- We recommend standardizing on `ExternalProject`. Although `FetchContent` is also usable, their execution timing differs: libraries imported via `FetchContent` (executed during the configure stage) may fail to find libraries imported via `ExternalProject` (executed during the build stage) using `find_package`.
- We previously attempted standardizing on `FetchContent`. Initially, because `FetchContent` automatically inherits parent project variables, we thought it would be convenient for cross-platform and cross-compilation. However, it does not support non-CMake projects and lacks granular control over `install` and `build` details. Therefore, we ultimately standardized on `ExternalProject`.

## Running agentxx_cli and Shared Library File Locking
- On Windows, build scripts cannot overwrite the binaries while the application is currently running because the operating system locks active executables and DLLs.
- On Linux, while the file can be replaced/overwritten, doing so can cause the running application to crash (e.g. segmentation fault). Always stop running instances before rebuilding.
