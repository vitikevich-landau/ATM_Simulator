#pragma once
// ============================================================================
//  Client.hpp — виртуальный посетитель банкомата (§7).
// ============================================================================
#include <chrono>

#include "atmsim/core/Types.hpp"

namespace atmsim {

// Клиент — простой «пакет данных» (agregate/POD-подобный), без методов:
// его создаёт фабрика по конфигурации (M2), а мутирует поток Engine (M3).
struct Client {
    ClientId id{};

    // Момент появления в системе. steady_clock — монотонные часы: их выбирают
    // для измерения ИНТЕРВАЛОВ (сколько ждём), потому что они не «прыгают» при
    // переводе системного времени. Терпение считаем как now - arrivalTime.
    std::chrono::steady_clock::time_point arrivalTime{};

    OperationType requestedOperation{OperationType::CheckBalance};
    Money amount{0};                       // 0 для CheckBalance
    std::chrono::seconds patience{0};      // сколько готов ждать в очереди
    ClientState state{ClientState::Waiting};
    AccountId accountId{};
};

}  // namespace atmsim
