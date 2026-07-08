#include "atmsim/engine/AtmEngine.hpp"

#include <algorithm>  // std::min
#include <chrono>
#include <mutex>  // std::unique_lock, std::lock_guard

#include "atmsim/core/Operation.hpp"

namespace atmsim {

using Clock = std::chrono::steady_clock;

AtmEngine::AtmEngine(const Config& cfg, Logger* logger)
    : cfg_(cfg),
      cashbox_(cfg.atm.initialCash),
      serviceProvider_(makeServiceTimeProvider(cfg.serviceTime)),
      // Два независимых seed'а: разные потоки — разные движки (см. заголовок).
      serviceRng_(cfg.simulation.randomSeed),
      arrivalRng_(cfg.simulation.randomSeed + 1u),
      logger_(logger) {
    // По счёту на каждого будущего клиента (клиент i пользуется счётом i).
    accounts_.reserve(static_cast<std::size_t>(cfg.clients.count));
    for (int i = 0; i < cfg.clients.count; ++i) {
        accounts_.emplace_back(static_cast<AccountId>(i), cfg.clients.initialBalance);
    }
    roster_.reserve(static_cast<std::size_t>(cfg.clients.count));
    startTime_ = Clock::now();  // отсчёт аптайма
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
void AtmEngine::generateArrivals() {
    // Остановка — через requestStop() (state_ == Stopped): он под mutex_ меняет
    // состояние и делает notify_all(), а предикаты ожиданий ниже проверяют
    // state_ == Stopped. Это канонический паттерн condition_variable без потери
    // пробуждений. Никаких stop_token/stop_callback/cv_any (несовместимо с MSVC).
    const double meanGapSec = 60.0 / cfg_.clients.arrivalRatePerMinute;
    std::exponential_distribution<double> gapDist(1.0 / meanGapSec);

    while (state_.load() != AtmState::Stopped && generatedCount_ < cfg_.clients.count) {
        // Интервал до следующего прихода. Первый клиент приходит сразу (gap = 0),
        // дальше — по модели: poisson => случайный интервал, batch => постоянный.
        const double gapModel =
            (generatedCount_ == 0) ? 0.0
            : (cfg_.clients.arrivalMode == ArrivalMode::Poisson) ? gapDist(arrivalRng_)
                                                                 : meanGapSec;
        const std::chrono::duration<double> realWait(gapModel / cfg_.simulation.timeScale);

        {
            // Прерываемое ожидание интервала: проснёмся раньше при остановке.
            std::unique_lock<std::mutex> lock(mutex_);
            wakeUp_.wait_for(lock, realWait, [this] {
                return state_.load() == AtmState::Stopped;
            });
            if (state_.load() == AtmState::Stopped) return;
        }

        // Если по конфигу приход НЕ продолжается во время ТО (§4.5 п.4) — ждём
        // окончания ТО, прежде чем впускать нового клиента. По умолчанию приход
        // продолжается (люди подходят и видят, что банкомат недоступен).
        if (!cfg_.maintenance.arrivalsContinue) {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeUp_.wait(lock, [this] {
                return state_.load() != AtmState::Maintenance;  // Stopped тоже != Maintenance
            });
            if (state_.load() == AtmState::Stopped) return;
        }

        Client c = makeClient();  // трогает только arrivalRng_/счётчики этого потока

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Стоп мог прийти, пока мы создавали клиента (между локами) — тогда
            // не добавляем его: «висящий» Waiting-клиент исказил бы итоговые счётчики.
            if (state_.load() == AtmState::Stopped) return;
            queue_.push_back(c);
            roster_.push_back(c);  // реестр всех клиентов (roster_[id-1])
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
void AtmEngine::run() {
    if (logger_) logger_->info("Поток обслуживания запущен");

    // Остановка — через requestStop() (state_ == Stopped), см. комментарий к
    // wakeUp_ в заголовке. Все предикаты ожиданий ниже проверяют state_.
    while (true) {
        Client client;
        bool haveClient = false;
        double serviceModelSec = 0.0;
        double waitedModel = 0.0;  // сколько клиент прождал (для статистики)
        Money cashAfter = 0;       // касса после операции (для лога низкой кассы)

        // --- Фаза 1: дождаться работы и взять клиента (под эксклюзивным локом) ---
        {
            std::unique_lock<std::mutex> lock(mutex_);

            // РЕЖИМ ТО (§4.5). Обрабатываем отдельно: новых клиентов не берём,
            // ждём конца ТО (по таймеру или по команде maintenance stop).
            if (state_.load() == AtmState::Maintenance) {
                // Один раз при входе в ТО — каждый в очереди «решает» уйти/остаться.
                if (maintenanceRenegePending_) {
                    applyMaintenanceRenegingLocked();
                    maintenanceRenegePending_ = false;
                }
                // Ждём: наступит дедлайн ТО, ЛИБО сменится режим (maintenance stop /
                // stop), смотря что раньше. Для «бессрочного» ТО (deadline == max)
                // ждём без таймаута: wait_until с time_point::max() на некоторых
                // реализациях переполняется.
                const auto maintenanceOver = [this] {
                    return state_.load() != AtmState::Maintenance;  // Stopped тоже подходит
                };
                if (maintenanceDeadline_ == Clock::time_point::max()) {
                    wakeUp_.wait(lock, maintenanceOver);
                } else {
                    wakeUp_.wait_until(lock, maintenanceDeadline_, maintenanceOver);
                }
                // Остановка во время ТО: выходим из цикла (иначе продолжали бы ждать ТО).
                if (state_.load() == AtmState::Stopped) break;
                // Если всё ещё ТО и время вышло — авто-завершение.
                if (state_.load() == AtmState::Maintenance && Clock::now() >= maintenanceDeadline_) {
                    state_.store(queue_.empty() ? AtmState::Idle : AtmState::Serving);
                    // Обязательно будим: поток прихода может спать в ожидании конца
                    // ТО (arrivals_continue=false), и без notify его предикат
                    // «state != Maintenance» никто больше не перепроверит — приход
                    // клиентов замер бы навсегда.
                    wakeUp_.notify_all();
                }
                continue;  // на новый круг: там уже обычный режим
            }

            // «Спим с условием»: просыпаемся, когда есть кого обслуживать в рабочем
            // режиме, ЛИБО пришла остановка, ЛИБО сменился режим (пауза/ТО). На
            // паузе предикат ложен — значит продолжаем спать, не крутя цикл впустую.
            wakeUp_.wait(lock, [this] {
                const AtmState s = state_.load();
                if (s == AtmState::Stopped) return true;
                if (s == AtmState::Maintenance) return true;  // выйти -> обработать ТО сверху
                if (s == AtmState::Paused) return false;      // на паузе клиентов не берём
                return !queue_.empty();
            });

            if (state_.load() == AtmState::Stopped) break;
            if (state_.load() == AtmState::Maintenance) continue;  // на след. круге — ветка ТО
            if (state_.load() == AtmState::Paused || queue_.empty()) continue;

            client = queue_.front();
            queue_.pop_front();
            waitedModel = modelSecondsWaited(client);

            // Уход по терпению (reneging): если клиент, пока стоял, уже перетерпел
            // — не обслуживаем его, считаем ушедшим (§4.1). (M3: проверяем в момент,
            // когда до него дошла очередь; проактивный уход из очереди — позже.)
            if (waitedModel > static_cast<double>(client.patience.count())) {
                ++totalLeft_;
                roster_[client.id - 1].state = ClientState::LeftQueue;
                if (queue_.empty() && state_.load() == AtmState::Serving) {
                    state_.store(AtmState::Idle);
                }
                continue;
            }

            client.state = ClientState::InService;
            roster_[client.id - 1].state = ClientState::InService;
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
            std::unique_lock<std::mutex> lock(mutex_);

            // Вместо «слепого» sleep_for(realDuration) ждём по условию, накапливая
            // ЧИСТОЕ время обслуживания (servedReal), пока не набежит realDuration.
            // Разные прерывания трактуем по-разному (§4.6):
            //   * pause — ПРИОСТАНОВКА: клиент остаётся «в обслуживании»
            //     (currentClient_ не сбрасываем), таймер службы замирает и
            //     продолжается с resume — остаток дообслуживаем;
            //   * stop / ТО — «доработать текущую»: выходим из цикла и ниже
            //     доводим операцию до конца.
            // Пока идут ожидания, лок ОТПУЩЕН — читатели-снимки не блокируются,
            // а pause/resume/stop применяются мгновенно (§5, §14).
            std::chrono::duration<double> remaining(realDuration);
            std::chrono::duration<double> servedReal(0.0);
            for (;;) {
                const auto sliceStart = Clock::now();
                wakeUp_.wait_for(lock, remaining, [this] {
                    return state_.load() != AtmState::Serving;  // Paused/Stopped/ТО будят
                });
                servedReal += std::chrono::duration<double>(Clock::now() - sliceStart);
                if (state_.load() != AtmState::Paused) {
                    // Serving — истёк таймер (обслужен штатно); Stopped/ТО — доводим.
                    break;
                }
                // Пауза: замираем до resume/stop, НЕ тратя время службы. Остаток
                // (realDuration − уже отработанное) дообслужим после возобновления.
                remaining = realDuration - servedReal;
                if (remaining < std::chrono::duration<double>::zero()) {
                    remaining = std::chrono::duration<double>::zero();
                }
                wakeUp_.wait(lock, [this] { return state_.load() != AtmState::Paused; });
                if (state_.load() == AtmState::Stopped) break;  // stop во время паузы
                // resume вернул Serving — следующая итерация дообслужит remaining.
            }
            // Фактически прошедшее МОДЕЛЬНОЕ время обслуживания (без учёта пауз).
            // При штатном завершении ~= serviceModelSec; при stop/ТО в середине —
            // меньше. Нужно для честного учёта загрузки ниже.
            const double actualServiceModelSec = servedReal.count() * cfg_.simulation.timeScale;

            // Операцию доводим до конца в любом случае (§4.6 по умолчанию —
            // «доработать текущую»): мы не ждём остаток задержки, но результат
            // операции применяем, чтобы клиент считался обслуженным честно.
            Account& acct = accounts_[static_cast<std::size_t>(client.accountId)];
            const OperationOutcome outcome =
                applyOperation(client.requestedOperation, client.amount, acct, cashbox_);

            // Запоминаем направление последнего УСПЕШНОГО движения кассы —
            // для подсветки суммы кассы на дашборде (зелёным/красным).
            if (outcome.ok() && (client.requestedOperation == OperationType::Deposit ||
                                 client.requestedOperation == OperationType::Withdraw)) {
                lastCashMove_ = client.requestedOperation;
            }

            // Запись в бизнес-журнал (§4.4): кто, что, когда, с каким исходом.
            OperationRecord rec;
            rec.id = nextOperationId_++;
            rec.clientId = client.id;
            rec.type = client.requestedOperation;
            rec.amount = client.amount;
            rec.balanceAfter = outcome.balanceAfter;
            rec.timestamp = std::chrono::system_clock::now();
            rec.success = outcome.ok();
            rec.errorMessage = outcome.ok() ? std::string{} : to_string(outcome.status);
            log_.push_back(rec);

            // Обновляем реестр и статистику.
            roster_[client.id - 1].state = ClientState::Served;
            ++totalServed_;
            sumWaitModel_ += waitedModel;
            sumServiceModel_ += serviceModelSec;
            // В загрузку (utilization = busy/uptime) кредитуем ФАКТИЧЕСКИ прошедшее
            // модельное время, а не номинал: иначе прерывание длинной операции
            // приписывало бы банкомату больше «занятого» времени, чем прошло с его
            // старта, и utilization могла превысить 1.0. min — страховка от джиттера
            // планировщика при штатном завершении. sumServiceModel_ (среднее время
            // обслуживания) остаётся номинальным — это самостоятельная метрика.
            busyModel_ += std::min(actualServiceModelSec, serviceModelSec);
            cashAfter = cashbox_.balance();
            currentClient_.reset();

            // Если мы всё ещё в режиме Serving (проснулись по таймауту, штатно) —
            // выбираем следующий режим по наличию очереди. Если во время
            // обслуживания пришла пауза/стоп — состояние уже сменено, не трогаем.
            if (state_.load() == AtmState::Serving) {
                state_.store(queue_.empty() ? AtmState::Idle : AtmState::Serving);
            }
        }

        // Технический лог — ВНЕ критической секции (файловый I/O не держит mutex_).
        if (logger_) {
            logger_->debug("Обслужен клиент #" + std::to_string(client.id) +
                           " (" + to_string(client.requestedOperation) + ")");
            if (!lowCashLogged_ && cashAfter < cfg_.atm.lowCashThreshold) {
                logger_->warn("Касса ниже порога инкассации: " + formatMoney(cashAfter));
                lowCashLogged_ = true;
            }
        }

        if (state_.load() == AtmState::Stopped) break;
    }
    if (logger_) logger_->info("Поток обслуживания завершён");
}

// ---------------------------------------------------------------------------
//  КОМАНДЫ. Каждая: под кратким эксклюзивным локом меняет режим (atomic store),
//  затем notify_all() будит спящие потоки. Вызывающего не блокируют.
// ---------------------------------------------------------------------------
bool AtmEngine::requestPause() {
    bool applied = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Пауза действует ТОЛЬКО из рабочих режимов (Idle/Serving). Раньше здесь
        // было «любой не-Stopped -> Paused», и pause поверх Maintenance незаметно
        // обрывал ТО: Maintenance -> Paused, а resume уводил в Idle/Serving в обход
        // и таймера, и команды maintenance stop — единственных легальных выходов из
        // ТО (§4.5 п.5). Теперь во время ТО pause — no-op, ТО идёт своим чередом.
        const AtmState s = state_.load();
        if (s == AtmState::Idle || s == AtmState::Serving) {
            state_.store(AtmState::Paused);
            applied = true;
        }
    }
    // Будить потоки есть смысл только если режим реально сменился.
    if (applied) wakeUp_.notify_all();
    if (logger_) {
        logger_->info(applied ? "Команда: пауза"
                              : "Команда: пауза проигнорирована (идёт ТО или остановка)");
    }
    return applied;
}

void AtmEngine::requestResume() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_.load() == AtmState::Paused) {
            // Если пауза застала клиента «в обслуживании» (currentClient_ держится),
            // возвращаемся в Serving, чтобы поток дообслужил его остаток — даже если
            // очередь опустела. Иначе режим выбираем по наличию очереди.
            const bool hasWork = currentClient_.has_value() || !queue_.empty();
            state_.store(hasWork ? AtmState::Serving : AtmState::Idle);
        }
    }
    wakeUp_.notify_all();
    if (logger_) logger_->info("Команда: возобновление");
}

void AtmEngine::requestStop() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        state_.store(AtmState::Stopped);
    }
    wakeUp_.notify_all();
    if (logger_) logger_->info("Команда: остановка");
}

void AtmEngine::requestMaintenance(std::optional<int> durationSeconds) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_.load() == AtmState::Stopped) return;

        // Длительность (в МОДЕЛЬНЫХ секундах): явная из аргумента, иначе из конфига.
        const int durModel = durationSeconds.value_or(cfg_.maintenance.defaultDurationSeconds);
        if (durModel > 0) {
            // Модель бежит в time_scale раз быстрее реального времени.
            const double realSec = static_cast<double>(durModel) / cfg_.simulation.timeScale;
            maintenanceDeadline_ = Clock::now() +
                std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(realSec));
        } else {
            maintenanceDeadline_ = Clock::time_point::max();  // до явной команды stop
        }
        state_.store(AtmState::Maintenance);
        maintenanceRenegePending_ = true;  // поток обслуживания применит уйти/остаться
    }
    wakeUp_.notify_all();
    if (logger_) logger_->info("Команда: техобслуживание начато");
}

void AtmEngine::endMaintenance() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_.load() == AtmState::Maintenance) {
            state_.store(queue_.empty() ? AtmState::Idle : AtmState::Serving);
        }
    }
    wakeUp_.notify_all();
    if (logger_) logger_->info("Команда: техобслуживание завершено");
}

void AtmEngine::applyMaintenanceRenegingLocked() {
    // Каждый стоящий в очереди с вероятностью renege_probability уходит сразу
    // (§4.5 п.3), остальные остаются ждать (но не дольше своего терпения — это
    // отработает обычная проверка reneging при выборке). serviceRng_ принадлежит
    // только потоку обслуживания, откуда мы и вызваны, поэтому гонки за RNG нет.
    std::bernoulli_distribution leaves(cfg_.maintenance.renegeProbability);
    std::deque<Client> kept;
    while (!queue_.empty()) {
        Client c = queue_.front();
        queue_.pop_front();
        if (leaves(serviceRng_)) {
            roster_[c.id - 1].state = ClientState::LeftQueue;
            ++totalLeft_;
            ++totalLeftMaintenance_;
        } else {
            kept.push_back(c);
        }
    }
    queue_ = std::move(kept);
}

bool AtmEngine::allClientsProcessed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Каждый из cfg_.clients.count клиентов заканчивает ровно в одном терминальном
    // состоянии — обслужен или ушёл, — поэтому равенство «обслужено + ушли == всего»
    // само по себе означает, что в очереди и на обслуживании никого не осталось.
    // Считаем ТОЛЬКО lock-защищённые счётчики (totalServed_/totalLeft_) и const-
    // конфиг. generatedCount_ читать нельзя: его без мьютекса ведёт поток прихода
    // (иначе гонка данных — ловится ThreadSanitizer).
    return (totalServed_ + totalLeft_) >= static_cast<std::uint64_t>(cfg_.clients.count);
}

// ---------------------------------------------------------------------------
//  СНИМКИ. Читатели берут shared_lock — параллельно друг другу и не мешая
//  писателю дольше, чем нужно на копирование небольшого объёма данных.
// ---------------------------------------------------------------------------
AtmSnapshot AtmEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    s.uptimeSeconds =
        std::chrono::duration<double>(Clock::now() - startTime_).count() * cfg_.simulation.timeScale;
    s.lowCash = cashbox_.balance() < cfg_.atm.lowCashThreshold;
    s.lastCashMove = lastCashMove_;

    // Остаток режима ТО (для status/atm/дашборда).
    if (state_.load() == AtmState::Maintenance) {
        if (maintenanceDeadline_ == Clock::time_point::max()) {
            s.maintenanceEtaSeconds = -1.0;  // до явной команды stop
        } else {
            const double realLeft =
                std::chrono::duration<double>(maintenanceDeadline_ - Clock::now()).count();
            s.maintenanceEtaSeconds = (realLeft > 0.0) ? realLeft * cfg_.simulation.timeScale : 0.0;
        }
    }
    return s;
}

std::vector<ClientSnapshot> AtmEngine::queueSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    Money total = 0;
    for (const auto& a : accounts_) total += a.balance();
    return total;
}

StatsSnapshot AtmEngine::statsSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StatsSnapshot s;
    s.served = totalServed_;
    s.left = totalLeft_;
    s.renegedByMaintenance = totalLeftMaintenance_;
    s.avgWaitSeconds = (totalServed_ > 0) ? sumWaitModel_ / static_cast<double>(totalServed_) : 0.0;
    s.avgServiceSeconds = (totalServed_ > 0) ? sumServiceModel_ / static_cast<double>(totalServed_) : 0.0;
    s.maxQueueLength = maxQueueLen_;

    // ρ = λ/μ из конфигурации (§10). μ = 1/среднее_время_обслуживания, где
    // среднее берётся по фактическому распределению (для uniform это (min+max)/2,
    // а не mean_seconds) — см. expectedServiceSeconds().
    const double lambda = cfg_.clients.arrivalRatePerMinute / 60.0;
    const double meanService = expectedServiceSeconds(cfg_.serviceTime);
    const double mu = (meanService > 0.0) ? 1.0 / meanService : 0.0;
    s.rhoTheoretical = (mu > 0.0) ? lambda / mu : 0.0;

    const double uptimeModel =
        std::chrono::duration<double>(Clock::now() - startTime_).count() * cfg_.simulation.timeScale;
    s.uptimeSeconds = uptimeModel;
    s.utilization = (uptimeModel > 0.0) ? busyModel_ / uptimeModel : 0.0;
    return s;
}

std::optional<ClientReport> AtmEngine::clientReport(ClientId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == 0 || id > roster_.size()) return std::nullopt;
    const Client& c = roster_[static_cast<std::size_t>(id - 1)];
    ClientReport r;
    r.id = c.id;
    r.state = c.state;
    r.requestedOperation = c.requestedOperation;
    r.amount = c.amount;
    r.patienceSeconds = static_cast<long>(c.patience.count());
    r.accountBalance = accounts_[static_cast<std::size_t>(c.accountId)].balance();
    for (const auto& rec : log_) {
        if (rec.clientId == id) r.history.push_back(rec);
    }
    return r;
}

std::optional<Money> AtmEngine::balanceOf(ClientId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == 0 || id > roster_.size()) return std::nullopt;
    const Client& c = roster_[static_cast<std::size_t>(id - 1)];
    return accounts_[static_cast<std::size_t>(c.accountId)].balance();
}

std::vector<OperationRecord> AtmEngine::operations(const OperationFilter& filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OperationRecord> out;
    for (const auto& rec : log_) {
        if (filter.client && rec.clientId != *filter.client) continue;
        if (filter.type && rec.type != *filter.type) continue;
        out.push_back(rec);
    }
    // --last N: оставляем только последние N записей.
    if (filter.last && out.size() > *filter.last) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(*filter.last));
    }
    return out;
}

}  // namespace atmsim
