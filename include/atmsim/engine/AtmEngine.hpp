#pragma once
// ============================================================================
//  AtmEngine.hpp — многопоточное ядро банкомата (M3).
//
//  МОДЕЛЬ ПАРАЛЛЕЛИЗМА (§6.1) — «один писатель, много читателей»:
//    * поток обслуживания (run)        — ЕДИНСТВЕННЫЙ, кто меняет счета/кассу/
//                                         журнал/счётчики;
//    * поток прихода (generateArrivals) — добавляет клиентов в очередь;
//    * поток консоли                    — только читает снимки и шлёт команды.
//  Всё общее изменяемое состояние защищено одним std::mutex (кратко: писатели
//  и читатели берут его эксклюзивно, но держат лишь на время копирования снимка;
//  во время долгого обслуживания лок отпущен). Состояние-режим (state_) — atomic,
//  чтобы быстро проверять его в предикатах ожидания и в снимках.
// ============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include "atmsim/config/Config.hpp"
#include "atmsim/core/Account.hpp"
#include "atmsim/core/Cashbox.hpp"
#include "atmsim/core/Client.hpp"
#include "atmsim/engine/ServiceTimeProvider.hpp"
#include "atmsim/engine/Snapshots.hpp"
#include "atmsim/reporting/Logger.hpp"

namespace atmsim {

/// Результат команды «maintenance start» (§4.5): Started — ТО началось сразу
/// (никого не обслуживали); Deferred — банкомат дорабатывает текущего клиента,
/// ТО начнётся после него; Ignored — банкомат уже остановлен, команда не имеет
/// смысла. Консоль по этому значению печатает точный ответ администратору.
enum class MaintenanceStart { Started, Deferred, Ignored };

/// \brief Многопоточное ядро банкомата: поток обслуживания + поток прихода,
///        потокобезопасные снимки для отчётов, прерываемое ожидание (§6.2).
/// \note Всё изменяемое состояние мутирует только поток обслуживания (§6.1);
///       читатели работают через const-снимки под тем же мьютексом.
class AtmEngine {
public:
    // logger — необязательный технический лог (§10); nullptr = не логировать.
    explicit AtmEngine(const Config& cfg, Logger* logger = nullptr);

    // --- Потоковые функции (каждая крутится в своём std::thread) -------------
    // Останавливаются по requestStop() (state_ становится Stopped); поэтому
    // вызывающий ОБЯЗАН вызвать requestStop() перед join() этих потоков.
    void run();               // цикл обслуживания
    void generateArrivals();  // цикл прихода клиентов

    // --- Команды администратора (потокобезопасны, вызывающего НЕ блокируют) --
    // Пауза приостанавливает ОБСЛУЖИВАНИЕ (§4.6). Возвращает true, если пауза
    // применена; false — если банкомат не в рабочем режиме (идёт ТО или уже
    // остановлен) и команда проигнорирована: ТО прерывать паузой нельзя, оно
    // завершается только по таймеру или командой maintenance stop (§4.5 п.5).
    bool requestPause();
    void requestResume();
    void requestStop();
    // Техобслуживание (§4.5). durationSeconds — длительность в МОДЕЛЬНЫХ секундах;
    // если не задано — берётся default_duration_seconds из конфига; значение <= 0
    // означает «до явной команды maintenance stop». Если сейчас идёт обслуживание,
    // ТО откладывается до его конца (возврат Deferred, снимок покажет
    // maintenancePending) — начатую операцию не обрываем.
    MaintenanceStart requestMaintenance(std::optional<int> durationSeconds);
    void endMaintenance();

    // --- Снимки и отчёты (потокобезопасное чтение под shared_lock) -----------
    AtmSnapshot snapshot() const;
    std::vector<ClientSnapshot> queueSnapshot() const;
    StatsSnapshot statsSnapshot() const;                       // команда stats
    std::optional<ClientReport> clientReport(ClientId id) const; // команда client
    std::optional<Money> balanceOf(ClientId id) const;         // команда balance
    std::vector<OperationRecord> operations(const OperationFilter& filter) const; // operations

    // Единый согласованный снимок для одного кадра дашборда: atm+stats+queue+
    // лента(feedFilter)+allProcessed под ОДНИМ захватом mutex_ (см. FullSnapshot
    // в Snapshots.hpp). Рендер-цикл зовёт именно его вместо пяти раздельных локов.
    FullSnapshot fullSnapshot(const OperationFilter& feedFilter) const;

    // Сумма балансов всех счетов — для проверки инварианта денег и статистики.
    Money accountsTotal() const;

    bool isStopped() const { return state_.load() == AtmState::Stopped; }

    // true, когда прогон отработан полностью: все клиенты сгенерированы, никого
    // нет в очереди и на обслуживании, и каждый достиг терминального состояния
    // (обслужен или ушёл). Консоль по этому признаку предлагает перезапуск/выход.
    bool allClientsProcessed() const;

private:
    // Вспомогательные (вызываются строго из потока прихода / под локом).
    Client makeClient();                              // только поток прихода
    double modelSecondsWaited(const Client& c) const; // сколько модельных сек. ждёт
    // Тот же расчёт, но с ГОТОВОЙ отметкой времени: снимки/прополка очереди
    // берут Clock::now() ОДИН раз на весь проход и передают его сюда — так все
    // waited-времена в одном снимке согласованы на один миг, а не «плывут» от
    // клиента к клиенту, и часы не дёргаются на каждого из десятков клиентов.
    double modelSecondsWaited(const Client& c, std::chrono::steady_clock::time_point now) const;
    // Уход по терпению (§4.1): удалить из очереди всех, кто уже перетерпел, и
    // найти ближайший дедлайн следующего ухода. Оба метода вызываются ПОД mutex_.
    bool pruneExpiredQueueLocked();
    std::optional<std::chrono::steady_clock::time_point> nextQueuePatienceDeadlineLocked() const;
    // Перевести банкомат в ТО. Вызывается ПОД mutex_: сразу, если клиентов нет
    // на обслуживании, либо после доработки текущего клиента.
    void beginMaintenanceLocked(std::optional<int> durationSeconds);
    // «Занятое» прерываемое ожидание realDurationSec ЧИСТОГО времени работы
    // (сердце §6.2, общее для подхода и обслуживания): на паузе честно замирает
    // (слайс закрыт — прогресс для снимков заморожен), просыпается на дедлайны
    // терпения (перетерпевшие выкидываются из очереди), прерывается остановкой.
    // Прогресс публикуется в пару (servedRealSecOut, sliceStartOut) — snapshot()
    // читает её под тем же mutex_. Вызывается из run() ПОД уже захваченным
    // lock; возвращает чистое отработанное реальное время (== realDurationSec
    // при штатном завершении, меньше — если прервались остановкой).
    double busyWaitLocked(std::unique_lock<std::mutex>& lock, double realDurationSec,
                          double& servedRealSecOut,
                          std::optional<std::chrono::steady_clock::time_point>& sliceStartOut);
    // Решение «уйти/остаться» для всех в очереди при старте ТО (§4.5). Вызывается
    // из потока обслуживания, ПОД уже захваченным mutex_ (потому «Locked»).
    void applyMaintenanceRenegingLocked();

    // Тела снимков БЕЗ захвата мьютекса (вызывающий уже держит mutex_). Публичные
    // snapshot()/statsSnapshot()/queueSnapshot()/operations()/allClientsProcessed()
    // — тонкие обёртки, берущие лок и делегирующие сюда; fullSnapshot() вызывает
    // все пятеро под ОДНИМ локом с ОДНИМ Clock::now(). now передаётся снаружи,
    // чтобы все части общего снимка были согласованы на один миг.
    AtmSnapshot snapshotLocked(std::chrono::steady_clock::time_point now) const;
    StatsSnapshot statsSnapshotLocked(std::chrono::steady_clock::time_point now) const;
    std::vector<ClientSnapshot> queueSnapshotLocked(std::chrono::steady_clock::time_point now) const;
    std::vector<OperationRecord> operationsLocked(const OperationFilter& filter) const;
    bool allClientsProcessedLocked() const;

    Config cfg_;

    // Один мьютекс на всё изменяемое общее состояние (§6.1). Модель «один
    // писатель — много читателей» сохраняется логически; технически берём
    // обычный std::mutex, а не shared_mutex.
    //
    // ПОЧЕМУ не shared_mutex + condition_variable_any (§6.2, §12): на ряде версий
    // MSVC STL связка condition_variable_any + shared_mutex + stop_token/jthread
    // падает в рантайме («unlock of unowned mutex», порча стека). ТЗ §12 прямо
    // допускает замену jthread/stop_token на std::thread + флаг. Поэтому здесь —
    // только базовые, железобетонные примитивы: std::mutex + std::condition_variable.
    // Критические секции коротки (копирование снимка), так что сериализация
    // читателей на масштабе этого симулятора незаметна, а требование §4.3 (чтение
    // не ждёт долгую задержку обслуживания) выполняется: во время обслуживания
    // лок ОТПУЩЕН (cv.wait его освобождает).
    mutable std::mutex mutex_;

    // Режим работы — атомарный: его читают в предикатах ожидания и в снимках.
    std::atomic<AtmState> state_{AtmState::Idle};

    // На wakeUp_ поток обслуживания «дремлет с условием» время обслуживания и
    // ждёт работы (см. §6.2 и run()). Остановка — через requestStop(): он под
    // mutex_ выставляет state_=Stopped и вызывает notify_all(); предикаты всех
    // ожиданий проверяют state_==Stopped. Это канонический паттерн без потери
    // пробуждений (изменение состояния и его проверка упорядочены общим mutex_).
    std::condition_variable wakeUp_;

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

    // Распределения генерации клиентов. Их параметры фиксированы конфигом на
    // весь прогон, поэтому строим ОДИН раз в конструкторе, а не заново на
    // каждого клиента (discrete_distribution ещё и аллоцирует вектор весов).
    // Они stateless (состояния между вызовами не хранят), так что вынос в поля
    // НЕ меняет последовательность чисел из arrivalRng_ — прогон воспроизводим
    // бит-в-бит. Трогает их ТОЛЬКО поток прихода (makeClient), как и arrivalRng_.
    std::discrete_distribution<int> opDist_;
    std::uniform_int_distribution<std::int64_t> amountDist_;
    std::uniform_int_distribution<int> patienceDist_;

    std::optional<Client> currentClient_;  // кого обслуживаем сейчас (защищён mutex_)

    // Прогресс ТЕКУЩЕГО обслуживания — для снимков «что делает клиент» (§4.8).
    // Пишет ТОЛЬКО поток обслуживания и ТОЛЬКО под mutex_; snapshot() читает
    // под ним же. Прогресс не хранится готовым числом — он вычисляется в
    // snapshot() из этих трёх полей, чтобы расти НЕПРЕРЫВНО, а не скачками по
    // пробуждениям потока обслуживания (между пробуждениями тот спит с
    // отпущенным локом и ничего обновлять не может):
    //   отработано_реальных_сек = serviceServedRealSec_
    //                           + (сейчас - serviceSliceStart_, если слайс идёт)
    //   прогресс = отработано / (servicePlannedModelSec_ / time_scale)
    // На паузе слайс не идёт (serviceSliceStart_ пуст) — прогресс замирает,
    // как и таймер службы (§4.6). Вне обслуживания planned == 0.
    double servicePlannedModelSec_{0.0};  // полная длительность (модельные сек.)
    double serviceServedRealSec_{0.0};    // чистое реальное время ДО текущего слайса
    std::optional<std::chrono::steady_clock::time_point> serviceSliceStart_;  // начало слайса
    // Прогресс ПОДХОДА к банкомату (clients.walk_seconds) — та же механика
    // слайсов, что у прогресса обслуживания выше, но своя тройка полей: фазы
    // строго последовательны (сначала подход, потом обслуживание), и снимок
    // различает их по тому, какой planned сейчас ненулевой.
    double approachPlannedModelSec_{0.0};  // полное время подхода (модельные сек.)
    double approachServedRealSec_{0.0};    // чистое реальное время ДО текущего слайса
    std::optional<std::chrono::steady_clock::time_point> approachSliceStart_;
    std::optional<OperationType> lastCashMove_;  // направление посл. движения кассы
    std::uint64_t totalServed_{0};
    std::uint64_t totalLeft_{0};              // всего ушли (терпение + ТО)
    std::uint64_t totalLeftMaintenance_{0};   // из них — ушли из-за ТО
    std::size_t maxQueueLen_{0};

    // Режим ТО (§4.5): дедлайн авто-завершения и одноразовый флаг «применить
    // решение уйти/остаться к очереди». Оба под mutex_.
    std::chrono::steady_clock::time_point maintenanceDeadline_{};
    bool maintenanceRenegePending_{false};
    bool maintenanceStartPending_{false};  // ТО запрошено, но текущий клиент дорабатывает
    std::optional<int> pendingMaintenanceDurationSeconds_;

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

    Logger* logger_{nullptr};          // технический лог (может быть nullptr)
    bool lowCashLogged_{false};        // предупреждение о низкой кассе — один раз
};

}  // namespace atmsim
