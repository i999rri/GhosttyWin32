#pragma once

#include <windows.h>
#include <cstdio>

// One line to the debugger's output window, Debug / ASan builds
// only — Release compiles the call and its format string out
// entirely, so a trace costs nothing to leave in.
//
//   DEBUG_TRACE(L"CellSnap[%llu]: applied %ux%u\n", tick, w, h);
//   DEBUG_TRACE(L"TearOut: spawnHost returned null\n");
//
// The format string is the first variadic argument so a call with no
// values is legal under both the traditional and the conformant
// preprocessor. Each subsystem prefixes its own lines (UndoPark[..],
// CellSnap[..], PanelInvariant, TearOut) so a filtered output window
// reads as one story.
#if defined(_DEBUG)
#define DEBUG_TRACE(...)                                              \
    do {                                                              \
        wchar_t debugTraceBuf_[224];                                  \
        swprintf_s(debugTraceBuf_, __VA_ARGS__);                      \
        OutputDebugStringW(debugTraceBuf_);                           \
    } while (0)
#else
#define DEBUG_TRACE(...) do { } while (0)
#endif
