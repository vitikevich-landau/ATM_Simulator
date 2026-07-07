#include "atmsim/config/ConfigLoader.hpp"

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
    if (c.clients.amountRange.min < 0 || c.clients.amountRange.min > c.clients.amountRange.max) {
        throw ConfigError("некорректный amount_range (нужно 0 <= min <= max)");
    }
    if (c.clients.patienceSeconds.min < 0 ||
        c.clients.patienceSeconds.min > c.clients.patienceSeconds.max) {
        throw ConfigError("некорректный patience_seconds (нужно 0 <= min <= max)");
    }
    if (c.clients.arrivalRatePerMinute <= 0.0) {
        throw ConfigError("arrival_rate_per_minute должен быть больше нуля");
    }
    if (c.clients.count < 0) {
        throw ConfigError("clients.count не может быть отрицательным");
    }
    if (c.atm.initialCash < 0 || c.atm.lowCashThreshold < 0) {
        throw ConfigError("значения кассы не могут быть отрицательными");
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
    if (c.ui.refreshHz < 1) {
        throw ConfigError("ui.refresh_hz должен быть >= 1");
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
    } catch (const json::parse_error& e) {
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
