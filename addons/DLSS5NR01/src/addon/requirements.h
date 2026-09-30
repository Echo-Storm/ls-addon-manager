#pragma once
// What Neural Rendering needs to run, checked and explained in plain words: an NVIDIA card, the NVIDIA driver's NGX core, the model file
// (nvngx_dlssnr.dll), this addon's helper DLL, and whether the engine started. Evaluate() is pure (it takes what was found and says what it
// means), Gather() is what looks at the machine. Nothing here loads the model or downloads anything.
#include <cstdint>
#include <string>
#include <vector>

namespace req {

enum class Level { Ok, Note, Missing };

struct Row {
    std::string label;   // "Graphics card", "NVIDIA driver", "Model file", "Helper DLL", "Engine"
    std::string value;   // what was found, in one line
    std::string hint;    // what to do about it; empty when the row is Ok
    Level level = Level::Ok;
};

// The model this addon was built and tested against. Another build may work; it has just not been seen here.
constexpr uint64_t kTestedModelSize = 165840496ull;
constexpr const char* kTestedModelVersion = "310.8";
constexpr uint64_t kSmallestPlausibleModel = 20ull * 1024 * 1024;   // the real one is about 158 MB; anything far smaller is not it

enum class EngineState { NotStarted, Ready, Running, Failed };

// The compatibility self-test (nr_selftest.exe): does the model file create its feature on this graphics card, and change a test picture?
enum class SelfTestState { NotRun, Running, Passed, Failed };

struct SelfTestResult {
    bool passed = false;
    int code = -1;               // nr_selftest's exit code: 0 passed, 10..20 what went wrong (see nr_selftest.cpp)
    std::string key;             // its short name (MODEL_LOAD, NOT_SUPPORTED, ...), or NOT_FOUND, CRASH, TIMEOUT, UNEXPECTED from here
    std::string text;            // what it says, in words
    std::string gpu;             // the graphics card it tested (with two NVIDIA cards, the one Lossless Scaling runs on when that is known)
};

struct Inputs {
    bool nvidiaFound = false;                // an NVIDIA hardware adapter exists
    std::string gpuName;
    uint64_t gpuMemoryBytes = 0;

    bool ngxRegistered = false;              // the driver's registry entry (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore) names a folder
    bool ngxCoreFound = false;               // and _nvngx.dll is in it
    std::string ngxCoreVersion;              // its file version, "32.0.16.1692"

    std::string modelPath;                   // where the model is expected (UTF-8)
    bool modelFound = false;
    uint64_t modelSize = 0;
    std::string modelVersion;                // its file version, "310.8.0.0" (or "310,8,0,0")

    bool helperFound = false;                // nvngx.dll_dlss5nr01.dll beside the addon

    SelfTestState selfTest = SelfTestState::NotRun;
    std::string selfTestKey, selfTestText;   // what the last run said (see SelfTestResult)

    EngineState engine = EngineState::NotStarted;
    std::string engineError;                 // the engine's own message when it failed
};

struct Report {
    std::vector<Row> rows;
    Level overall = Level::Ok;               // Missing if any row is; Note if any row is; else Ok
    std::string headline;                    // the first problem in one line, or that everything is in place
};

Report Evaluate(const Inputs& in);

std::string DriverFromNgxVersion(const std::string& fileVersion);   // "32.0.16.1692" -> "616.92"; "" when it is not in that shape
std::string VersionShort(const std::string& fileVersion);           // "310.8.0.0" or "310,8,0,0" -> "310.8"
std::string SizeText(uint64_t bytes);                               // "158.2 MB"
std::string PlainEngineError(const std::string& raw);               // the engine's message in words a person can act on

// Looks at this machine. `modelPath` is where the model is expected, `addonDir` the folder holding the helper DLL. The engine fields are
// left at their defaults for the caller to fill in. Reads file headers only: it does not load the model, and takes well under a second.
Inputs Gather(const std::wstring& modelPath, const std::wstring& addonDir);

// ---- the compatibility self-test: running it and reading what it says
struct ProcessResult {
    bool started = false;        // the program could be started
    bool timedOut = false;       // it was still running after the time allowed, and was ended
    unsigned long exitCode = 0;
    unsigned long startError = 0;
    std::string output;          // everything it printed
};
// Runs a command line with no window, waits up to `timeoutMs`, ends it if it is still running, and returns what it printed and how it ended.
// The program is put in a job that ends it if this process ends first, so a test program never outlives Lossless Scaling.
ProcessResult RunProcess(const std::wstring& commandLine, unsigned timeoutMs);

// Reads nr_selftest's output: the last line starting with "SELFTEST" is its verdict (the NGX core may print after it), and its exit code has to
// agree. A program that ended without a verdict crashed, or was ended for taking too long.
SelfTestResult ParseSelfTest(const std::string& output, unsigned long exitCode, bool timedOut);

// Runs <addonDir>\nr_selftest.exe on `modelPath` and returns the verdict. Takes seconds: the model is loaded and run once. Call from a worker thread.
// luid: the card to test as "high:low" in hex (empty: the first NVIDIA card). With two NVIDIA cards the model runs on the one Lossless Scaling
// runs on (the engine follows it), which is not always the first one DXGI lists.
SelfTestResult RunSelfTest(const std::wstring& addonDir, const std::wstring& modelPath, const std::wstring& lsDir, unsigned timeoutMs = 90000, const std::wstring& luid = std::wstring());

struct PlaceResult {
    bool ok = false;
    std::string message;          // what happened, in words, for the panel
    std::wstring placedPath;      // where the model now is
    std::wstring backupPath;      // where the file that was there before went, when there was one
};

// Puts a model file the user picked next to Lossless Scaling as nvngx_dlssnr.dll. The picked file is only read. A file already there is moved
// to `backupDir` with a time stamp, never deleted or overwritten; the new copy is written under a temporary name and renamed into place, so a
// failed copy leaves the old file where it was. Refuses a file that is not a .dll or is too small to be the model. Picking the file that is
// already in place is a success that changes nothing. Nothing is downloaded and nothing is checked against a list of known files.
PlaceResult PlaceModel(const std::wstring& source, const std::wstring& lsDir, const std::wstring& backupDir);

} // namespace req
