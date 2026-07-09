#pragma once
#ifdef _WIN32
#include <windows.h>
struct ConsoleUtf8AutoSetup {
    ConsoleUtf8AutoSetup() { SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8); }
};
inline ConsoleUtf8AutoSetup g_consoleUtf8AutoSetup;
#endif