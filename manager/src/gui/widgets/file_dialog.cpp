#include "file_dialog.h"
#include <windows.h>
#include <shobjidl.h>
#include <cwchar>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace eam {
namespace widgets {

static bool Pick(bool save, const wchar_t* title, const wchar_t* defaultName, const wchar_t* filterName, const wchar_t* filterSpec, std::wstring& out) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IFileDialog* dlg = nullptr;
    const HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))
                            : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0; dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
        if (title) dlg->SetTitle(title);
        if (filterSpec) { COMDLG_FILTERSPEC f[2] = { { filterName ? filterName : L"Files", filterSpec }, { L"All files", L"*.*" } }; dlg->SetFileTypes(2, f); }
        if (save && defaultName) dlg->SetFileName(defaultName);
        if (save && filterSpec && filterSpec[0] == L'*' && filterSpec[1] == L'.' && filterSpec[2] && !wcspbrk(filterSpec + 2, L"*?;")) dlg->SetDefaultExtension(filterSpec + 2);   // "*.json" gives "json"
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); ok = true; }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

bool PickOpenFile(const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec, std::wstring& out) { return Pick(false, title, nullptr, filterName, filterSpec, out); }
bool PickSaveFile(const wchar_t* title, const wchar_t* defaultName, const wchar_t* filterName, const wchar_t* filterSpec, std::wstring& out) { return Pick(true, title, defaultName, filterName, filterSpec, out); }

} // namespace widgets
} // namespace eam
