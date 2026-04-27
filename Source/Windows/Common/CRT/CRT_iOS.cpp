// SPDX-License-Identifier: MIT
// Minimal CRT lifecycle hooks for the iOS-host arm64ec build of libarm64ecfex.dll.
// The full CRT.cpp redefines _tls_index, DllMainCRTStartup, section markers,
// and runs rpmalloc — all of which conflict with mingw's default startup
// chain that we use on iOS-host builds (since llvm-mingw 22.1.4's arm64ec
// EC-mangling doesn't reconcile cleanly with -nostdlib + custom CRT). This
// stub provides ONLY the symbols Module.cpp's namespace FEX::Windows
// references, so the link succeeds.

namespace FEX::Windows {
void InitCRTProcess() {
  // No-op: mingw's default startup ran ctors and initialized the heap.
}
void InitCRTThread() {
  // No-op: mingw's default per-thread init handles TLS.
}
void DeinitCRTThread() {
  // No-op: matches InitCRTThread.
}
} // namespace FEX::Windows
