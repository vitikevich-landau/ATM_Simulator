#pragma once
// ============================================================================
//  ServiceTimeProvider.hpp — время обслуживания как ПОДКЛЮЧАЕМАЯ стратегия (§6.3).
//
//  Паттерн Strategy: у нас есть общий интерфейс «дай длительность обслуживания»,
//  а конкретные распределения (uniform/normal/exponential) — сменные реализации.
//  Это позволяет менять распределение через конфигурацию, не трогая движок.
// ============================================================================
#include <memory>
#include <random>

#include "atmsim/config/Config.hpp"
#include "atmsim/core/Types.hpp"

namespace atmsim {

class ServiceTimeProvider {
public:
    virtual ~ServiceTimeProvider() = default;

    // Возвращает длительность обслуживания в СЕКУНДАХ (модельных).
    // Генератор случайных чисел передаётся снаружи: так все случайные величины
    // берутся из ОДНОГО seed'а (воспроизводимость, §5), а в многопоточном
    // варианте (M3) не будет гонки за общим движком (§6.3).
    // Параметр op пока не используется (в v1 одно распределение на все операции,
    // §16 п.5), но оставлен в интерфейсе на будущее.
    virtual double nextSeconds(OperationType op, std::mt19937_64& rng) = 0;
};

// Фабрика (§6.4): создаёт нужную стратегию по конфигурации.
std::unique_ptr<ServiceTimeProvider> makeServiceTimeProvider(const ServiceTimeConfig& cfg);

// Ожидаемое (среднее) время обслуживания по выбранному распределению — для
// теоретического ρ = λ/μ (§10). ВАЖНО: среднее зависит от распределения, а не
// только от mean_seconds:
//   * normal / exponential — параметр mean_seconds;
//   * uniform — середина диапазона (min_seconds + max_seconds) / 2; mean_seconds
//     здесь вообще не участвует в генерации, поэтому брать его для μ неверно.
double expectedServiceSeconds(const ServiceTimeConfig& cfg);

}  // namespace atmsim
