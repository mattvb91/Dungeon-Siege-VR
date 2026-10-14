#pragma once

namespace dsvr
{
    // M3 step 1: read-only probe that periodically logs SiegeCamera fields so the statically derived
    // offsets (docs/EXE-ANALYSIS.md) can be confirmed against live gameplay.
    void StartCameraProbe();
}
