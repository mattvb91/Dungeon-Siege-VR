#pragma once

#include <windows.h>

namespace dsvr
{
    // M3 step 2: hardware write-watchpoints (debug registers DR0-DR2) on SiegeCamera fields, set on every
    // thread of the process except the caller. A vectored exception handler records which instructions
    // write them, plus code-looking return addresses from the stack, so we can find the per-frame camera
    // update to hook. Logs a summary when disarmed.
    void WatchCameraWrites(DWORD camera, DWORD durationMs);
}
