#pragma once
// ============================================================================
//  ConfigLoader.hpp — загрузка и валидация конфигурации из JSON.
// ============================================================================
#include <stdexcept>
#include <string>

#include "atmsim/config/Config.hpp"

namespace atmsim {

// Ошибка конфигурации — это программная/входная ошибка (§6.5): битый JSON,
// отсутствующий файл, недопустимое значение. Ловится в main(), приводит к
// корректному завершению с сообщением (в отличие от бизнес-исходов, которые
// возвращаются через OperationOutcome, а не бросаются).
struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// \brief Загрузчик конфигурации из JSON с валидацией значений.
class ConfigLoader {
public:
    // Читает файл целиком и разбирает его. Бросает ConfigError, если файла нет,
    // JSON некорректен или значения не проходят валидацию.
    static Config loadFromFile(const std::string& path);

    // Разбирает конфигурацию прямо из строки (удобно для тестов).
    static Config loadFromString(const std::string& jsonText);
};

}  // namespace atmsim
