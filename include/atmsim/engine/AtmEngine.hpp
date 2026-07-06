#pragma once
// ============================================================================
//  AtmEngine.hpp — многопоточное ядро банкомата (M3).
//
//  МОДЕЛЬ ПАРАЛЛЕЛИЗМА (§6.1) — «один писатель, много читателей»:
//    * поток обслуживания (run)        — ЕДИНСТВЕННЫЙ, кто меняет счета/кассу/
//                                         журнал/счётчики;
//    * поток прихода (generateArrivals) — добавляет клиентов в очередь;
//    * поток консоли                    — только читает снимки и шлёт команды.
//  Всё общее изменяемое состояние защищено одним std::shared_mutex: писатели
//  берут unique_lock (эксклюзивно, кратко), читатели — shared_lock (параллельно).
//  Состояние-режим (state_) — atomic, чтобы быстро проверять его в предикатах.
// ============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <shared_mutex>
#include <stop_token>
#include <vector>

#include "atmsim/config/Config.hpp"
#include "atmsim/core/Account.hpp"
#include "atmsim/core/Cashbox.hpp"
#include "atmsim/core/Client.hpp"
#include "atmsim/engine/ServiceTimeProvider.hpp"
#include "atmsim/engine/Snapshots.hpp"
#include "atmsim/reporting/Logger.hpp"

namespace atmsim {

/// \brief Многопоточное ядро банкомата: поток обслуживания + поток прихода,
///        потокобезопасные снимки для отчётов, прерываемое ожидание (§6.2).
/// \note Всё изменяемое состояние мутирует только поток обслуживания (§6.1);
///       читатели работают через const-снимки под shared_lock.
class AtmEngine {
public:
    // logger — необязательный технический лог (§10); nullptr = не логировать.
    explicit AtmEngine(const Config& cfg, Logger* logger = nullptr);

    // --- Потоковые функции (каждая крутится в своём std::jthread) ------------
    void run(std::stop_token stopToken);              // цикл обслуживания
    void generateArrivals(std::stop_token stopToken); // цикл прихода клиентов

    // --- Команды администратора (потокобезопасны, вызывающего НЕ блокируют) --
    void requestPause();
    void requestResume();
    void requestStop();
    // Техобслуживание (§4.5). durationSeconds — длительность в МОДЕЛЬНЫХ секундах;
    // если не задано — берётся default_duration_seconds из конфига; значение <= 0
    // означает «до явной команды maintenance stop».
    void requestMaintenance(std::optional<int> durationSeconds);
    void endMaintenance();

    // --- Снимки и отчёты (потокобезопасное чтение под shared_lock) -----------
    AtmSnapshot snapshot() const;
    std::vector<ClientSnapshot> queueSnapshot() const;
    StatsSnapshot statsSnapshot() const;                       // команда stats
    std::optional<ClientReport> clientReport(ClientId id) const; // команда client
    std::optional<Money> balanceOf(ClientId id) const;         // команда balance
    std::vector<OperationRecord> operations(const OperationFilter& filter) const; // operations

    // Сумма балансов всех счетов — для проверки инварианта денег и статистики.
    Money accountsTotal() const;

    bool isStopped() const { return state_.load() == AtmState::Stopped; }

private:
    // Вспомогательные (вызываются строго из потока прихода / под локом).
    Client makeClient();                              // только поток прихода
    double modelSecondsWaited(const Client& c) const; // сколько модельных сек. ждёт
    // Решение «уйти/остаться» для всех в очереди при старте ТО (§4.5). Вызывается
    // из потока обслуживания, ПОД уже захваченным mutex_ (потому «Locked»).
    void applyMaintenanceRenegingLocked();

    Config cfg_;

    // Один мьютекс на всё изменяемое общее состояние. shared_mutex позволяет
    // множеству читателей-снимков работать параллельно, не мешая друг другу.
    mutable std::shared_mutex mutex_;

    // Режим работы — атомарный: его читают в предикатах ожидания и в снимках.
    std::atomic<AtmState> state_{AtmState::Idle};

    // condition_variable_any (а не condition_variable) — потому что она умеет
    // работать с shared_mutex. На ней поток обслуживания «дремлет с условием»
    // время обслуживания (см. §6.2 и run()).
    //
    // ВАЖНО (совместимость с MSVC): перегрузки wait*/wait_for/wait_until,
    // принимающие std::stop_token, здесь СОЗНАТЕЛЬНО НЕ используются — в ряде
    // версий MSVC STL их внутренняя механика падает в рантайме («unlock of
    // unowned mutex», порча стека). Вместо них: обычные перегрузки с предикатом,
    // который явно проверяет stopToken.stop_requested(), плюс std::stop_callback
    // в run()/generateArrivals(), будящий wakeUp_ при запросе остановки.
    // Поведение эквивалентно: stop будит поток мгновенно (§6.2).
    std::condition_variable_any wakeUp_;

    std::deque<Client> queue_;         // очередь ожидающих (защищена mutex_)
    std::vector<Account> accounts_;    // по счёту на клиента (защищены mutex_)
    Cashbox cashbox_;                  // касса (защищена mutex_)

    std::unique_ptr<ServiceTimeProvider> serviceProvider_;

    // ВАЖНО про RNG: у std::mt19937_64 нет внутренней синхронизации, поэтому
    // ОДИН движок из двух потоков — это гонка данных (§6.3). Держим ДВА
    // отдельных движка: один только для потока обслуживания, другой только для
    // потока прихода. Пересекаться они не будут.
    std::mt19937_64 serviceRng_;       // только поток обслуживания
    std::mt19937_64 arrivalRng_;       // только поток прихода

    std::optional<Client> currentClient_;  // кого обслуживаем сейчас (защищён mutex_)
    std::optional<OperationType> lastCashMove_;  // направление посл. движения кассы
    std::uint64_t totalServed_{0};
    std::uint64_t totalLeft_{0};              // всего ушли (терпение + ТО)
    std::uint64_t totalLeftMaintenance_{0};   // из них — ушли из-за ТО
    std::size_t maxQueueLen_{0};

    // Режим ТО (§4.5): дедлайн авто-завершения и одноразовый флаг «применить
    // решение уйти/остаться к очереди». Оба под mutex_.
    std::chrono::steady_clock::time_point maintenanceDeadline_{};
    bool maintenanceRenegePending_{false};

    // Отчётность (всё под mutex_): журнал операций и реестр ВСЕХ клиентов
    // (чтобы отвечать на client <id> даже после того, как клиент ушёл/обслужен).
    std::vector<OperationRecord> log_;
    std::vector<Client> roster_;       // roster_[id-1] — клиент с этим id
    std::uint64_t nextOperationId_{1}; // только поток обслуживания

    // Аккумуляторы статистики (модельные секунды), пишет поток обслуживания.
    double sumWaitModel_{0.0};
    double sumServiceModel_{0.0};
    double busyModel_{0.0};
    std::chrono::steady_clock::time_point startTime_;  // момент старта (для аптайма)

    int generatedCount_{0};            // только поток прихода
    ClientId nextClientId_{1};         // только поток прихода

    Logger* logger_{nullptr};          // технический лог (может быть nullptr)
    bool lowCashLogged_{false};        // предупреждение о низкой кассе — один раз
};

}  // namespace atmsim
