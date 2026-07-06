#pragma once
// ============================================================================
//  Snapshots.hpp — DTO-снимки состояния для отчётов (§6.3).
//
//  Ключевая идея: снимки НАМЕРЕННО не содержат mutex/atomic и вообще ничего
//  «живого». Поток консоли получает КОПИЮ данных, снятую под кратким локом, и
//  дальше работает с ней сколько угодно, не мешая потоку обслуживания. Это и
//  есть основа неблокирующего мониторинга (§4.3).
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atmsim/core/Operation.hpp"  // OperationRecord
#include "atmsim/core/Types.hpp"

namespace atmsim {

// Снимок состояния банкомата целиком (команда status / atm / будущий дашборд).
struct AtmSnapshot {
    AtmState state{AtmState::Idle};
    Money cashboxBalance{0};
    std::optional<ClientId> currentClientId;          // кого обслуживают сейчас
    std::optional<OperationType> currentOperation;    // и какую операцию
    std::uint64_t totalServed{0};
    std::uint64_t totalLeft{0};                       // ушли по терпению
    std::size_t queueLength{0};
    std::size_t maxQueueLength{0};
    double uptimeSeconds{0.0};                        // аптайм в модельных секундах
    bool lowCash{false};                              // касса ниже порога инкассации
    // Остаток режима ТО в модельных секундах: 0 — не в ТО; -1 — ТО до явной
    // команды maintenance stop; >0 — сколько ещё осталось.
    double maintenanceEtaSeconds{0.0};
    // Направление последней УСПЕШНОЙ операции, изменившей кассу: Deposit (внесли)
    // или Withdraw (сняли). Нужно, чтобы подсвечивать сумму кассы: зелёным после
    // внесения, красным после снятия. nullopt — движений кассы ещё не было.
    std::optional<OperationType> lastCashMove;
};

// Снимок одного ожидающего клиента (команда queue / дашборд).
struct ClientSnapshot {
    ClientId id{};
    OperationType requestedOperation{OperationType::CheckBalance};
    Money amount{0};
    double waitedSeconds{0.0};        // сколько уже ждёт (модельные секунды)
    double remainingPatience{0.0};    // сколько терпения осталось (может быть < 0)
};

// Подробный отчёт по клиенту (команда client <id>).
struct ClientReport {
    ClientId id{};
    ClientState state{ClientState::Waiting};
    OperationType requestedOperation{OperationType::CheckBalance};
    Money amount{0};
    long patienceSeconds{0};
    Money accountBalance{0};
    std::vector<OperationRecord> history;  // операции этого клиента из журнала
};

// Сводная статистика СМО в реальном времени (команда stats).
struct StatsSnapshot {
    std::uint64_t served{0};
    std::uint64_t left{0};                    // всего ушли (терпение + ТО)
    std::uint64_t renegedByMaintenance{0};    // из них — ушли из-за ТО (§4.5)
    double avgWaitSeconds{0.0};
    double avgServiceSeconds{0.0};
    std::size_t maxQueueLength{0};
    double rhoTheoretical{0.0};    // ρ = λ/μ (из конфигурации)
    double utilization{0.0};       // фактическая загрузка = занятость/аптайм
    double uptimeSeconds{0.0};
};

// Фильтр для команды operations [--client N] [--type T] [--last N].
struct OperationFilter {
    std::optional<ClientId> client;
    std::optional<OperationType> type;
    std::optional<std::size_t> last;
};

}  // namespace atmsim
