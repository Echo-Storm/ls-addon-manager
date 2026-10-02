// nr_gpuloadtest: reads the graphics card's load through NVML the way the "Keep the graphics card under a limit" setting does (gpu_load.cpp), a few times, and says what it found.
//   nr_gpuloadtest [card name]      (without a name the first NVIDIA card is read; exit code 0 when a load was read or NVML is simply not there)
#include "addon/gpu_load.h"
#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    nr::GpuLoad load;
    const std::string card = argc > 1 ? argv[1] : "";
    for (int i = 0; i < 4; ++i) {
        const unsigned p = load.Percent(GetTickCount64(), card);
        printf("sample %d: %u %%  (available: %s%s%s)\n", i + 1, p, load.Available() ? "yes" : "no", load.Available() ? "" : ", ", load.Available() ? "" : load.Why().c_str());
        if (!load.Available()) return 0;   // no NVML (an AMD card): the setting does nothing there, and says so in the log
        Sleep(1100);
    }
    return 0;
}
