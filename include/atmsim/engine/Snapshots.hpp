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

#include "atmsim/core/Types.hpp"

namespace atmsim {

// Снимок состояния банкомата целиком (для команды status / будущего дашборда).
struct AtmSnapshot {
    AtmState state{AtmState::Idle};
    Money cashboxBalance{0};
    std::optional<ClientId> currentClientId;          // кого обслуживают сейчас
    std::optional<OperationType> currentOperation;    // и какую операцию
    std::uint64_t totalServed{0};
    std::uint64_t totalLeft{0};                       // ушли по терпению
    std::size_t queueLength{0};
    std::size_t maxQueueLength{0};
};

// Снимок одного ожидающего клиента (для команды queue / дашборда).
struct ClientSnapshot {
    ClientId id{};
    OperationType requestedOperation{OperationType::CheckBalance};
    Money amount{0};
    double waitedSeconds{0.0};        // сколько уже ждёт (модельные секунды)
    double remainingPatience{0.0};    // сколько терпения осталось (может быть < 0)
};

}  // namespace atmsim
