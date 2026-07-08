#pragma once
// ============================================================================
//  ScopedEngineThreads.hpp — RAII-владелец рабочих потоков движка.
//
//  Конструктор запускает поток обслуживания (AtmEngine::run) и поток прихода
//  клиентов (AtmEngine::generateArrivals). stop() идемпотентно останавливает их
//  (requestStop) и присоединяет (join); деструктор гарантированно зовёт stop().
//
//  Зачем отдельная сущность: раньше main держал engineThread/arrivalThread
//  «голыми» std::thread. При исключении (например из AdminConsole::run())
//  деструктор JOINABLE-потока зовёт std::terminate РАНЬШЕ, чем управление дойдёт
//  до catch, — процесс падает в обход всей обработки ошибок в main. RAII делает
//  остановку+join безусловной: на «счастливом» пути вызывающий зовёт stop() явно
//  (чтобы снять финальные снимки уже ПОСЛЕ остановки потоков — для
//  детерминированного итога, §5), а на пути с исключением стоп+join делает
//  деструктор при раскрутке стека.
// ============================================================================
#include <thread>

#include "atmsim/engine/AtmEngine.hpp"

namespace atmsim {

class ScopedEngineThreads {
public:
    // Запускает оба потока движка. Порядок как в исходном main: сперва поток
    // обслуживания, затем поток прихода. engine должен пережить этот объект.
    explicit ScopedEngineThreads(AtmEngine& engine)
        : engine_(engine),
          engineThread_([&engine] { engine.run(); }),
          arrivalThread_([&engine] { engine.generateArrivals(); }) {}

    // Плавно останавливает потоки и присоединяет их. Идемпотентно: повторный
    // вызов (в т.ч. из деструктора после явного stop) — no-op.
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        engine_.requestStop();  // переводит потоки в Stopped и будит их
        if (engineThread_.joinable())  engineThread_.join();
        if (arrivalThread_.joinable()) arrivalThread_.join();
    }

    ~ScopedEngineThreads() { stop(); }

    ScopedEngineThreads(const ScopedEngineThreads&) = delete;             // владеет потоками —
    ScopedEngineThreads& operator=(const ScopedEngineThreads&) = delete;  // не копируется

private:
    AtmEngine& engine_;
    std::thread engineThread_;
    std::thread arrivalThread_;
    bool stopped_ = false;
};

}  // namespace atmsim
