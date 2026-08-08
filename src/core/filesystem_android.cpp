/**
 * @file        rex/core/filesystem_android.cpp
 * @brief       Android filesystem hooks (content:// URI access).
 *
 * content:// URIs require JNI into the app's Java ContentResolver, which is
 * wired up in the app-shell phase. Until then these are minimal stubs: URIs are
 * recognised (pure string check) but cannot be opened, so callers fail
 * gracefully. Regular path-based file access is unaffected.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <rex/filesystem.h>

namespace rex {
namespace filesystem {

void AndroidInitialize() {}

void AndroidShutdown() {}

bool IsAndroidContentUri(const std::string_view source) {
  constexpr std::string_view kContentScheme = "content://";
  return source.size() >= kContentScheme.size() &&
         source.substr(0, kContentScheme.size()) == kContentScheme;
}

int OpenAndroidContentFileDescriptor(const std::string_view /*uri*/,
                                     const char* /*mode*/) {
  // TODO(app-shell): open via ContentResolver.openFileDescriptor over JNI.
  return -1;
}

}  // namespace filesystem
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
