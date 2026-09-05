#include "desktop/ShortcutResolver.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <wrl/client.h>

ShortcutInfo ShortcutResolver::Resolve(const std::wstring& shortcutPath) const {
    ShortcutInfo info;

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&shellLink)))) {
        return info;
    }

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile))) {
        return info;
    }

    if (FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
        return info;
    }

    wchar_t target[MAX_PATH]{};
    WIN32_FIND_DATAW findData{};
    if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, &findData, SLGP_UNCPRIORITY))) {
        info.targetPath = target;
    }

    wchar_t arguments[INFOTIPSIZE]{};
    if (SUCCEEDED(shellLink->GetArguments(arguments, INFOTIPSIZE))) {
        info.arguments = arguments;
    }

    wchar_t workingDirectory[MAX_PATH]{};
    if (SUCCEEDED(shellLink->GetWorkingDirectory(workingDirectory, MAX_PATH))) {
        info.workingDirectory = workingDirectory;
    }

    return info;
}
