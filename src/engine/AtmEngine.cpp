#include "atmsim/engine/AtmEngine.hpp"

#include <chrono>
#include <mutex>       // std::unique_lock
#include <shared_mutex>

#include "atmsim/core/Operation.hpp"

namespace atmsim {

using Clock = std::chrono::steady_clock;

AtmEngine::AtmEngine(const Config& cfg)
    : cfg_(cfg),
      cashbox_(cfg.atm.initialCash),
      serviceProvider_(makeServiceTimeProvider(cfg.serviceTime)),
      // Два независимых seed'а: разные потоки — разные движки (см. заголовок).
      serviceRng_(cfg.simulation.randomSeed),
      arrivalRng_(cfg.simulation.randomSeed + 1u) {
    // По счёту на каждого будущего клиента (клиент i пользуется счётом i).
    accounts_.reserve(static_cast<std::size_t>(cfg.clients.count));
    for (int i = 0; i < cfg.clients.count; ++i) {
        accounts_.emplace_back(static_cast<AccountId>(i), cfg.clients.initialBalance);
    }
}

// Сколько МОДЕЛЬНЫХ секунд клиент уже ждёт. Модельное время идёт в time_scale
// раз быстрее реального, поэтому реальное ожидание умножаем на time_scale.
double AtmEngine::modelSecondsWaited(const Client& c) const {
    const auto realElapsed = Clock::now() - c.arrivalTime;
    const double realSec = std::chrono::duration<double>(realElapsed).count();
    return realSec * cfg_.simulation.timeScale;
}

Client AtmEngine::makeClient() {
    // Эти распределения дешёвые, строим их по месту. Все берут ТОЛЬКО arrivalRng_,
    // к которому не прикасается ни один другой поток.
    std::discrete_distribution<int> opDist{
        cfg_.clients.weights.checkBalance, cfg_.clients.weights.withdraw, cfg_.clients.weights.deposit};
    std::uniform_int_distribution<std::int64_t> amountDist(cfg_.clients.amountRange.min,
                                                           cfg_.clients.amountRange.max);
    std::uniform_int_distribution<int> patienceDist(cfg_.clients.patienceSeconds.min,
                                                    cfg_.clients.patienceSeconds.max);

    Client c;
    c.id = nextClientId_++;
    c.accountId = static_cast<AccountId>(generatedCount_);  // индекс в accounts_
    c.arrivalTime = Clock::now();
    const int pick = opDist(arrivalRng_);
    c.requestedOperation = (pick == 0) ? OperationType::CheckBalance
                         : (pick == 1) ? OperationType::Withdraw
                                       : OperationType::Deposit;
    c.amount = (c.requestedOperation == OperationType::CheckBalance) ? Money{0}
                                                                     : amountDist(arrivalRng_);
    c.patience = std::chrono::seconds(patienceDist(arrivalRng_));
    c.state = ClientState::Waiting;
    ++generatedCount_;
    return c;
}

// ---------------------------------------------------------------------------
//  ПОТОК ПРИХОДА. Ждёт интервал между клиентами (прерываемо), создаёт клиента,
//  кладёт в очередь и будит поток обслуживания. Останавливается, когда все
//  cfg.clients.count клиентов сгенерированы или пришёл сигнал остановки.
// ---------------------------------------------------------------------------
void AtmEngine::generateArrivals(std::stop_token stopToken) {
    const double meanGapSec = 60.0 / cfg_.clients.arrivalRatePerMinute;
    std::exponential_distribution<double> gapDist(1.0 / meanGapSec);

    while (!stopToken.stop_requested() && generatedCount_ < cfg_.clients.count) {
        // Интервал до следующего прихода. Первый клиент приходит сразу (gap = 0),
        // дальше — по модели: poisson => случайный интервал, batch => постоянный.
        const double gapModel =
            (generatedCount_ == 0) ? 0.0
            : (cfg_.clients.arrivalMode == ArrivalMode::Poisson) ? gapDist(arrivalRng_)
                                                                 : meanGapSec;
        const std::chrono::duration<double> realWait(gapModel / cfg_.simulation.timeScale);

        {
            // Прерываемое ожидание интервала: проснёмся раньше, если остановка.
            std::unique_lock<std::shared_mutex> lock(mutex_);
            wakeUp_.wait_for(lock, stopToken, realWait,
                             [this] { return state_.load() == AtmState::Stopped; });
            if (stopToken.stop_requested() || state_.load() == AtmState::Stopped) return;
        }

        Client c = makeClient();  // трогает только arrivalRng_/счётчики этого потока

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            queue_.push_back(c);
            if (queue_.size() > maxQueueLen_) maxQueueLen_ = queue_.size();
            // Если банкомат простаивал — переводим в рабочий режим.
            if (state_.load() == AtmState::Idle) state_.store(AtmState::Serving);
        }
        wakeUp_.notify_all();  // будим поток обслуживания: появилась работа
    }
}

// ---------------------------------------------------------------------------
//  ПОТОК ОБСЛУЖИВАНИЯ. Главный цикл банкомата.
// ---------------------------------------------------------------------------
void AtmEngine::run(std::stop_token stopToken) {
    while (true) {
        Client client;
        bool haveClient = false;
        double serviceModelSec = 0.0;

        // --- Фаза 1: дождаться работы и взять клиента (под эксклюзивным локом) ---
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            // «Спим с условием»: просыпаемся, когда есть кого обслуживать в рабочем
            // режиме, ЛИБО пришла остановка, ЛИБО (из паузы) сменился режим. На
            // паузе предикат ложен — значит продолжаем спать, не крутя цикл впустую.
            wakeUp_.wait(lock, stopToken, [this] {
                const AtmState s = state_.load();
                if (s == AtmState::Stopped) return true;
                if (s == AtmState::Paused) return false;  // на паузе клиентов не берём
                return !queue_.empty();
            });

            if (stopToken.stop_requested() || state_.load() == AtmState::Stopped) break;
            if (state_.load() == AtmState::Paused || queue_.empty()) continue;

            client = queue_.front();
            queue_.pop_front();

            // Уход по терпению (reneging): если клиент, пока стоял, уже перетерпел
            // — не обслуживаем его, считаем ушедшим (§4.1). (M3: проверяем в момент,
            // когда до него дошла очередь; проактивный уход из очереди — позже.)
            if (modelSecondsWaited(client) > static_cast<double>(client.patience.count())) {
                ++totalLeft_;
                if (queue_.empty() && state_.load() == AtmState::Serving) {
                    state_.store(AtmState::Idle);
                }
                continue;
            }

            client.state = ClientState::InService;
            currentClient_ = client;
            state_.store(AtmState::Serving);
            // Длительность обслуживания берём здесь, под локом, из «своего» RNG.
            serviceModelSec = serviceProvider_->nextSeconds(client.requestedOperation, serviceRng_);
            haveClient = true;
        }
        if (!haveClient) continue;

        // Реальная длительность = модельная / ускорение времени.
        const std::chrono::duration<double> realDuration(serviceModelSec / cfg_.simulation.timeScale);

        // --- Фаза 2: обслуживание (ПРЕРЫВАЕМОЕ ОЖИДАНИЕ) — сердце §6.2 ---------
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            // Вместо «слепого» sleep_for(realDuration) ждём по условию: проснёмся
            // либо когда истечёт realDuration (клиент обслужен штатно), либо когда
            // сменится режим (пауза/стоп через state_ + notify_all), либо по
            // stop_token — СМОТРЯ ЧТО НАСТУПИТ РАНЬШЕ. Благодаря этому pause/stop
            // применяются мгновенно, не дожидаясь конца случайной задержки (§5, §14).
            // Пока идёт это ожидание, лок ОТПУЩЕН — читатели-снимки не блокируются.
            wakeUp_.wait_for(lock, stopToken, realDuration,
                             [this] { return state_.load() != AtmState::Serving; });

            // Операцию доводим до конца в любом случае (§4.6 по умолчанию —
            // «доработать текущую»): мы не ждём остаток задержки, но результат
            // операции применяем, чтобы клиент считался обслуженным честно.
            Account& acct = accounts_[static_cast<std::size_t>(client.accountId)];
            applyOperation(client.requestedOperation, client.amount, acct, cashbox_);
            ++totalServed_;
            currentClient_.reset();

            // Если мы всё ещё в режиме Serving (проснулись по таймауту, штатно) —
            // выбираем следующий режим по наличию очереди. Если во время
            // обслуживания пришла пауза/стоп — состояние уже сменено, не трогаем.
            if (state_.load() == AtmState::Serving) {
                state_.store(queue_.empty() ? AtmState::Idle : AtmState::Serving);
            }
        }

        if (state_.load() == AtmState::Stopped) break;
    }
}

// ---------------------------------------------------------------------------
//  КОМАНДЫ. Каждая: под кратким эксклюзивным локом меняет режим (atomic store),
//  затем notify_all() будит спящие потоки. Вызывающего не блокируют.
// ---------------------------------------------------------------------------
void AtmEngine::requestPause() {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (state_.load() != AtmState::Stopped) state_.store(AtmState::Paused);
    }
    wakeUp_.notify_all();
}

void AtmEngine::requestResume() {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (state_.load() == AtmState::Paused) {
            state_.store(queue_.empty() ? AtmState::Idle : AtmState::Serving);
        }
    }
    wakeUp_.notify_all();
}

void AtmEngine::requestStop() {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        state_.store(AtmState::Stopped);
    }
    wakeUp_.notify_all();
}

// ---------------------------------------------------------------------------
//  СНИМКИ. Читатели берут shared_lock — параллельно друг другу и не мешая
//  писателю дольше, чем нужно на копирование небольшого объёма данных.
// ---------------------------------------------------------------------------
AtmSnapshot AtmEngine::snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    AtmSnapshot s;
    s.state = state_.load();
    s.cashboxBalance = cashbox_.balance();
    if (currentClient_) {
        s.currentClientId = currentClient_->id;
        s.currentOperation = currentClient_->requestedOperation;
    }
    s.totalServed = totalServed_;
    s.totalLeft = totalLeft_;
    s.queueLength = queue_.size();
    s.maxQueueLength = maxQueueLen_;
    return s;
}

std::vector<ClientSnapshot> AtmEngine::queueSnapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ClientSnapshot> out;
    out.reserve(queue_.size());
    for (const auto& c : queue_) {
        ClientSnapshot cs;
        cs.id = c.id;
        cs.requestedOperation = c.requestedOperation;
        cs.amount = c.amount;
        const double waited = modelSecondsWaited(c);
        cs.waitedSeconds = waited;
        cs.remainingPatience = static_cast<double>(c.patience.count()) - waited;
        out.push_back(cs);
    }
    return out;
}

Money AtmEngine::accountsTotal() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Money total = 0;
    for (const auto& a : accounts_) total += a.balance();
    return total;
}

}  // namespace atmsim
