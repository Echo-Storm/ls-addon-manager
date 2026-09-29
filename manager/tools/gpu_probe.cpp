// Prints what the Performance tab and the header would show, once every half second: eam_gpuprobe [nvml|counters] [seconds]
// Run it twice at once (one of each) while the GPU is busy to compare NVML's numbers with Windows' counters (the fallback for AMD and Intel).
#include "../src/host/gpu_stats.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

int main(int argc, char** argv) {
    const bool counters = argc > 1 && !strcmp(argv[1], "counters");
    const int seconds = argc > 2 ? atoi(argv[2]) : 10;
    if (counters) eam::GpuStats::PreferCounters(true);
    eam::GpuStats& g = eam::GpuStats::Instance();
    for (int i = 0; i < seconds * 2; ++i) {
        const bool ok = g.SampleOnce();
        const eam::GpuStats::Snapshot s = g.Get();
        if (!ok) { printf("%s: not available: %s\n", counters ? "counters" : "nvml", s.why.c_str()); return 1; }
        printf("%-8s %-34s load %3u%%  vram %5llu / %5llu MB  power %s  temp %s  clocks %s  cards %d\n", counters ? "counters" : "nvml", s.name.c_str(), s.utilGpu,
               (unsigned long long)s.vramUsedMB, (unsigned long long)s.vramTotalMB,
               s.hasPower ? "yes" : "-", s.hasTemp ? "yes" : "-", s.hasClocks ? "yes" : "-", s.deviceCount);
        fflush(stdout);
        Sleep(500);
    }
    g.Shutdown();
    return 0;
}
