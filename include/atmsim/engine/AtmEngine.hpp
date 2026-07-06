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

namespace atmsim {

class AtmEngine {
public:
    explicit AtmEngine(const Config& cfg);

    // --- Потоковые функции (каждая крутится в своём std::jthread) ------------
    void run(std::stop_token stopToken);              // цикл обслуживания
    void generateArrivals(std::stop_token stopToken); // цикл прихода клиентов

    // --- Команды администратора (потокобезопасны, вызывающего НЕ блокируют) --
    void requestPause();
    void requestResume();
    void requestStop();

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

    Config cfg_;

    // Один мьютекс на всё изменяемое общее состояние. shared_mutex позволяет
    // множеству читателей-снимков работать параллельно, не мешая друг другу.
    mutable std::shared_mutex mutex_;

    // Режим работы — атомарный: его читают в предикатах ожидания и в снимках.
    std::atomic<AtmState> state_{AtmState::Idle};

    // condition_variable_any (а не condition_variable) — потому что она умеет
    // работать с shared_mutex и со stop_token (C++20). На ней поток обслуживания
    // «дремлет с условием» время обслуживания (см. §6.2 и run()).
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
    std::uint64_t totalServed_{0};
    std::uint64_t totalLeft_{0};
    std::size_t maxQueueLen_{0};

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
};

}  // namespace atmsim
