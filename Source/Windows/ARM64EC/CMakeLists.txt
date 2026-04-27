add_library(arm64ecfex SHARED
  Module.cpp
  Module.S
  IOSStubs.S
  libarm64ecfex.def
  $<TARGET_OBJECTS:FEXCore_object>)

patch_library_wine(arm64ecfex)

target_include_directories(arm64ecfex PRIVATE
  "${CMAKE_SOURCE_DIR}/Source/Windows/include/"
  "${CMAKE_SOURCE_DIR}/Source/"
  "${CMAKE_SOURCE_DIR}/Source/Windows/")

target_link_libraries(arm64ecfex PRIVATE
  FEXCore_Base
  Common
  CommonTools
  CommonWindows
  CommonWindowsRuntime
  ntdll_ex)

# Link order: libc++ provides STL, libc++abi provides exception personality
# (__gxx_personality_seh0). Both need --whole-archive on llvm-mingw arm64ec
# because EC-mangled symbols are weak/lazy and don't get pulled by default
# resolution. But linking BOTH whole-archive duplicates std::bad_alloc vtables
# etc — they're defined in both archives. Workaround: --whole-archive libc++
# only, then explicit-load libc++abi's personality function via a forced
# extern reference in target_link_options.
target_link_options(arm64ecfex PRIVATE -static -nostdlib -nostartfiles -nodefaultlibs
  -Wl,--whole-archive -lc++ -Wl,--no-whole-archive
  -Wl,--whole-archive -lc++abi -Wl,--no-whole-archive
  -lunwind -lm -lmsvcrt -lkernel32 -lucrt)
target_link_libraries(arm64ecfex PRIVATE ${LIBGCC_PATH})

# llvm-mingw arm64ec link issues: btowc/wctob/access are POSIX-extension
# functions not in our restricted ucrt subset; __gxx_personality_seh0 is the
# C++ exception personality. Stubs in IOSStubs.S satisfy the linker without
# pulling in heavier stdlib chains. FEX doesn't actually throw or call these.
install(TARGETS arm64ecfex RUNTIME
  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  COMPONENT Runtime)
