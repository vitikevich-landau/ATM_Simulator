#include "atmsim/engine/SimulationRunner.hpp"

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include "atmsim/core/Account.hpp"
#include "atmsim/core/Cashbox.hpp"
#include "atmsim/core/Operation.hpp"
#include "atmsim/engine/ClientFactory.hpp"
#include "atmsim/engine/ServiceTimeProvider.hpp"

namespace atmsim {
namespace {

Money sumAccounts(const std::vector<Account>& accounts) {
    Money total = 0;
    for (const auto& a : accounts) total += a.balance();
    return total;
}

}  // namespace

SimulationResult SimulationRunner::run(const Config& cfg) {
    // Единственный генератор случайных чисел на весь прогон — от seed из конфига.
    // Все случайности (приход, операции, суммы, терпение, время обслуживания)
    // берутся из него, поэтому прогон полностью воспроизводим (§5).
    std::mt19937_64 rng(cfg.simulation.randomSeed);

    // 1) Готовим входные данные: клиентов, стратегию времени обслуживания, счета, кассу.
    const std::vector<SimClient> clients = ClientFactory(cfg.clients).generate(rng);
    auto serviceTime = makeServiceTimeProvider(cfg.serviceTime);

    std::vector<Account> accounts;
    accounts.reserve(clients.size());
    for (std::size_t i = 0; i < clients.size(); ++i) {
        accounts.emplace_back(static_cast<AccountId>(i), cfg.clients.initialBalance);
    }
    Cashbox cashbox(cfg.atm.initialCash);

    SimulationResult r;
    r.totalClients = static_cast<int>(clients.size());
    r.accountsStart = sumAccounts(accounts);
    r.cashStart = cashbox.balance();

    // Для расчёта максимальной длины очереди собираем «события ожидания»:
    // на каждый интервал [пришёл, начал обслуживаться/ушёл) ставим +1 в начале
    // и −1 в конце. Максимум одновременно активных интервалов = макс. очередь.
    std::vector<std::pair<double, int>> queueEvents;
    queueEvents.reserve(clients.size() * 2);

    // 2) ГЛАВНЫЙ ЦИКЛ. Клиенты идут в порядке прихода (FIFO). Ключевая переменная —
    //    serverFreeTime: момент, когда банкомат освободится. Обрабатываем строго
    //    по возрастанию времени прихода, и этого достаточно, чтобы корректно
    //    смоделировать одноканальную очередь с уходом по терпению:
    //      * ушедший клиент НЕ занимает банкомат (serverFreeTime не двигает);
    //      * следующий начинает обслуживаться в max(его приход, serverFreeTime).
    double serverFreeTime = 0.0;
    double busyTime = 0.0;       // суммарное время, когда банкомат обслуживал
    double sumWait = 0.0;        // сумма времён ожидания обслуженных
    double sumService = 0.0;     // сумма времён обслуживания
    double lastEventSec = 0.0;   // конец последней активности (для загрузки)

    for (const SimClient& c : clients) {
        // Когда клиент начал БЫ обслуживаться, если дождётся: не раньше своего
        // прихода и не раньше, чем освободится банкомат.
        const double startIfServed = std::max(c.arrivalSec, serverFreeTime);
        const double wait = startIfServed - c.arrivalSec;

        if (wait > c.patienceSec) {
            // --- УХОД ПО ТЕРПЕНИЮ (reneging, §4.1) ---
            // Клиент не дожил в очереди до своей очереди — уходит в момент,
            // когда терпение истекло. Банкомат он не занимает.
            ++r.leftByPatience;
            const double leaveSec = c.arrivalSec + c.patienceSec;
            queueEvents.emplace_back(c.arrivalSec, +1);
            queueEvents.emplace_back(leaveSec, -1);
            lastEventSec = std::max(lastEventSec, leaveSec);
        } else {
            // --- ОБСЛУЖИВАНИЕ ---
            const double service = serviceTime->nextSeconds(c.op, rng);
            const double serviceStart = startIfServed;
            const double serviceEnd = serviceStart + service;
            serverFreeTime = serviceEnd;  // банкомат занят до этого момента

            ++r.served;
            sumWait += wait;
            sumService += service;
            busyTime += service;
            lastEventSec = std::max(lastEventSec, serviceEnd);

            // Применяем банковскую операцию к счёту и кассе (M1-логика).
            // Отказ (нет средств/наличных) — штатный исход, клиент всё равно
            // считается обслуженным (он дошёл до банкомата и получил ответ).
            Account& acct = accounts[static_cast<std::size_t>(c.accountId)];
            const OperationOutcome outcome = applyOperation(c.op, c.amount, acct, cashbox);
            if (outcome.ok()) ++r.opSuccess; else ++r.opFailed;

            // В очереди клиент стоял с прихода до начала обслуживания.
            queueEvents.emplace_back(c.arrivalSec, +1);
            queueEvents.emplace_back(serviceStart, -1);
        }
    }

    // 3) Макс. длина очереди — «заметание» событий по времени. При совпадении
    //    времён сначала обрабатываем уход (−1), потом приход (+1), чтобы не
    //    засчитать лишнего в момент, когда один ушёл ровно когда другой пришёл.
    std::sort(queueEvents.begin(), queueEvents.end(),
              [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });
    int current = 0;
    for (const auto& e : queueEvents) {
        current += e.second;
        r.maxQueueLength = std::max(r.maxQueueLength, current);
    }

    // 4) Финальные метрики.
    r.avgWaitSeconds = (r.served > 0) ? sumWait / r.served : 0.0;
    r.avgServiceSeconds = (r.served > 0) ? sumService / r.served : 0.0;
    r.totalModelSeconds = lastEventSec;

    // ρ = λ/μ: λ — приходов в секунду, μ — обслуживаний в секунду (1/среднее).
    const double lambdaPerSec = cfg.clients.arrivalRatePerMinute / 60.0;
    const double muPerSec = (cfg.serviceTime.meanSeconds > 0.0)
                                ? 1.0 / cfg.serviceTime.meanSeconds : 0.0;
    r.rhoTheoretical = (muPerSec > 0.0) ? lambdaPerSec / muPerSec : 0.0;
    r.serverUtilization = (lastEventSec > 0.0) ? busyTime / lastEventSec : 0.0;

    r.accountsEnd = sumAccounts(accounts);
    r.cashEnd = cashbox.balance();
    // Инвариант: разность (счета − касса) обязана сохраниться (§4.2).
    r.moneyConserved =
        (r.accountsEnd - r.cashEnd) == (r.accountsStart - r.cashStart);

    return r;
}

}  // namespace atmsim
