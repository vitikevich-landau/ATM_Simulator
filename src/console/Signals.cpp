#include "atmsim/console/Signals.hpp"

#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

namespace atmsim {
namespace {

// volatile sig_atomic_t — единственный тип, который безопасно менять прямо в
// обработчике сигнала / консольного события (async-signal-safe). Никакой сложной
// логики в обработчике быть не должно.
volatile std::sig_atomic_t g_shutdownFlag = 0;

#ifdef _WIN32
// Настоящий (не псевдо-) хэндл главного потока: по нему из потока обработчика
// прерываем зависшее синхронное чтение stdin (getline), чтобы Ctrl+C не ждал
// Enter. GetCurrentThread() возвращает псевдо-хэндл, годный только в своём
// потоке, поэтому дублируем его в настоящий на этапе установки (в главном потоке).
HANDLE g_mainThread = nullptr;

// ОС вызывает этот обработчик в ОТДЕЛЬНОМ потоке. Делаем минимум: взводим флаг и
// отменяем блокирующее чтение главного потока. Возврат TRUE = «событие
// обработано», чтобы CRT не завершил процесс по умолчанию, а main остановился
// плавно (§4.6).
BOOL WINAPI onConsoleCtrl(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        g_shutdownFlag = 1;
        if (g_mainThread) ::CancelSynchronousIo(g_mainThread);
        return TRUE;
    }
    return FALSE;  // прочие события (закрытие окна и т.п.) — обработчику по умолчанию
}
#else
extern "C" void onSignal(int /*sig*/) {
    g_shutdownFlag = 1;
}
#endif

}  // namespace

void installSignalHandlers() {
#ifdef _WIN32
    // std::signal(SIGINT) на Windows лишь взводил флаг, но НЕ прерывал getline:
    // Ctrl+C «застревал» до следующего Enter, и §4.6 фактически не работал.
    // SetConsoleCtrlHandler + запомненный хэндл главного потока позволяют из
    // обработчика разблокировать его чтение stdin (см. onConsoleCtrl). ВАЖНО:
    // это лечит командный режим (getline). В live-режиме ключи читаются через
    // _getch (conio) — он так не прерывается; там плавный выход по Ctrl+C
    // происходит при следующем нажатии клавиши (опрос флага в AdminConsole).
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                      ::GetCurrentProcess(), &g_mainThread,
                      0, FALSE, DUPLICATE_SAME_ACCESS);
    ::SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
    // На POSIX используем sigaction БЕЗ SA_RESTART: тогда блокирующий read()
    // внутри getline прерывается сигналом (EINTR), а не перезапускается — и
    // консоль возвращает управление main для плавной остановки.
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
#endif
}

bool shutdownRequested() {
    return g_shutdownFlag != 0;
}

}  // namespace atmsim
