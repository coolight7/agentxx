#if _WIN32
#include <windows.h>
#undef max
#undef min
#else
#include <pthread.h>
#include <unistd.h>
#endif

#if _WIN32
#define DllImport         __declspec(dllimport)
#define DllExport         __declspec(dllexport)
#define FFI_PLUGIN_EXPORT DllExport
#else
#define FFI_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#if __cplusplus
extern "C" {
#endif

FFI_PLUGIN_EXPORT void* agentxx_malloc(size_t size);
FFI_PLUGIN_EXPORT void  agentxx_free(const void* ptr);

#if __cplusplus
}
#endif