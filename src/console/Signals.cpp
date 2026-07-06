#include "atmsim/console/Signals.hpp"

#include <csignal>

namespace atmsim {
namespace {

// volatile sig_atomic_t — единственный тип, который безопасно менять прямо в
// обработчике сигнала (async-signal-safe). Никакой сложной логики в обработчике
// быть не должно.
volatile std::sig_atomic_t g_shutdownFlag = 0;

extern "C" void onSignal(int /*sig*/) {
    g_shutdownFlag = 1;
}

}  // namespace

void installSignalHandlers() {
#ifdef _WIN32
    // На Windows обработчик Ctrl+C ставится через std::signal.
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
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
