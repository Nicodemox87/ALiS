// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace alis {
inline std::uint64_t availableRamBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX m = {}; m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return m.ullAvailPhys;
#endif
    return std::uint64_t(2) << 30; // Conservative fallback, never assume unlimited memory.
}
}
