#include "atmsim/config/ConfigLoader.hpp"

#include <cmath>  // std::isfinite (валидация walk_seconds)
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>  // единственное место, где проект зависит от JSON-библиотеки

namespace atmsim {
namespace {

using nlohmann::json;

// --- разбор строковых значений в перечисления ------------------------------
ArrivalMode parseArrivalMode(const std::string& s) {
    if (s == "batch") return ArrivalMode::Batch;
    if (s == "poisson") return ArrivalMode::Poisson;
    throw ConfigError("неизвестный arrival_mode: '" + s + "' (ожидалось batch или poisson)");
}

ServiceDistribution parseDistribution(const std::string& s) {
    if (s == "uniform") return ServiceDistribution::Uniform;
    if (s == "normal") return ServiceDistribution::Normal;
    if (s == "exponential") return ServiceDistribution::Exponential;
    throw ConfigError("неизвестное распределение времени обслуживания: '" + s +
                      "' (ожидалось uniform, normal или exponential)");
}

// --- проверка согласованности значений после чтения ------------------------
void validate(const Config& c) {
    const auto& w = c.clients.weights;
    if (w.checkBalance < 0 || w.withdraw < 0 || w.deposit < 0) {
        throw ConfigError("веса операций не могут быть отрицательными");
    }
    if (w.checkBalance + w.withdraw + w.deposit <= 0.0) {
        throw ConfigError("сумма весов операций должна быть больше нуля");
    }
    // min >= 1, а не >= 0: суммы из этого диапазона идут в снятие/внесение, а те
    // при amount <= 0 дают InvalidAmount (Operation.cpp). Ноль в диапазоне породил
    // бы клиентов с гарантированно проваленной операцией. (CheckBalance ставит
    // сумму 0 сам, в обход диапазона, поэтому его это не затрагивает.)
    if (c.clients.amountRange.min < 1 || c.clients.amountRange.min > c.clients.amountRange.max) {
        throw ConfigError("некорректный amount_range (нужно 1 <= min <= max)");
    }
    if (c.clients.patienceSeconds.min < 0 ||
        c.clients.patienceSeconds.min > c.clients.patienceSeconds.max) {
        throw ConfigError("некорректный patience_seconds (нужно 0 <= min <= max)");
    }
    // Время подхода уходит в uniform_real_distribution(min, max), precondition
    // которого — конечные min <= max; отрицательное или бесконечное время
    // бессмысленно (inf может просочиться из отдельно взятого JSON-значения,
    // даже если литеральное переполнение ловится на разборе). 0/0 — легальное
    // «подход мгновенный» (поведение до фичи подхода).
    if (!std::isfinite(c.clients.walkSeconds.min) || !std::isfinite(c.clients.walkSeconds.max) ||
        c.clients.walkSeconds.min < 0.0 ||
        c.clients.walkSeconds.min > c.clients.walkSeconds.max) {
        throw ConfigError("некорректный walk_seconds (нужно конечные 0 <= min <= max)");
    }
    if (c.clients.arrivalRatePerMinute <= 0.0) {
        throw ConfigError("arrival_rate_per_minute должен быть больше нуля");
    }
    // count идёт прямо в vector::reserve (по счёту и записи реестра на клиента).
    // Верхняя граница — защита от абсурдного значения: без неё count ~1e9 роняет
    // reserve через std::bad_alloc/std::length_error, а это НЕ ConfigError и
    // main его не ловит -> процесс падает в std::terminate вместо осмысленной
    // ошибки конфигурации.
    constexpr int kMaxClients = 100'000'000;
    if (c.clients.count < 0) {
        throw ConfigError("clients.count не может быть отрицательным");
    }
    if (c.clients.count > kMaxClients) {
        throw ConfigError("clients.count слишком велик (максимум " +
                          std::to_string(kMaxClients) + ")");
    }
    // Начальный баланс счёта, как и прочие денежные поля, не может быть
    // отрицательным: иначе завёлся бы счёт «в минусе», у которого любое снятие
    // сразу даёт InsufficientFunds — бессмысленный (хоть и не аварийный) конфиг.
    if (c.clients.initialBalance < 0) {
        throw ConfigError("clients.initial_balance не может быть отрицательным");
    }
    if (c.atm.initialCash < 0 || c.atm.lowCashThreshold < 0) {
        throw ConfigError("значения кассы не могут быть отрицательными");
    }
    // Число знаков после разделителя уходит в построение строки денег (дополнение
    // нулями/усечение). Отрицательное бессмысленно, а абсурдно большое раздуло бы
    // строку сотнями нулей — ограничиваем разумным диапазоном.
    if (c.atm.currencyOverride.decimals &&
        (*c.atm.currencyOverride.decimals < 0 || *c.atm.currencyOverride.decimals > 6)) {
        throw ConfigError("atm.currency_format.decimals должен быть в диапазоне 0..6");
    }
    if (c.serviceTime.minSeconds < 0 || c.serviceTime.meanSeconds < 0 ||
        c.serviceTime.stddevSeconds < 0) {
        throw ConfigError("параметры времени обслуживания не могут быть отрицательными");
    }
    if (c.serviceTime.distribution == ServiceDistribution::Uniform &&
        c.serviceTime.minSeconds > c.serviceTime.maxSeconds) {
        throw ConfigError("для uniform нужно min_seconds <= max_seconds");
    }
    // Для нормального распределения std::normal_distribution требует СТРОГО
    // положительный stddev — это precondition стандарта ([rand.dist.norm.normal]),
    // при нарушении поведение не определено. Ноль дал бы вырожденное распределение
    // (всегда mean), отрицательный — формальный UB. Ловим здесь, до движка.
    if (c.serviceTime.distribution == ServiceDistribution::Normal &&
        c.serviceTime.stddevSeconds <= 0.0) {
        throw ConfigError("для normal нужно stddev_seconds > 0");
    }
    // Для normal и exponential среднее время обслуживания — параметр распределения
    // (normal: μ; exponential: 1/λ, откуда λ = 1/mean). При mean_seconds == 0
    // конфиг раньше проходил, но ExponentialServiceTime молча подменял λ=1 (факт.
    // обслуживание шло с другим средним, чем задано), а expectedServiceSeconds
    // возвращал 0 -> mu=0 -> теоретическая загрузка ρ=λ/μ печаталась нулевой при
    // реально идущем обслуживании. Требуем строго > 0.
    if ((c.serviceTime.distribution == ServiceDistribution::Normal ||
         c.serviceTime.distribution == ServiceDistribution::Exponential) &&
        c.serviceTime.meanSeconds <= 0.0) {
        throw ConfigError("для normal/exponential нужно mean_seconds > 0");
    }
    // Верхняя граница: период перерисовки считается как 1000/refresh_hz мс
    // (LiveRenderer и просмотр очереди) — при refresh_hz > 1000 он обнулился бы,
    // и циклы отрисовки крутились бы без сна (100% CPU на ядро). 60 Гц хватает
    // любому терминалу с запасом (в конфиге рекомендовано 2-10).
    if (c.ui.refreshHz < 1 || c.ui.refreshHz > 60) {
        throw ConfigError("ui.refresh_hz должен быть в диапазоне 1..60");
    }
    // Границы сцены. fps: ниже 5 анимация разваливается на слайды, выше 30 —
    // бессмысленная нагрузка (см. docs/scene_frame_budget.md). rows: ниже 10
    // не влезает корпус банкомата с подписями, выше 14 сцена съедает таблицу.
    if (c.ui.sceneFps < 5 || c.ui.sceneFps > 30) {
        throw ConfigError("ui.scene_fps должен быть в диапазоне 5..30");
    }
    if (c.ui.sceneRows < 10 || c.ui.sceneRows > 14) {
        throw ConfigError("ui.scene_rows должен быть в диапазоне 10..14");
    }
    if (c.simulation.timeScale <= 0.0) {
        throw ConfigError("time_scale должен быть больше нуля");
    }
    // renege_probability уходит прямо в std::bernoulli_distribution, precondition
    // которого — вероятность в диапазоне [0, 1]; вне диапазона поведение стандартом
    // не определено. Проверяем здесь, чтобы битый конфиг падал осмысленно.
    if (c.maintenance.renegeProbability < 0.0 || c.maintenance.renegeProbability > 1.0) {
        throw ConfigError("maintenance.renege_probability должен быть в диапазоне [0, 1]");
    }
}

}  // namespace

Config ConfigLoader::loadFromString(const std::string& jsonText) {
    Config c;  // начинаем со значений по умолчанию (см. Config.hpp)

    // 1) Разбор самого JSON. Синтаксическая ошибка -> ConfigError.
    //    Аргументы parse: (текст, callback=nullptr, allow_exceptions=true,
    //    ignore_comments=true). Последний ВКЛючает поддержку комментариев //… и
    //    /*…*/ — чтобы config/default_config.json можно было подробно
    //    прокомментировать (строгий JSON комментарии запрещает). На чтение
    //    значений это не влияет: комментарии просто пропускаются.
    json j;
    try {
        j = json::parse(jsonText, nullptr, true, true);
    } catch (const json::exception& e) {
        // Не только parse_error: переполнение числового литерала (например,
        // «3.0e999») nlohmann кидает как json::out_of_range, который от
        // parse_error НЕ наследуется — без широкого catch он улетал бы в main
        // как «непредвиденная ошибка» вместо осмысленной ошибки конфигурации.
        throw ConfigError(std::string("некорректный JSON: ") + e.what());
    }
    if (!j.is_object()) {
        throw ConfigError("корень конфигурации должен быть объектом JSON");
    }

    // 2) Чтение значений. Отсутствующий ключ -> остаётся значение по умолчанию
    //    (value(key, default)). Неверный ТИП значения -> json::type_error,
    //    который мы переводим в ConfigError.
    try {
        if (j.contains("atm")) {
            const auto& a = j.at("atm");
            c.atm.initialCash = a.value("initial_cash", c.atm.initialCash);
            c.atm.lowCashThreshold = a.value("low_cash_threshold", c.atm.lowCashThreshold);
            c.atm.currency = a.value("currency", c.atm.currency);
            // Необязательное переопределение формата валюты. Заданные поля
            // перекрывают встроенную таблицу (resolveCurrencyFormat) при выводе.
            // symbol_position: "before"/"after"; group_separator "" — без группировки.
            if (a.contains("currency_format")) {
                const auto& cf = a.at("currency_format");
                CurrencyOverride ov;
                if (cf.contains("symbol"))
                    ov.symbol = cf.at("symbol").get<std::string>();
                if (cf.contains("symbol_position")) {
                    // Как и прочие строковые enum-поля конфига (arrival_mode,
                    // distribution), неизвестное написание не глотаем молча (иначе
                    // опечатка «befor» тихо дала бы "after" и изменила весь вывод
                    // сумм), а отвергаем с понятной ошибкой.
                    const std::string pos = cf.at("symbol_position").get<std::string>();
                    if (pos != "before" && pos != "after") {
                        throw ConfigError("atm.currency_format.symbol_position должен быть "
                                          "\"before\" или \"after\" (получено: '" + pos + "')");
                    }
                    ov.symbolBefore = (pos == "before");
                }
                if (cf.contains("space_between"))
                    ov.spaceBetween = cf.at("space_between").get<bool>();
                if (cf.contains("group_separator"))
                    ov.groupSeparator = cf.at("group_separator").get<std::string>();
                if (cf.contains("decimal_separator"))
                    ov.decimalSeparator = cf.at("decimal_separator").get<std::string>();
                if (cf.contains("decimals"))
                    ov.decimals = cf.at("decimals").get<int>();
                c.atm.currencyOverride = ov;
            }
        }

        if (j.contains("clients")) {
            const auto& cl = j.at("clients");
            c.clients.arrivalMode = parseArrivalMode(cl.value("arrival_mode", std::string("poisson")));
            c.clients.arrivalRatePerMinute =
                cl.value("arrival_rate_per_minute", c.clients.arrivalRatePerMinute);
            c.clients.count = cl.value("count", c.clients.count);
            c.clients.initialBalance = cl.value("initial_balance", c.clients.initialBalance);

            if (cl.contains("operation_weights")) {
                const auto& w = cl.at("operation_weights");
                c.clients.weights.checkBalance = w.value("check_balance", c.clients.weights.checkBalance);
                c.clients.weights.withdraw = w.value("withdraw", c.clients.weights.withdraw);
                c.clients.weights.deposit = w.value("deposit", c.clients.weights.deposit);
            }
            if (cl.contains("amount_range")) {
                const auto& r = cl.at("amount_range");
                c.clients.amountRange.min = r.value("min", c.clients.amountRange.min);
                c.clients.amountRange.max = r.value("max", c.clients.amountRange.max);
            }
            if (cl.contains("patience_seconds")) {
                const auto& r = cl.at("patience_seconds");
                c.clients.patienceSeconds.min = r.value("min", c.clients.patienceSeconds.min);
                c.clients.patienceSeconds.max = r.value("max", c.clients.patienceSeconds.max);
            }
            if (cl.contains("walk_seconds")) {
                const auto& r = cl.at("walk_seconds");
                c.clients.walkSeconds.min = r.value("min", c.clients.walkSeconds.min);
                c.clients.walkSeconds.max = r.value("max", c.clients.walkSeconds.max);
            }
        }

        if (j.contains("service_time")) {
            const auto& s = j.at("service_time");
            c.serviceTime.distribution =
                parseDistribution(s.value("distribution", std::string("normal")));
            if (s.contains("params")) {
                const auto& p = s.at("params");
                c.serviceTime.meanSeconds = p.value("mean_seconds", c.serviceTime.meanSeconds);
                c.serviceTime.stddevSeconds = p.value("stddev_seconds", c.serviceTime.stddevSeconds);
                c.serviceTime.minSeconds = p.value("min_seconds", c.serviceTime.minSeconds);
                c.serviceTime.maxSeconds = p.value("max_seconds", c.serviceTime.maxSeconds);
            }
        }

        if (j.contains("maintenance")) {
            const auto& m = j.at("maintenance");
            c.maintenance.renegeProbability = m.value("renege_probability", c.maintenance.renegeProbability);
            c.maintenance.defaultDurationSeconds =
                m.value("default_duration_seconds", c.maintenance.defaultDurationSeconds);
            c.maintenance.arrivalsContinue =
                m.value("arrivals_continue_during_maintenance", c.maintenance.arrivalsContinue);
        }

        if (j.contains("simulation")) {
            const auto& s = j.at("simulation");
            c.simulation.randomSeed = s.value("random_seed", c.simulation.randomSeed);
            c.simulation.timeScale = s.value("time_scale", c.simulation.timeScale);
        }

        if (j.contains("ui")) {
            const auto& u = j.at("ui");
            c.ui.liveMode = u.value("live_mode", c.ui.liveMode);
            c.ui.refreshHz = u.value("refresh_hz", c.ui.refreshHz);
            c.ui.eventsTail = u.value("events_tail", c.ui.eventsTail);
            c.ui.showProgressBars = u.value("show_progress_bars", c.ui.showProgressBars);
            c.ui.color = u.value("color", c.ui.color);
            c.ui.scene = u.value("scene", c.ui.scene);
            c.ui.sceneFps = u.value("scene_fps", c.ui.sceneFps);
            c.ui.sceneRows = u.value("scene_rows", c.ui.sceneRows);
            c.ui.sceneEffects = u.value("scene_effects", c.ui.sceneEffects);
        }

        if (j.contains("logging")) {
            const auto& l = j.at("logging");
            c.logging.level = l.value("level", c.logging.level);
            c.logging.file = l.value("file", c.logging.file);
        }
    } catch (const json::exception& e) {
        // Неверный тип значения и т.п. — переводим в осмысленную ошибку конфигурации.
        throw ConfigError(std::string("ошибка чтения значения конфигурации: ") + e.what());
    }

    // 3) Проверка согласованности.
    validate(c);
    return c;
}

Config ConfigLoader::loadFromFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Ищем файл конфигурации. Сначала — по указанному пути (как есть). Если его
    // там нет, а путь ОТНОСИТЕЛЬНЫЙ (например, "config/default_config.json"),
    // поднимаемся вверх по дереву каталогов от текущей рабочей папки и пробуем
    // найти его там. Это нужно, потому что при запуске из IDE рабочая папка —
    // подкаталог сборки (out/build/x64-Debug), а config/ лежит в корне проекта.
    fs::path resolved(path);
    if (!fs::exists(resolved, ec) && resolved.is_relative()) {
        for (fs::path dir = fs::current_path(ec); ; dir = dir.parent_path()) {
            const fs::path candidate = dir / path;
            if (fs::exists(candidate, ec)) {
                resolved = candidate;
                break;
            }
            if (dir == dir.parent_path()) break;  // дошли до корня файловой системы
        }
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        throw ConfigError("не удалось открыть файл конфигурации: " + path +
                          " (искал по этому пути, а также вверх по каталогам от текущей папки)");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadFromString(ss.str());
}

}  // namespace atmsim
