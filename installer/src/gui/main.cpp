// LS Addon Manager Setup: a small wizard over the installer core (installer.h), and a silent mode for scripts and tests.
//
//   LSAddonManagerSetup.exe                                         the wizard
//   LSAddonManagerSetup.exe --folder <Lossless Scaling folder>      the wizard, starting with that folder
//   LSAddonManagerSetup.exe --silent status|install|uninstall --folder <dir> [--remove-addons] [--payload <dir>] [--log <file>]
//   LSAddonManagerSetup.exe --version                               what this setup carries (used by the build to check it)
//   LSAddonManagerSetup.exe --folder <dir> --update-when-closed [--restart]
//                                                                   the manager's update: waits (a window with Cancel) until Lossless Scaling
//                                                                   running from that folder has closed, then updates it as the wizard does;
//                                                                   --restart starts Lossless Scaling again afterwards
//   LSAddonManagerSetup.exe --shot <dir> --folder <fake folder>     the README's pictures: the start page, then (after installing into that folder)
//                                                                   the result page, saved as setup-start.bmp and setup-done.bmp; the window
//                                                                   is kept off the screen, nothing is remembered. For a throwaway folder only.
//
// The files it installs are a resource of this exe (see payload.h); --payload <dir> uses a folder instead (for development and tests).
// The wizard is Windows' own TaskDialog: a few pages moved between with TDM_NAVIGATE_PAGE. Nothing here decides anything about files: that is the core's job.
#include "fileinfo.h"
#include "installer.h"
#include "lsfolder.h"
#include "payload.h"
#include "state.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;
using namespace setup;

namespace {

// --------------------------------------------------------------------------------------------------------------------------------------- words

const wchar_t* const kNoModelNotice =
    L"DLSS 5 Neural Rendering needs your own copy of nvngx_dlssnr.dll. It is not part of this download, and this project does not download it or say where to get it. "
    L"Setup can copy your file into the folder afterwards. Everything else works without it.\n"
    L"Tested with World of Warcraft: Forever, Fallout: New Vegas and Lossless Scaling 3.2.2.0. Unsigned, like everything in this project.";

std::wstring W(const std::string& s) { return Widen(s); }

std::wstring PlanText(Action a) {
    const wchar_t* safety =
        L"\n\nEverything that gets replaced is first copied to a backups folder inside the Lossless Scaling folder; nothing is deleted. "
        L"Your settings (addons\\config.json), your other addons and your removed addons are never touched. The new files are checked after copying, "
        L"and if anything goes wrong the folder is put back exactly as it was.";
    switch (a) {
    case Action::Install:
        return std::wstring(L"Lossless Scaling's own Lossless.dll is kept, renamed Lossless_original.dll (the manager passes everything on to it). "
                            L"LS Addon Manager's Lossless.dll takes its place, with its two icon files and the addons folder.") + safety;
    case Action::Update:
        return std::wstring(L"The earlier LS Addon Manager files are replaced with these; Lossless_original.dll stays as it is.") + safety;
    case Action::Repair:
        return std::wstring(L"Lossless Scaling updated itself and put its own Lossless.dll back over LS Addon Manager's. Setup keeps that new one as Lossless_original.dll "
                            L"and puts LS Addon Manager's Lossless.dll back in.") + safety;
    case Action::Reinstall:
        return std::wstring(L"Puts the LS Addon Manager files back as they come in this download.") + safety;
    default: return L"";
    }
}

// --------------------------------------------------------------------------------------------------------------------------------------- state

enum Cmd {
    kAction = 1001, kUninstall, kUninstallAll, kOther, kAdmin, kRecheck, kPlaceModel, kOpenFolder, kOpenBackups, kBrowse, kBack, kToStart,
    kCandidate0 = 2000,
};

struct Page {
    std::wstring title, instruction, content, expanded, footer;
    std::vector<std::pair<int, std::wstring>> links;
    std::vector<TASKDIALOG_BUTTON> buttons;
    TASKDIALOGCONFIG cfg{};
    bool progress = false;
};

struct App {
    std::wstring exeDir, tempDir, payloadDir, folder, note;
    PayloadInfo payload;
    std::string payloadError;
    State state;
    Advice advice;
    std::vector<Candidate> candidates;
    bool elevated = false, writable = true;

    Result result;
    bool resultIsUninstall = false;
    std::thread worker;
    std::atomic<bool> done{false};
    bool showingProgress = false;

    bool waitForClose = false, restartAfter = false;   // --update-when-closed, --restart
    bool waiting = false;                              // the waiting page is shown
    bool restarted = false;
    DWORD lastLook = 0;

    int testCloseMs = 0;
    DWORD startTick = 0;
    std::wstring shotDir;   // --shot: where the two pictures go
    int shotStage = 0;      // 0 the start page, 1 installing, 2 the result page shown, 3 done
    DWORD shotTick = 0;
    HICON icon = nullptr;
    std::vector<std::unique_ptr<Page>> pages;   // kept alive until the dialog ends: the dialog may still point into the last ones
};

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n)) r = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return r;
}

// Can this program create a file in the folder? (Program Files and some Steam libraries say no without administrator rights.)
bool CanWriteFolder(const std::wstring& dir) {
    const std::wstring probe = JoinPath(dir, L".echo_setup_write_test_" + std::to_wstring(GetCurrentProcessId()));
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void Refresh(App& a) {
    a.state = Inspect(a.folder);
    a.advice = Advise(a.state, a.payload.ok ? a.payload.version : std::string());
    a.writable = CanWriteFolder(a.folder);
}

// A folder the person chose (from the list or with Browse) is remembered at once, so the next start offers it first, even if nothing was installed. Only a folder
// that really is Lossless Scaling's is kept.
void RememberChoice(const App& a) {
    if (a.state.situation != Situation::NotLosslessScaling) RememberFolder(a.folder);
}

// --------------------------------------------------------------------------------------------------------------------------------------- pages

HRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data);

Page& NewPage(App& a, const std::wstring& instruction, const std::wstring& content, bool closeButton = true) {
    a.pages.push_back(std::make_unique<Page>());
    Page& p = *a.pages.back();
    p.title = L"LS Addon Manager " + (a.payload.ok ? W(a.payload.version) + L" " : std::wstring()) + L"Setup";
    p.instruction = instruction;
    p.content = content;
    p.footer = kNoModelNotice;
    p.cfg.cbSize = sizeof(p.cfg);
    p.cfg.pszWindowTitle = p.title.c_str();
    p.cfg.pszMainInstruction = p.instruction.c_str();
    p.cfg.pszContent = p.content.c_str();
    p.cfg.pfCallback = DialogProc;
    p.cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&a);
    p.cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_CALLBACK_TIMER;
    p.cfg.cxWidth = 260;
    p.cfg.dwCommonButtons = closeButton ? TDCBF_CLOSE_BUTTON : 0;
    if (a.icon) { p.cfg.dwFlags |= TDF_USE_HICON_MAIN; p.cfg.hMainIcon = a.icon; }
    else p.cfg.pszMainIcon = TD_INFORMATION_ICON;
    return p;
}

void Finish(Page& p, bool withFooter = true) {
    p.buttons.clear();
    for (const auto& l : p.links) p.buttons.push_back({l.first, l.second.c_str()});
    p.cfg.pButtons = p.buttons.empty() ? nullptr : p.buttons.data();
    p.cfg.cButtons = static_cast<UINT>(p.buttons.size());
    if (p.buttons.empty()) p.cfg.dwFlags &= ~static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS);   // the flag without any custom button is E_INVALIDARG
    if (!p.expanded.empty()) {
        p.cfg.pszExpandedInformation = p.expanded.c_str();
        p.cfg.pszCollapsedControlText = L"Show the details";
        p.cfg.pszExpandedControlText = L"Hide the details";
    }
    if (withFooter) {
        p.cfg.pszFooter = p.footer.c_str();
        p.cfg.pszFooterIcon = TD_INFORMATION_ICON;
    }
}

void Navigate(HWND hwnd, Page& p) { SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&p.cfg)); }

Page& MainPage(App& a) {
    a.showingProgress = false;
    if (!a.payload.ok) {
        Page& p = NewPage(a, L"This setup file is damaged.", W(a.payloadError.empty() ? "It carries no files to install." : a.payloadError) +
                                                            L"\n\nDownload it again from the project's releases page.");
        Finish(p, false);
        return p;
    }
    std::wstring content = W(a.advice.detail);
    if (!content.empty()) content += L"\n\n";
    content += L"Lossless Scaling folder:\n" + a.folder;
    if (!a.state.lsVersion.empty()) content += L"\nLossless Scaling " + W(a.state.lsVersion);
    if (!a.note.empty()) { content += L"\n\n" + a.note; a.note.clear(); }
    if (a.advice.blocked) content += L"\n\n" + W(a.advice.blockedReason);
    const bool canChange = a.writable || a.elevated;
    if (!canChange && a.state.situation != Situation::NotLosslessScaling) content += L"\n\nWindows only lets an administrator change files in that folder.";

    Page& p = NewPage(a, W(a.advice.headline), content);
    const bool haveAction = a.advice.action != Action::None && !a.advice.blocked;
    if (haveAction && canChange) {
        std::wstring label = W(a.advice.actionLabel);
        if (a.advice.action == Action::Install) label += L"\nKeeps Lossless Scaling's own file, adds the manager and its addons";
        else if (a.advice.action == Action::Update) label += L"\nReplaces the earlier version";
        else if (a.advice.action == Action::Repair) label += L"\nPuts the manager back after the Lossless Scaling update";
        else if (a.advice.action == Action::Reinstall) label += L"\nPuts its files back as they came";
        p.links.push_back({kAction, label});
        p.expanded = PlanText(a.advice.action);
    }
    if ((haveAction || a.advice.canUninstall) && !canChange) p.links.push_back({kAdmin, L"Restart as administrator\nThis folder can only be changed with administrator rights"});
    if (a.advice.blocked) p.links.push_back({kRecheck, L"Check again\nAfter closing Lossless Scaling"});
    if (a.advice.canUninstall && canChange && !a.advice.blocked) {
        p.links.push_back({kUninstall, L"Uninstall\nPuts Lossless Scaling's own Lossless.dll back. Your addons and settings stay."});
        p.links.push_back({kUninstallAll, L"Uninstall and take the addons out too\nThe addons folder, with your addons' settings, is moved to the backups folder, not deleted."});
    }
    p.links.push_back({kOther, L"Use a different folder..."});
    Finish(p);
    return p;
}

Page& ChooserPage(App& a, const std::wstring& why) {
    a.showingProgress = false;
    std::wstring content = why;
    if (!content.empty()) content += L"\n\n";
    content += a.candidates.empty() ? L"Setup did not find Lossless Scaling by itself. Choose its folder: the one that holds LosslessScaling.exe."
                                    : L"Choose the folder of the Lossless Scaling you want to use. Setup lists what it found; \"Browse\" picks any other folder.";
    Page& p = NewPage(a, L"Which Lossless Scaling?", content);
    int i = 0;
    for (const auto& c : a.candidates) {
        if (i >= 8) break;
        const State s = Inspect(c.dir);
        std::wstring line = c.dir + L"\n";
        line += s.installedVersion.empty() ? L"LS Addon Manager is not installed here" : L"LS Addon Manager " + W(s.installedVersion) + L" is installed here";
        if (!s.lsVersion.empty()) line += L"  |  Lossless Scaling " + W(s.lsVersion);
        line += L"  |  found: " + W(c.how);
        p.links.push_back({kCandidate0 + i, line});
        ++i;
    }
    p.links.push_back({kBrowse, L"Browse for the folder..."});
    if (!a.folder.empty()) p.links.push_back({kBack, L"Back"});
    Finish(p);
    return p;
}

Page& ProgressPage(App& a, const std::wstring& what) {
    a.showingProgress = true;
    Page& p = NewPage(a, what, L"This takes a few seconds. Please do not switch off the computer.", false);
    p.progress = true;
    p.cfg.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
    Finish(p, false);
    return p;
}

// The manager's update: Lossless Scaling has to close first (the manager lives inside it). Checked a few times a second.
Page& WaitingPage(App& a) {
    a.showingProgress = true;   // the marquee; and the window test does not close it while it waits
    a.waiting = true;
    Page& p = NewPage(a, L"Close Lossless Scaling to update LS Addon Manager" + (a.payload.ok ? L" to " + W(a.payload.version) : std::wstring()) + L".",
                      L"Setup is waiting for it. Close Lossless Scaling: its window, or right-click its icon next to the clock and choose Exit.\n\n"
                      L"Then the update runs by itself, with the usual backups" +
                      std::wstring(a.restartAfter ? L", and Lossless Scaling starts again." : L".") +
                      L"\n\nLossless Scaling folder:\n" + a.folder);
    p.progress = true;
    p.cfg.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
    Finish(p, false);
    return p;
}

// Lossless Scaling again, after the manager's update (--restart). From an elevated Setup it is started through Explorer, so that it does not
// inherit administrator rights it never had.
void RestartLosslessScaling(App& a) {
    const std::wstring exe = JoinPath(a.folder, L"LosslessScaling.exe");
    if (!Exists(exe)) return;
    if (a.elevated) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, a.folder.c_str(), SW_SHOWNORMAL);
    a.restarted = true;
}

Page& ResultPage(App& a) {
    a.showingProgress = false;
    const Result& r = a.result;
    std::wstring instruction;
    if (r.ok) instruction = a.resultIsUninstall ? L"Uninstalled." : L"Done.";
    else instruction = r.rolledBack ? L"It did not work. Nothing was changed." : L"It did not work.";
    std::wstring content = W(r.message);
    if (r.ok && !a.resultIsUninstall) content += a.restarted ? L"\n\nLossless Scaling is starting again; the manager opens with it."
                                                             : L"\n\nStart Lossless Scaling once; the manager opens by itself.";
    if (!a.note.empty()) { content += L"\n\n" + a.note; a.note.clear(); }
    Page& p = NewPage(a, instruction, content);
    if (!r.log.empty() || !r.backupDir.empty()) {
        for (const auto& line : r.log) p.expanded += W(line) + L"\n";
        if (!r.backupDir.empty()) p.expanded += L"\nBackups: " + r.backupDir;
    }
    if (r.ok && !a.resultIsUninstall && !Exists(JoinPath(a.folder, L"nvngx_dlssnr.dll")))
        p.links.push_back({kPlaceModel, L"Copy my nvngx_dlssnr.dll into this folder...\nOnly for DLSS 5 Neural Rendering. Choose the file you already have; nothing is downloaded."});
    p.links.push_back({kOpenFolder, L"Open the Lossless Scaling folder"});
    if (!r.backupDir.empty()) p.links.push_back({kOpenBackups, L"Open the backups folder\nWhat was replaced or taken out is there"});
    p.links.push_back({kToStart, L"Back to the start"});
    Finish(p);
    return p;
}

// --------------------------------------------------------------------------------------------------------------------------------------- actions

// A folder picker (FOS_PICKFOLDERS) or a file picker for a .dll; returns an empty string when cancelled.
std::wstring PickFile(HWND owner, bool folders) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    if (folders) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Choose the Lossless Scaling folder (the one with LosslessScaling.exe)");
    } else {
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        dlg->SetTitle(L"Choose your nvngx_dlssnr.dll");
        const COMDLG_FILTERSPEC filter[] = {{L"DLL files", L"*.dll"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filter);
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// Copy the person's own model file next to LosslessScaling.exe, checked by hash; only when there is none yet, so nothing is overwritten.
std::wstring PlaceModel(App& a, const std::wstring& src) {
    const std::wstring target = JoinPath(a.folder, L"nvngx_dlssnr.dll");
    if (Exists(target)) return L"There is already a nvngx_dlssnr.dll in that folder, so nothing was copied.";
    uint64_t size = 0;
    if (!FileSizeOf(src, size) || size == 0) return L"That file could not be read, or it is empty.";
    const std::wstring temp = target + L".tmp";
    if (!CopyFileW(src.c_str(), temp.c_str(), FALSE)) return L"The file could not be copied into the folder.";
    if (Sha256File(src).empty() || Sha256File(src) != Sha256File(temp)) { DeleteFileW(temp.c_str()); return L"The copy did not match the original, so it was removed."; }
    if (!MoveFileExW(temp.c_str(), target.c_str(), 0)) { DeleteFileW(temp.c_str()); return L"The copy could not be put in place."; }
    return L"Copied nvngx_dlssnr.dll into the folder. Open the manager, open the Neural Rendering panel and press \"Test compatibility\".";
}

void StartWork(HWND hwnd, App& a, bool uninstall, bool removeAddons) {
    a.done = false;
    a.resultIsUninstall = uninstall;
    Navigate(hwnd, ProgressPage(a, uninstall ? L"Taking LS Addon Manager out..." : L"Installing LS Addon Manager..."));
    if (a.worker.joinable()) a.worker.join();
    a.worker = std::thread([&a, uninstall, removeAddons] {
        a.result = uninstall ? Uninstall(a.folder, removeAddons) : Install(a.folder, a.payloadDir);
        a.done = true;
    });
}

bool RestartElevated(App& a) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // A folder that ends in a backslash (a drive root) would escape the closing quote in the command line: such a backslash is written twice.
    const std::wstring folderArg = (!a.folder.empty() && a.folder.back() == L'\\') ? a.folder + L"\\" : a.folder;
    const std::wstring args = L"--folder \"" + folderArg + L"\"" + (a.tempDir.empty() || a.payloadDir.empty() || a.payloadDir.find(a.tempDir) == 0 ? L"" : L" --payload \"" + a.payloadDir + L"\"");
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", exe, args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

HRESULT OnButton(HWND hwnd, App& a, int id) {
    if (id >= kCandidate0 && id < kCandidate0 + 8) {
        const size_t i = static_cast<size_t>(id - kCandidate0);
        if (i < a.candidates.size()) { a.folder = a.candidates[i].dir; Refresh(a); RememberChoice(a); Navigate(hwnd, MainPage(a)); }
        return S_FALSE;
    }
    switch (id) {
    case kAction: StartWork(hwnd, a, false, false); return S_FALSE;
    case kUninstall: StartWork(hwnd, a, true, false); return S_FALSE;
    case kUninstallAll: StartWork(hwnd, a, true, true); return S_FALSE;
    case kOther: Navigate(hwnd, ChooserPage(a, L"")); return S_FALSE;
    case kBack: case kToStart: case kRecheck:
        a.candidates = FindCandidates();
        Refresh(a);
        Navigate(hwnd, MainPage(a));
        return S_FALSE;
    case kBrowse: {
        const std::wstring picked = PickFile(hwnd, true);
        if (!picked.empty()) {
            a.folder = picked;
            Refresh(a);
            if (a.state.situation == Situation::NotLosslessScaling) {   // the parent of the folder was picked: use the Lossless Scaling folder inside it, if there is exactly one
                const auto inside = ScanDrive(picked);
                if (inside.size() == 1) { a.folder = inside[0]; Refresh(a); a.note = L"Lossless Scaling was inside the folder you chose, so Setup uses that one."; }
            }
            RememberChoice(a);
            Navigate(hwnd, MainPage(a));
        }
        return S_FALSE;
    }
    case kAdmin:
        if (RestartElevated(a)) return S_OK;   // the elevated copy takes over; this one closes
        a.note = L"Setup could not restart itself as administrator.";
        Navigate(hwnd, MainPage(a));
        return S_FALSE;
    case kPlaceModel: {
        const std::wstring src = PickFile(hwnd, false);
        if (!src.empty()) a.note = PlaceModel(a, src);
        Navigate(hwnd, ResultPage(a));
        return S_FALSE;
    }
    case kOpenFolder: ShellExecuteW(nullptr, L"open", a.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return S_FALSE;
    case kOpenBackups: ShellExecuteW(nullptr, L"open", a.result.backupDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return S_FALSE;
    default: return S_OK;   // Close
    }
}

// The window as it is drawn, into a 32-bit BMP (PrintWindow with the full content, so it works off the screen too).
bool SaveWindowPicture(HWND hwnd, const std::wstring& path) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    const bool drawn = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT) != FALSE;
    bool ok = false;
    if (drawn && bits) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
            fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
            BITMAPINFOHEADER ih = bi.bmiHeader; ih.biHeight = -h;   // top-down rows, as drawn
            ok = fwrite(&fh, sizeof fh, 1, f) == 1 && fwrite(&ih, sizeof ih, 1, f) == 1 && fwrite(bits, static_cast<size_t>(w) * h * 4, 1, f) == 1;
            fclose(f);
        }
    }
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr, screen);
    return ok;
}

HRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data) {
    App& a = *reinterpret_cast<App*>(data);
    switch (msg) {
    case TDN_CREATED:
    case TDN_NAVIGATED:
        if (a.showingProgress) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        if (!a.shotDir.empty()) SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);   // off the screen
        break;
    case TDN_TIMER:
        if (!a.shotDir.empty()) {   // the README's pictures: the start page, install, the result page, close
            const DWORD now = GetTickCount();
            if (a.shotStage == 0 && now - a.startTick > 1200) {
                SaveWindowPicture(hwnd, JoinPath(a.shotDir, L"setup-start.bmp"));
                a.shotStage = 1;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, kAction, 0);
            } else if (a.shotStage == 2 && now - a.shotTick > 1200) {
                SaveWindowPicture(hwnd, JoinPath(a.shotDir, L"setup-done.bmp"));
                a.shotStage = 3;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);
            }
        }
        if (a.waiting && GetTickCount() - a.lastLook >= 200) {
            a.lastLook = GetTickCount();
            if (!LosslessScalingRunning(a.folder)) {
                a.waiting = false;
                Refresh(a);
                const bool canChange = a.writable || a.elevated;
                if (a.advice.action != Action::None && !a.advice.blocked && canChange && a.payload.ok) StartWork(hwnd, a, false, false);
                else Navigate(hwnd, MainPage(a));   // something else is in the way: the usual page says what, and what to do
            }
        } else if (a.showingProgress && a.done) {
            if (a.worker.joinable()) a.worker.join();
            if (a.result.ok && !a.resultIsUninstall && a.shotDir.empty()) RememberFolder(a.folder);
            Refresh(a);
            if (a.result.ok && !a.resultIsUninstall && a.restartAfter) RestartLosslessScaling(a);
            Navigate(hwnd, ResultPage(a));
            if (a.shotStage == 1) { a.shotStage = 2; a.shotTick = GetTickCount(); }
        } else if (a.testCloseMs > 0 && !a.showingProgress && GetTickCount() - a.startTick > static_cast<DWORD>(a.testCloseMs)) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);   // the window test closes the wizard by itself
        }
        break;
    case TDN_BUTTON_CLICKED:
        return OnButton(hwnd, a, static_cast<int>(wp));
    }
    return S_OK;
}

// --------------------------------------------------------------------------------------------------------------------------------------- command line

std::wstring Flag(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == name) return args[i + 1];
    return std::wstring();
}
bool Has(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (const auto& a : args) if (a == name) return true;
    return false;
}

struct Out {
    FILE* file = nullptr;
    bool console = false;
    void Line(const std::string& s) {
        if (file) { fputs((s + "\n").c_str(), file); fflush(file); }
        if (console) puts(s.c_str());
    }
};

const char* SituationText(Situation s) {
    switch (s) {
    case Situation::NotLosslessScaling: return "not a Lossless Scaling folder";
    case Situation::NotInstalled: return "not installed";
    case Situation::Installed: return "installed";
    case Situation::AfterLsUpdate: return "after a Lossless Scaling update";
    case Situation::NoOriginal: return "no original";
    case Situation::BothOurs: return "both ours";
    case Situation::Unrecognised: return "unrecognised";
    }
    return "?";
}

int RunSilent(App& a, const std::vector<std::wstring>& args, Out& out) {
    const std::wstring what = Flag(args, L"--silent");
    if (a.folder.empty()) { out.Line("a folder is needed: --folder <Lossless Scaling folder>"); return 2; }
    Refresh(a);
    if (what == L"status") {
        out.Line(std::string("situation: ") + SituationText(a.state.situation));
        out.Line("installed: " + (a.state.installedVersion.empty() ? std::string("-") : a.state.installedVersion));
        out.Line("payload: " + (a.payload.ok ? a.payload.version : std::string("none")));
        out.Line("advice: " + a.advice.headline);
        return 0;
    }
    if (what != L"install" && what != L"uninstall") { out.Line("usage: --silent status|install|uninstall"); return 2; }
    if (what == L"install" && !a.payload.ok) { out.Line("no files to install: " + (a.payloadError.empty() ? std::string("this setup carries none") : a.payloadError)); return 2; }
    const Result r = what == L"install" ? Install(a.folder, a.payloadDir) : Uninstall(a.folder, Has(args, L"--remove-addons"));
    for (const auto& l : r.log) out.Line("  " + l);
    out.Line(r.message);
    if (!r.backupDir.empty()) out.Line("backups: " + Narrow(r.backupDir));
    if (r.ok && what == L"install") RememberFolder(a.folder);
    return r.ok ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argvW, argvW + argc);
    LocalFree(argvW);

    App a;
    a.elevated = IsElevated();
    a.startTick = GetTickCount();
    a.icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    const bool silent = Has(args, L"--silent"), showVersion = Has(args, L"--version");
    const std::wstring testClose = Flag(args, L"--test-close-ms");
    if (!testClose.empty()) a.testCloseMs = _wtoi(testClose.c_str());
    a.shotDir = Flag(args, L"--shot");
    if (!testClose.empty() || !a.shotDir.empty() || Has(args, L"--no-remember"))
        UseRegistryKeyForTest(L"Software\\LSAddonManager\\SetupTest");   // tests must never touch the person's remembered folder

    Out out;
    const std::wstring logPath = Flag(args, L"--log");
    if (!logPath.empty()) _wfopen_s(&out.file, logPath.c_str(), L"wb");
    if ((silent || showVersion) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        out.console = true;
    }

    // The files to install: a folder given on the command line, or the bundle inside this exe (unpacked to a temporary folder)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring given = Flag(args, L"--payload");
    if (!given.empty()) {
        a.payloadDir = given;
    } else {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!EmbeddedPayload(data, size)) {
            a.payloadError = "This setup file carries no files (it was built without them).";
        } else {
            a.tempDir = JoinPath(tmp, L"LSAddonManagerSetup-" + std::to_wstring(GetCurrentProcessId()));
            std::string err;
            if (UnpackTo(data, size, JoinPath(a.tempDir, L"files"), err)) a.payloadDir = JoinPath(a.tempDir, L"files");
            else a.payloadError = err;
        }
    }
    if (!a.payloadDir.empty()) {
        a.payload = CheckPayload(a.payloadDir);
        if (!a.payload.ok) a.payloadError = a.payload.error;
    }

    int code = 0;
    if (showVersion) {
        out.Line(a.payload.ok ? "payload " + a.payload.version : "payload none: " + a.payloadError);
        code = a.payload.ok ? 0 : 2;
    } else if (silent) {
        a.folder = Flag(args, L"--folder");
        code = RunSilent(a, args, out);
    } else if (HANDLE once = CreateMutexW(nullptr, TRUE, (L"Local\\LSAddonManagerSetup" + Flag(args, L"--instance-name")).c_str()); GetLastError() == ERROR_ALREADY_EXISTS) {
        // A second window over the same folder could start a second install in the middle of the first: one Setup window at a time.
        if (a.testCloseMs == 0) MessageBoxW(nullptr, L"LS Addon Manager Setup is already open. Look for its window on the taskbar.", L"LS Addon Manager Setup", MB_OK | MB_ICONINFORMATION);
        code = 4;
        if (once) CloseHandle(once);
    } else {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        a.candidates = FindCandidates();
        a.folder = Flag(args, L"--folder");
        a.waitForClose = Has(args, L"--update-when-closed");
        a.restartAfter = Has(args, L"--restart");
        Page* first = nullptr;
        if (a.waitForClose && !a.folder.empty()) {   // the manager's update: the waiting page (it moves on at once if Lossless Scaling is not running)
            Refresh(a);
            first = &WaitingPage(a);
        } else if (!a.folder.empty() || a.candidates.size() == 1) {
            if (a.folder.empty()) a.folder = a.candidates[0].dir;
            Refresh(a);
            first = &MainPage(a);
        } else {
            first = &ChooserPage(a, L"");
        }
        int button = 0;
        const HRESULT hr = TaskDialogIndirect(&first->cfg, &button, nullptr, nullptr);
        if (FAILED(hr)) { code = 3; out.Line("the window could not be created: 0x" + std::to_string(static_cast<unsigned long>(hr))); }
        if (a.worker.joinable()) a.worker.join();
        CoUninitialize();
        if (once) { ReleaseMutex(once); CloseHandle(once); }
    }
    if (out.file) fclose(out.file);
    if (!a.tempDir.empty()) { std::error_code ec; fs::remove_all(a.tempDir, ec); }
    return code;
}
