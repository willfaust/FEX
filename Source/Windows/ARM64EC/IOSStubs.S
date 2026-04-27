// Stubs for symbols missing in llvm-mingw's arm64ec runtime when building
// libarm64ecfex.dll for iOS hosting. These are referenced by C++ EH unwind
// tables and POSIX-extension wrappers, but FEX doesn't actually throw or
// call them — they just need to satisfy the linker.

#ifdef FEX_IOS_HOST
.text

// libc++abi defines `T #__gxx_personality_seh0` (EC-mangled). The linker
// references the plain non-EC form too — provide a non-EC alias here.
.global __gxx_personality_seh0
__gxx_personality_seh0:
  mov w0, #0
  ret

.global "#access"
"#access":
.global access
access:
  // POSIX access() — return -1 (failure). Not used by FEX hot paths.
  mov w0, #-1
  ret

.global "#btowc"
"#btowc":
.global btowc
btowc:
  // ANSI char-to-wchar promote. Trivial mapping for ASCII.
  ret

.global "#wctob"
"#wctob":
.global wctob
wctob:
  // ANSI wchar-to-char demote. Trivial mapping for ASCII.
  ret

#endif
