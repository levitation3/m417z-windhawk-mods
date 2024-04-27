// ==WindhawkMod==
// @id              icon-resource-redirect
// @name            Icon Resource Redirect
// @description     Define alternative resource files for loading icons (e.g. instead of imageres.dll) for simple theming without having to modify system files
// @version         1.0.5
// @author          m417z
// @github          https://github.com/m417z
// @twitter         https://twitter.com/m417z
// @homepage        https://m417z.com/
// @include         *
// @compilerOptions -lshlwapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Icon Resource Redirect

Define alternative resource files for loading icons (e.g. instead of
imageres.dll) for simple theming without having to modify system files.

## Theme folder

A theme folder can be selected in the settings. It's a folder with alternative
resource files, and the `theme.ini` file that contains redirection rules. For
example, the `theme.ini` file may contain the following:

```
[redirections]
%SystemRoot%\explorer.exe=explorer.exe
%SystemRoot%\system32\imageres.dll=imageres.dll
```

In this case, the folder must also contain the `explorer.exe`, `imageres.dll`
files which will be used as the custom resource files.

## Supported resource types and loading methods

The mod started with icon redirection, but was extended with time, and now it
supports the following resource types and loading methods:

* Icons extracted with the `PrivateExtractIconsW` function.
* Icons, cursors and bitmaps loaded with the `LoadImageW` function.
* Strings loaded with the `LoadStringW` function.
* GDI+ images (e.g. PNGs) loaded with the `SHCreateStreamOnModuleResourceW`
  function.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- themeFolder: ''
  $name: Theme folder
  $description: A folder with alternative resource files and theme.ini
- redirectionResourcePaths:
  - - original: '%SystemRoot%\System32\imageres.dll'
      $name: The original resource file
      $description: The original file from which icons are loaded
    - redirect: 'C:\my-themes\theme-1\imageres.dll'
      $name: The custom resource file
      $description: The custom resource file that will be used instead
  $name: Redirection resource paths
*/
// ==/WindhawkModSettings==

#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

std::shared_mutex g_redirectionResourcePathsMutex;
std::unordered_map<std::wstring, std::vector<std::wstring>>
    g_redirectionResourcePaths;

std::shared_mutex g_redirectionResourceModulesMutex;
std::unordered_map<std::wstring, HMODULE> g_redirectionResourceModules;

bool DevicePathToDosPath(const WCHAR* device_path,
                         WCHAR* dos_path,
                         size_t dos_path_size) {
    WCHAR drive_strings[MAX_PATH];
    if (!GetLogicalDriveStrings(ARRAYSIZE(drive_strings), drive_strings)) {
        return false;
    }

    // Drive strings are stored as a set of null terminated strings, with an
    // extra null after the last string. Each drive string is of the form "C:\".
    // We convert it to the form "C:", which is the format expected by
    // QueryDosDevice().
    WCHAR drive_colon[3] = L" :";
    for (const WCHAR* next_drive_letter = drive_strings; *next_drive_letter;
         next_drive_letter += wcslen(next_drive_letter) + 1) {
        // Dos device of the form "C:".
        *drive_colon = *next_drive_letter;
        WCHAR device_name[MAX_PATH];
        if (!QueryDosDevice(drive_colon, device_name, ARRAYSIZE(device_name))) {
            continue;
        }

        size_t name_length = wcslen(device_name);
        if (_wcsnicmp(device_path, device_name, name_length) == 0) {
            size_t dos_path_size_required =
                2 + wcslen(device_path + name_length) + 1;
            if (dos_path_size < dos_path_size_required) {
                return false;
            }

            // Construct DOS path.
            wcscpy(dos_path, drive_colon);
            wcscat(dos_path, device_path + name_length);
            return true;
        }
    }

    return false;
}

HMODULE GetRedirectedModule(std::wstring_view fileName) {
    std::wstring fileNameUpper{fileName};
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &fileNameUpper[0],
                  static_cast<int>(fileNameUpper.length()), &fileNameUpper[0],
                  static_cast<int>(fileNameUpper.length()), nullptr, nullptr,
                  0);

    {
        std::shared_lock lock{g_redirectionResourceModulesMutex};
        const auto it = g_redirectionResourceModules.find(fileNameUpper);
        if (it != g_redirectionResourceModules.end()) {
            return it->second;
        }
    }

    HINSTANCE module = LoadLibraryEx(
        fileNameUpper.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        DWORD dwError = GetLastError();
        Wh_Log(L"LoadLibraryEx failed with error %u", dwError);
        return nullptr;
    }

    {
        std::unique_lock lock{g_redirectionResourceModulesMutex};
        g_redirectionResourceModules.try_emplace(fileNameUpper, module);
    }

    return module;
}

void FreeAndClearRedirectedModules() {
    std::unordered_map<std::wstring, HMODULE> modules;

    {
        std::unique_lock lock{g_redirectionResourceModulesMutex};
        modules.swap(g_redirectionResourceModules);
    }

    for (const auto& it : modules) {
        FreeLibrary(it.second);
    }
}

using PrivateExtractIconsW_t = decltype(&PrivateExtractIconsW);
PrivateExtractIconsW_t PrivateExtractIconsW_Original;
UINT WINAPI PrivateExtractIconsW_Hook(LPCWSTR szFileName,
                                      int nIconIndex,
                                      int cxIcon,
                                      int cyIcon,
                                      HICON* phicon,
                                      UINT* piconid,
                                      UINT nIcons,
                                      UINT flags) {
    Wh_Log(L"> szFileName: %s", szFileName);

    std::wstring fileNameUpper{szFileName};
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &fileNameUpper[0],
                  static_cast<int>(fileNameUpper.length()), &fileNameUpper[0],
                  static_cast<int>(fileNameUpper.length()), nullptr, nullptr,
                  0);

    bool triedRedirection = false;

    {
        std::shared_lock lock{g_redirectionResourcePathsMutex};

        if (const auto it = g_redirectionResourcePaths.find(fileNameUpper);
            it != g_redirectionResourcePaths.end()) {
            const auto& redirects = it->second;
            for (const auto& redirect : redirects) {
                if (!triedRedirection) {
                    Wh_Log(L"nIconIndex: %d", nIconIndex);
                    Wh_Log(L"cxIcon: %d", cxIcon);
                    Wh_Log(L"cyIcon: %d", cyIcon);
                    Wh_Log(L"phicon: %d", !!phicon);
                    Wh_Log(L"piconid: %d", !!piconid);
                    Wh_Log(L"nIcons: %u", nIcons);
                    Wh_Log(L"flags: 0x%08X", flags);
                    triedRedirection = true;
                }

                Wh_Log(L"Trying %s", redirect.c_str());

                UINT result = PrivateExtractIconsW_Original(
                    redirect.c_str(), nIconIndex, cxIcon, cyIcon, phicon,
                    piconid, nIcons, flags);
                if (result != 0xFFFFFFFF && result != 0) {
                    Wh_Log(L"Redirected successfully, result: %u", result);
                    return result;
                }

                Wh_Log(L"Redirection failed, result: %u", result);
            }
        }
    }

    if (triedRedirection) {
        Wh_Log(L"No redirection succeeded, falling back to original");
    }

    return PrivateExtractIconsW_Original(szFileName, nIconIndex, cxIcon, cyIcon,
                                         phicon, piconid, nIcons, flags);
}

bool RedirectModule(HINSTANCE hInstance,
                    std::function<void()> beforeFirstRedirectionFunction,
                    std::function<bool(HINSTANCE)> redirectFunction) {
    WCHAR szFileName[MAX_PATH];
    DWORD fileNameLen;
    if ((ULONG_PTR)hInstance & 3) {
        WCHAR szNtFileName[MAX_PATH * 2];

        if (!GetMappedFileName(GetCurrentProcess(), (void*)hInstance,
                               szNtFileName, ARRAYSIZE(szNtFileName))) {
            DWORD dwError = GetLastError();
            Wh_Log(L"> GetMappedFileName(%p) failed with error %u", hInstance,
                   dwError);
            return false;
        }

        if (!DevicePathToDosPath(szNtFileName, szFileName,
                                 ARRAYSIZE(szFileName))) {
            Wh_Log(L"> DevicePathToDosPath failed");
            return false;
        }

        fileNameLen = wcslen(szFileName);
    } else {
        fileNameLen =
            GetModuleFileName(hInstance, szFileName, ARRAYSIZE(szFileName));
        switch (fileNameLen) {
            case 0: {
                DWORD dwError = GetLastError();
                Wh_Log(L"> GetModuleFileName(%p) failed with error %u",
                       hInstance, dwError);
                return false;
            }

            case ARRAYSIZE(szFileName):
                Wh_Log(L"> GetModuleFileName(%p) failed, name too long",
                       hInstance);
                return false;
        }
    }

    Wh_Log(L"> Module: %s", szFileName);

    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &szFileName[0],
                  fileNameLen, &szFileName[0], fileNameLen, nullptr, nullptr,
                  0);

    bool triedRedirection = false;

    {
        std::shared_lock lock{g_redirectionResourcePathsMutex};

        if (const auto it = g_redirectionResourcePaths.find(szFileName);
            it != g_redirectionResourcePaths.end()) {
            const auto& redirects = it->second;
            for (const auto& redirect : redirects) {
                if (!triedRedirection) {
                    beforeFirstRedirectionFunction();
                    triedRedirection = true;
                }

                Wh_Log(L"Trying %s", redirect.c_str());

                HINSTANCE hInstanceRedirect = GetRedirectedModule(redirect);
                if (!hInstanceRedirect) {
                    Wh_Log(L"GetRedirectedModule failed");
                    continue;
                }

                if (redirectFunction(hInstanceRedirect)) {
                    return true;
                }
            }
        }
    }

    if (triedRedirection) {
        Wh_Log(L"No redirection succeeded, falling back to original");
    }

    return false;
}

using LoadImageW_t = decltype(&LoadImageW);
LoadImageW_t LoadImageW_Original;
HANDLE WINAPI LoadImageW_Hook(HINSTANCE hInst,
                              LPCWSTR name,
                              UINT type,
                              int cx,
                              int cy,
                              UINT fuLoad) {
    Wh_Log(L">");

    HANDLE result;

    bool redirected = RedirectModule(
        hInst,
        [&]() {
            switch (type) {
                case IMAGE_BITMAP:
                    Wh_Log(L"Resource type: Bitmap");
                    break;
                case IMAGE_ICON:
                    Wh_Log(L"Resource type: Icon");
                    break;
                case IMAGE_CURSOR:
                    Wh_Log(L"Resource type: Cursor");
                    break;
                default:
                    Wh_Log(L"Resource type: %u", type);
                    break;
            }

            if (IS_INTRESOURCE(name)) {
                Wh_Log(L"Resource number: %u", (DWORD)(ULONG_PTR)name);
            } else {
                Wh_Log(L"Resource name: %s", name);
            }

            Wh_Log(L"Width: %d", cx);
            Wh_Log(L"Height: %d", cy);
            Wh_Log(L"Flags: 0x%08X", fuLoad);
        },
        [&](HINSTANCE hInstanceRedirect) {
            result = LoadImageW_Original(hInstanceRedirect, name, type, cx, cy,
                                         fuLoad);
            if (result) {
                Wh_Log(L"Redirected successfully");
                return true;
            }

            DWORD dwError = GetLastError();
            Wh_Log(L"LoadImageW failed with error %u", dwError);
            return false;
        });
    if (redirected) {
        return result;
    }

    return LoadImageW_Original(hInst, name, type, cx, cy, fuLoad);
}

using LoadStringBaseExW_t = int(WINAPI*)(HINSTANCE hInstance,
                                         UINT uID,
                                         LPWSTR lpBuffer,
                                         int cchBufferMax,
                                         WORD wLanguage);
LoadStringBaseExW_t LoadStringBaseExW_Original;
int WINAPI LoadStringBaseExW_Hook(HINSTANCE hInstance,
                                  UINT uID,
                                  LPWSTR lpBuffer,
                                  int cchBufferMax,
                                  WORD wLanguage) {
    Wh_Log(L">");

    int result;

    bool redirected = RedirectModule(
        hInstance,
        [&]() {
            Wh_Log(L"String number: %u", uID);
            Wh_Log(L"String language: %04X", wLanguage);
        },
        [&](HINSTANCE hInstanceRedirect) {
            result = LoadStringBaseExW_Original(
                hInstanceRedirect, uID, lpBuffer, cchBufferMax, wLanguage);
            if (result) {
                Wh_Log(L"Redirected successfully");
                return true;
            }

            DWORD dwError = GetLastError();
            Wh_Log(L"LoadStringBaseExW failed with error %u", dwError);
            return false;
        });
    if (redirected) {
        return result;
    }

    return LoadStringBaseExW_Original(hInstance, uID, lpBuffer, cchBufferMax,
                                      wLanguage);
}

using SHCreateStreamOnModuleResourceW_t = HRESULT(WINAPI*)(HMODULE hModule,
                                                           LPCWSTR pwszName,
                                                           LPCWSTR pwszType,
                                                           IStream** ppStream);
SHCreateStreamOnModuleResourceW_t SHCreateStreamOnModuleResourceW_Original;
HRESULT WINAPI SHCreateStreamOnModuleResourceW_Hook(HMODULE hModule,
                                                    LPCWSTR pwszName,
                                                    LPCWSTR pwszType,
                                                    IStream** ppStream) {
    Wh_Log(L">");

    HRESULT result;

    bool redirected = RedirectModule(
        hModule,
        [&]() {
            if (IS_INTRESOURCE(pwszType)) {
                Wh_Log(L"Resource type: %u", (DWORD)(ULONG_PTR)pwszType);
            } else {
                Wh_Log(L"Resource type: %s", pwszType);
            }

            if (IS_INTRESOURCE(pwszName)) {
                Wh_Log(L"Resource number: %u", (DWORD)(ULONG_PTR)pwszName);
            } else {
                Wh_Log(L"Resource name: %s", pwszName);
            }
        },
        [&](HINSTANCE hInstanceRedirect) {
            result = SHCreateStreamOnModuleResourceW_Original(
                hInstanceRedirect, pwszName, pwszType, ppStream);
            if (SUCCEEDED(result)) {
                Wh_Log(L"Redirected successfully");
                return true;
            }

            Wh_Log(L"SHCreateStreamOnModuleResourceW failed with error %08X",
                   result);
            return false;
        });
    if (redirected) {
        return result;
    }

    return SHCreateStreamOnModuleResourceW_Original(hModule, pwszName, pwszType,
                                                    ppStream);
}

void LoadSettings() {
    std::unordered_map<std::wstring, std::vector<std::wstring>> paths;

    auto addRedirectionPath = [&paths](PCWSTR original, PCWSTR redirect) {
        WCHAR originalExpanded[MAX_PATH];
        DWORD originalExpandedLen = ExpandEnvironmentStrings(
            original, originalExpanded, ARRAYSIZE(originalExpanded));
        if (!originalExpandedLen ||
            originalExpandedLen > ARRAYSIZE(originalExpanded)) {
            Wh_Log(L"Failed to expand path: %s", original);
            return;
        }

        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE,
                      originalExpanded, originalExpandedLen - 1,
                      originalExpanded, originalExpandedLen - 1, nullptr,
                      nullptr, 0);

        Wh_Log(L"Loaded %s->%s", originalExpanded, redirect);

        paths[originalExpanded].push_back(redirect);
    };

    PCWSTR themeFolder = Wh_GetStringSetting(L"themeFolder");

    if (*themeFolder) {
        WCHAR themeIniFile[MAX_PATH];
        if (PathCombine(themeIniFile, themeFolder, L"theme.ini")) {
            std::wstring data(32768, L'\0');
            DWORD result = GetPrivateProfileSection(
                L"redirections", data.data(), data.size(), themeIniFile);
            if (result != data.size() - 2) {
                for (auto* p = data.data(); *p;) {
                    auto* pNext = p + wcslen(p) + 1;
                    auto* pEq = wcschr(p, L'=');
                    if (pEq) {
                        *pEq = L'\0';

                        WCHAR redirectFile[MAX_PATH];
                        if (PathCombine(redirectFile, themeFolder, pEq + 1)) {
                            addRedirectionPath(p, redirectFile);
                        }
                    } else {
                        Wh_Log(L"Skipping %s", p);
                    }

                    p = pNext;
                }
            } else {
                Wh_Log(L"Failed to read theme file");
            }
        }
    }

    Wh_FreeStringSetting(themeFolder);

    for (int i = 0;; i++) {
        PCWSTR original =
            Wh_GetStringSetting(L"redirectionResourcePaths[%d].original", i);
        PCWSTR redirect =
            Wh_GetStringSetting(L"redirectionResourcePaths[%d].redirect", i);

        bool hasRedirection = *original || *redirect;

        if (hasRedirection) {
            addRedirectionPath(original, redirect);
        }

        Wh_FreeStringSetting(original);
        Wh_FreeStringSetting(redirect);

        if (!hasRedirection) {
            break;
        }
    }

    std::unique_lock lock{g_redirectionResourcePathsMutex};
    g_redirectionResourcePaths = std::move(paths);
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    Wh_SetFunctionHook((void*)PrivateExtractIconsW,
                       (void*)PrivateExtractIconsW_Hook,
                       (void**)&PrivateExtractIconsW_Original);

    Wh_SetFunctionHook((void*)LoadImageW, (void*)LoadImageW_Hook,
                       (void**)&LoadImageW_Original);

    HMODULE kernelBaseModule = LoadLibrary(L"kernelbase.dll");
    if (kernelBaseModule) {
        FARPROC pLoadStringBaseExW =
            GetProcAddress(kernelBaseModule, "LoadStringBaseExW");
        if (pLoadStringBaseExW) {
            Wh_SetFunctionHook((void*)pLoadStringBaseExW,
                               (void*)LoadStringBaseExW_Hook,
                               (void**)&LoadStringBaseExW_Original);
        } else {
            Wh_Log(L"Couldn't find LoadStringBaseExW");
        }
    } else {
        Wh_Log(L"Couldn't load kernelbase.dll");
    }

    HMODULE shcoreModule = LoadLibrary(L"shcore.dll");
    if (shcoreModule) {
        FARPROC pSHCreateStreamOnModuleResourceW =
            GetProcAddress(shcoreModule, (PCSTR)109);
        if (pSHCreateStreamOnModuleResourceW) {
            Wh_SetFunctionHook(
                (void*)pSHCreateStreamOnModuleResourceW,
                (void*)SHCreateStreamOnModuleResourceW_Hook,
                (void**)&SHCreateStreamOnModuleResourceW_Original);
        } else {
            Wh_Log(L"Couldn't find SHCreateStreamOnModuleResourceW (#109)");
        }
    } else {
        Wh_Log(L"Couldn't load shcore.dll");
    }

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L">");

    FreeAndClearRedirectedModules();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    FreeAndClearRedirectedModules();

    // Invalidate icon cache.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
