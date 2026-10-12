// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif
namespace alis {
inline std::uint64_t availableRamBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX m = {}; m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return m.ullAvailPhys;
#elif defined(__APPLE__)
    const mach_port_t host=mach_host_self();
    vm_size_t page=0;vm_statistics64_data_t stats={};
    mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    const bool ok=host_page_size(host,&page)==KERN_SUCCESS &&
        host_statistics64(host,HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&stats),&count)==KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(),host);
    if(ok&&page>0)return (std::uint64_t(stats.free_count)+stats.inactive_count)*page;
#endif
    return std::uint64_t(2) << 30; // Conservative fallback, never assume unlimited memory.
}
}
