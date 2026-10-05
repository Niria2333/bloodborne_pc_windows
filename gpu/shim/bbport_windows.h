// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef _WIN32
#include <windows.h>
// The SDK macro would also expand vk::MemoryBarrier in Vulkan-Hpp users.
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#endif
