#pragma once
// ============================================================================
//  SimulationRunner.hpp — однопоточная (дискретно-событийная) симуляция очереди.
//
//  Это этап M2 (§13, шаг 2): «без потоков», чтобы отладить саму логику СМО —
//  очередь FIFO, уход по терпению (reneging), статистику — на детерминированном
//  прогоне. В M3 таймингом займётся отдельный поток с прерываемым ожиданием, но
//  доменная логика (applyOperation) и метрики уже будут проверены здесь.
// ============================================================================
#include "atmsim/config/Config.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {

// Сводная статистика прогона (§4.4, §10).
struct SimulationResult {
    int totalClients = 0;
    int served = 0;            // дошли до банкомата (операция выполнена/попытана)
    int leftByPatience = 0;    // ушли, не дождавшись (reneging)
    int opSuccess = 0;         // из обслуженных — успешные операции
    int opFailed = 0;          // из обслуженных — отказ (нет средств/наличных)

    double avgWaitSeconds = 0.0;      // среднее ожидание среди обслуженных
    double avgServiceSeconds = 0.0;   // среднее время обслуживания
    int maxQueueLength = 0;           // макс. число одновременно ожидающих
    double totalModelSeconds = 0.0;   // длительность прогона (последнее событие)

    double rhoTheoretical = 0.0;      // ρ = λ/μ (из конфигурации)
    double serverUtilization = 0.0;   // фактическая загрузка = занятость/время

    Money cashStart = 0, cashEnd = 0;
    Money accountsStart = 0, accountsEnd = 0;

    // Инвариант сохранения денег (§4.2): (сумма счетов − касса) не должна
    // измениться за весь прогон. true, если выполнено.
    bool moneyConserved = false;
};

class SimulationRunner {
public:
    // Прогоняет полную симуляцию по конфигурации и возвращает статистику.
    // Детерминизм: тот же seed (cfg.simulation.randomSeed) => тот же результат.
    static SimulationResult run(const Config& cfg);
};

}  // namespace atmsim
