// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  main() — тонкий оркестратор: загрузить конфиг и логгер, поставить обработчики
//  сигналов, прогнать цикл симуляций (команда restart -> новый прогон) и
//  напечатать итог. «Мясистая» логика вынесена в отдельные сущности:
//    * ScopedEngineThreads — запуск/остановка рабочих потоков движка (RAII);
//    * runSingleSimulation() — один прогон «от и до»;
//    * printStartupBanner/printRestartDivider/printFinalReport — презентация.
// ============================================================================

#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

#include "atmsim/Version.hpp"
#include "atmsim/config/ConfigLoader.hpp"
#include "atmsim/console/AdminConsole.hpp"
#include "atmsim/console/Signals.hpp"
#include "atmsim/console/SplashScreen.hpp"
#include "atmsim/engine/AtmEngine.hpp"
#include "atmsim/engine/ScopedEngineThreads.hpp"
#include "atmsim/reporting/Logger.hpp"
#include "atmsim/utils/ru_console.hpp"

using namespace atmsim;

namespace {

// Итог одного прогона: чем закончился (выход/перезапуск) и финальные снимки для
// отчёта. Снимки снимаются уже ПОСЛЕ остановки потоков (см. runSingleSimulation).
struct RunResult {
    AdminConsole::RunOutcome outcome;
    StatsSnapshot stats;
    AtmSnapshot atm;
    UiConfig ui;  // ui-настройки после прогона (scene on|off переживает restart)
};

// Баннер при старте: имя/версия, путь конфига, число клиентов, файл лога.
void printStartupBanner(const Config& cfg, const std::string& path, bool logOk) {
    std::cout << kProjectName << " v" << kVersion << '\n';
    std::cout << "Конфиг: " << path << "  |  клиентов: " << cfg.clients.count
              << "  |  лог: " << (logOk ? cfg.logging.file : std::string("(отключён)"))
              << "\n";
}

// Короткая строка-разделитель между прогонами при restart (полный отчёт — только
// на финальном выходе, см. printFinalReport).
void printRestartDivider(int runIndex, const StatsSnapshot& st) {
    std::cout << "\n── Прогон #" << runIndex << " завершён (обслужено "
              << st.served << ", ушли " << st.left << "). Перезапуск… ──\n";
}

// Итоговая статистика СМО при выходе (§4.4, §10). Возню с float-флагами держим
// здесь, чтобы она не «протекала» в остальной вывод.
void printFinalReport(const StatsSnapshot& st, const AtmSnapshot& s, const Config& cfg) {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n=== Итоговая статистика ===\n";
    std::cout << "Обслужено:            " << st.served << " / " << cfg.clients.count
              << " (всего клиентов)\n";
    std::cout << "Ушли (всего):         " << st.left
              << "  (из них по ТО: " << st.renegedByMaintenance << ")\n";
    std::cout << "Среднее ожидание:     " << st.avgWaitSeconds << " c\n";
    std::cout << "Среднее обслуживание: " << st.avgServiceSeconds << " c\n";
    std::cout << "Макс. длина очереди:  " << st.maxQueueLength << '\n';
    std::cout << std::setprecision(2) << "Загрузка ρ = λ/μ:     " << st.rhoTheoretical << '\n';
    std::cout << "Факт. загрузка:       " << st.utilization << '\n';
    std::cout << std::setprecision(0) << "Аптайм:               " << st.uptimeSeconds << " c\n";
    std::cout.unsetf(std::ios::floatfield);
    const CurrencyFormat cur = resolveCurrencyFormat(cfg.atm.currency, cfg.atm.currencyOverride);
    std::cout << "Касса:                " << formatMoney(s.cashboxBalance, cur) << '\n';
}

// Один прогон симуляции «от и до»: движок + его потоки (RAII) + консоль. Потоки
// гарантированно останавливаются и присоединяются — даже если AdminConsole::run()
// бросит исключение: тогда сработает деструктор ScopedEngineThreads, а не
// std::terminate на joinable-потоке. Финальные снимки снимаем ПОСЛЕ остановки —
// для детерминированного итога при фиксированном seed (§5).
RunResult runSingleSimulation(const Config& runCfg, Logger& logger) {
    // Экран загрузки «самотест банкомата» — ДО создания движка и его потоков:
    // симуляция (включая отсчёт аптайма и приход первого клиента с интервалом
    // 0.0) начинается только после заставки, поэтому и при рестарте старт
    // прогона виден с самого начала. Вне TTY заставки нет; клавиша пропускает.
    showSplash(runCfg);

    AtmEngine engine(runCfg, &logger);
    ScopedEngineThreads threads(engine);  // спавн engine.run()/generateArrivals()

    AdminConsole console(engine, runCfg);
    AdminConsole::RunOutcome outcome = console.run();  // блокирует до stop/EOF/сигнала/restart

    if (shutdownRequested()) {
        logger.info("Получен сигнал остановки (SIGINT/SIGTERM)");
        std::cout << "\n(получен сигнал остановки — завершаюсь плавно)\n";
        outcome = AdminConsole::RunOutcome::Quit;  // сигнал важнее restart
    }

    threads.stop();  // плавная остановка + join ДО снятия финальных снимков
    return RunResult{outcome, engine.statsSnapshot(), engine.snapshot(), console.config().ui};
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/default_config.json";

    try {
        const Config cfg = ConfigLoader::loadFromFile(path);

        // Технический лог (§10) — отдельно от бизнес-журнала операций. Если файл
        // не открылся (нет прав, битый путь), Logger молча глотает записи —
        // предупреждаем в stderr, но не падаем: лог вспомогательный, симуляции он
        // не мешает.
        // Уровень лога: неизвестное значение НЕ глотаем молча в Info — сначала
        // явно предупреждаем в stderr (tryParseLevel отличает неизвестное от
        // валидного), затем применяем безопасный дефолт.
        const std::optional<LogLevel> parsedLevel = Logger::tryParseLevel(cfg.logging.level);
        if (!parsedLevel) {
            std::cerr << "Предупреждение: неизвестный уровень логирования '" << cfg.logging.level
                      << "' — использую info (допустимо: debug | info | warn | error)\n";
        }
        Logger logger(cfg.logging.file, parsedLevel.value_or(LogLevel::Info));
        if (!logger.ok()) {
            std::cerr << "Предупреждение: не удалось открыть файл лога '"
                      << cfg.logging.file << "' — техническое логирование отключено\n";
        }
        logger.info("Конфигурация загружена: " + path);

        printStartupBanner(cfg, path, logger.ok());
        installSignalHandlers();  // SIGINT/SIGTERM -> плавная остановка (§4.6)

        // Цикл прогонов. Команда restart НЕ дожидается конца прогона: она
        // прерывает его, пересоздаёт движок и запускает заново, ПЕРЕЧИТАВ конфиг с
        // диска — правки файла (валюта, число клиентов, тайминги) применяются без
        // перезапуска программы. Выходим на Quit.
        Config baseCfg = cfg;            // «текущий» конфиг: обновляется при reload
        UiConfig uiState = baseCfg.ui;   // ui-переключения на лету (scene on|off)
        for (int runIndex = 1; ; ++runIndex) {
            Config runCfg = baseCfg;
            runCfg.ui = uiState;

            const RunResult r = runSingleSimulation(runCfg, logger);
            uiState = r.ui;  // если reload не случится/не удастся — переносим runtime-ui дальше
            logger.info("Итог прогона #" + std::to_string(runIndex) + ": обслужено " +
                        std::to_string(r.stats.served) + ", ушли " + std::to_string(r.stats.left));

            if (r.outcome != AdminConsole::RunOutcome::Restart) {
                printFinalReport(r.stats, r.atm, baseCfg);
                break;
            }

            printRestartDivider(runIndex, r.stats);
            // Перечитываем конфиг с диска перед следующим прогоном. Успех: файл —
            // источник истины, в т.ч. для ui (scene и пр.); применяется и тот же
            // random_seed из файла — при неизменном seed прогон воспроизводится
            // точно (§5), поменяйте random_seed для нового случайного сценария.
            // Ошибку чтения (файл удалён/битый JSON) НЕ роняем: предупреждаем и
            // продолжаем с прежним конфигом.
            try {
                baseCfg = ConfigLoader::loadFromFile(path);
                uiState = baseCfg.ui;
                // Применяем и настройки лога из перечитанного конфига: без этого
                // изменённые logging.file/level игнорировались бы, а прогон уже шёл
                // бы по новому конфигу — технический лог рассинхронился бы с ним.
                // reconfigure дописывает (append), логи прошлых прогонов не теряем.
                const std::optional<LogLevel> lvl = Logger::tryParseLevel(baseCfg.logging.level);
                if (!lvl) {
                    std::cerr << "Предупреждение: неизвестный уровень логирования '"
                              << baseCfg.logging.level << "' — использую info\n";
                }
                logger.reconfigure(baseCfg.logging.file, lvl.value_or(LogLevel::Info));
                if (!logger.ok()) {
                    std::cerr << "Предупреждение: не удалось открыть файл лога '"
                              << baseCfg.logging.file << "' после перечитывания — лог отключён\n";
                }
                logger.info("Конфиг перечитан с диска перед прогоном #" +
                            std::to_string(runIndex + 1));
                std::cout << "Конфиг перечитан: " << path << "  |  клиентов: "
                          << baseCfg.clients.count << "  |  валюта: " << baseCfg.atm.currency << '\n';
            } catch (const ConfigError& e) {
                logger.warn(std::string("Перечитать конфиг при restart не удалось: ") + e.what());
                std::cerr << "Предупреждение: не удалось перечитать конфиг '" << path << "': "
                          << e.what() << "\n  -> продолжаю с прежними настройками\n";
            }
        }
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        // Любое иное исключение (напр. std::bad_alloc/std::length_error из
        // reserve в конструкторе движка при экстремальном конфиге, ошибки ФС) —
        // печатаем и выходим с ненулевым кодом, а не роняем процесс в
        // std::terminate. Потоки движка к этому моменту уже сняты RAII-обёрткой
        // ScopedEngineThreads, поэтому раскрутка стека здесь безопасна.
        std::cerr << "Непредвиденная ошибка: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Непредвиденная ошибка неизвестного типа\n";
        return 1;
    }
    return 0;
}
