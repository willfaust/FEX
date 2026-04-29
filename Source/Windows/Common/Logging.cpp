// SPDX-License-Identifier: MIT
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/LogManager.h>

#include <cstdio>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>

#ifdef FEX_IOS_HOST
#include <unistd.h>
#endif

namespace {
void (*WineDbgOut)(const char* Message);
FILE* LogFile;

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  const auto Output = fextl::fmt::format("{} {:X} {}\n", LogMan::DebugLevelStr(Level), GetCurrentThreadId(), Message);
#ifdef FEX_IOS_HOST
  /* iOS-Mythic: route directly to stderr (which is dup2'd to mythic-log.txt
   * by WineProcessBridge). __wine_dbg_output is exported but doesn't always
   * resolve via GetProcAddress on our embedded ntdll, so go around it. */
  ::write(2, Output.c_str(), Output.size());
  return;
#endif
  if (WineDbgOut) {
    WineDbgOut(Output.c_str());
  } else if (LogFile) {
    fwrite(Output.c_str(), 1, Output.size(), LogFile);
  }
}

static void AssertHandler(const char* Message) {
  const auto Output = fextl::fmt::format("A {}\n", Message);
  if (WineDbgOut) {
    WineDbgOut(Output.c_str());
  } else if (LogFile) {
    fwrite(Output.c_str(), 1, Output.size(), LogFile);
  }
}
} // namespace

namespace FEX::Windows::Logging {
void Init() {
#ifndef FEX_IOS_HOST
  FEX_CONFIG_OPT(SilentLog, SILENTLOG);
  if (SilentLog()) {
    return;
  }
#endif

#ifdef FEX_IOS_HOST
  /* iOS-Mythic: trace install via stderr directly to confirm Init() ran. */
  const char *m = "[FEX-iOS] Logging::Init installing MsgHandler\n";
  ::write(2, m, 47);
#endif

  WineDbgOut = reinterpret_cast<decltype(WineDbgOut)>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_dbg_output"));
  if (!WineDbgOut) {
    const auto Path = fextl::fmt::format("{}\\fex-{}.log", getenv("LOCALAPPDATA"), GetCurrentProcessId());
    LogFile = fopen(Path.c_str(), "a");
  }
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
}
} // namespace FEX::Windows::Logging
