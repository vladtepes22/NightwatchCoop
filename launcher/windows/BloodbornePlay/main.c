/* Bloodborne.exe: starts the game straight away with the launcher's saved settings
 * ("Nightwatch Co-op.exe" --play, beside this file; "Lantern Engine.exe" before v0.2.0 beta 8) and waits for it, so a shortcut or Steam
 * (as a non-Steam game) sees the game running until it ends. Arguments are passed on. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t folder[32768];
    const DWORD n = GetModuleFileNameW(NULL, folder, sizeof(folder) / sizeof(folder[0]));
    if (n == 0 || n >= sizeof(folder) / sizeof(folder[0])) return 1;
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) slash[1] = 0;

    static wchar_t launcher[32768], line[32768 + 1024];
    if (_snwprintf(launcher, sizeof(launcher) / sizeof(launcher[0]), L"%lsNightwatch Co-op.exe", folder) < 0)
        return 1;
    if (GetFileAttributesW(launcher) == INVALID_FILE_ATTRIBUTES &&
        _snwprintf(launcher, sizeof(launcher) / sizeof(launcher[0]), L"%lsLantern Engine.exe", folder) < 0)
        return 1;
    if (_snwprintf(line, sizeof(line) / sizeof(line[0]), L"\"%ls\" --play %ls", launcher,
                   arguments ? arguments : L"") < 0)
        return 1;

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    if (!CreateProcessW(launcher, line, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process)) {
        MessageBoxW(NULL, L"Nightwatch Co-op.exe was not found beside Bloodborne.exe.\n\n"
                          L"Keep Bloodborne.exe in the Nightwatch Co-op folder.",
                    L"Nightwatch Co-op", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
