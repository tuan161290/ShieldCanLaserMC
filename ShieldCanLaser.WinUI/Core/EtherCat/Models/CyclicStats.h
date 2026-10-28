#pragma once
#include <cstdint>

namespace ecm
{
    /// <summary>
    /// Running statistics for the native cyclic timer of the SOEM bus since the bus was last opened
    /// — reset on every Open()/ConnectAllAxis(). PeriodJitter: how far the actual gap between
    /// successive timer firings was from the nominal cycle period. ScanTime: how long one
    /// send+receive of process data took (the real EtherCAT frame round-trip). All values in
    /// microseconds. SampleCount == 0 means no data yet (bus not open, or the simulated bus).
    /// </summary>
    struct CyclicStats
    {
        double PeriodJitterMinUs = 0;
        double PeriodJitterMaxUs = 0;
        double PeriodJitterAvgUs = 0;

        double ScanTimeMinUs = 0;
        double ScanTimeMaxUs = 0;
        double ScanTimeAvgUs = 0;

        uint32_t SampleCount = 0;
    };
}
