#include "atmsim/engine/ClientFactory.hpp"

namespace atmsim {

std::vector<SimClient> ClientFactory::generate(std::mt19937_64& rng) const {
    std::vector<SimClient> clients;
    if (cfg_.count <= 0) return clients;
    clients.reserve(static_cast<std::size_t>(cfg_.count));

    // Распределения для атрибутов клиента.
    //   opDist — выбор операции по весам {checkBalance, withdraw, deposit}.
    //            discrete_distribution сам нормирует веса, сумма не обязана быть 1.
    std::discrete_distribution<int> opDist{
        cfg_.weights.checkBalance, cfg_.weights.withdraw, cfg_.weights.deposit};
    std::uniform_int_distribution<std::int64_t> amountDist(cfg_.amountRange.min, cfg_.amountRange.max);
    std::uniform_int_distribution<int> patienceDist(cfg_.patienceSeconds.min, cfg_.patienceSeconds.max);

    // Интервал между приходами. Для poisson он случайный (экспоненциальный),
    // для batch — постоянный. Среднее одно и то же: 60/rate секунд.
    const double meanGapSec = 60.0 / cfg_.arrivalRatePerMinute;
    std::exponential_distribution<double> gapDist(1.0 / meanGapSec);

    double t = 0.0;
    for (int i = 0; i < cfg_.count; ++i) {
        SimClient c;
        c.id = static_cast<ClientId>(i + 1);
        c.accountId = static_cast<AccountId>(i);

        // Время прихода: первый клиент в момент 0, дальше — накопленный интервал.
        if (i > 0) {
            t += (cfg_.arrivalMode == ArrivalMode::Poisson) ? gapDist(rng) : meanGapSec;
        }
        c.arrivalSec = t;

        // Тип операции по весам: 0 = CheckBalance, 1 = Withdraw, 2 = Deposit.
        const int pick = opDist(rng);
        c.op = (pick == 0) ? OperationType::CheckBalance
             : (pick == 1) ? OperationType::Withdraw
                           : OperationType::Deposit;

        // Сумма нужна только для снятия/внесения; для проверки баланса — 0.
        c.amount = (c.op == OperationType::CheckBalance) ? Money{0} : amountDist(rng);

        c.patienceSec = static_cast<double>(patienceDist(rng));

        clients.push_back(c);
    }
    return clients;
}

}  // namespace atmsim
