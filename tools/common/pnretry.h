/* Event recovery must survive replacement of the control session.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <windows.h>
#include <string>

namespace pn {
class EventRetryLimiter {
    std::wstring instance;
    ULONGLONG failedAt = 0;
    bool seen = false;
public:
    void Reset() { instance.clear(); failedAt = 0; seen = false; }
    bool ShouldPause(const std::wstring &device, ULONGLONG now) {
        bool pause = seen && !_wcsicmp(instance.c_str(), device.c_str()) && now - failedAt < 10000;
        instance = device; failedAt = now; seen = true;
        return pause;
    }
};
}
