#pragma once
// ============================================================================
//  CommandParser.hpp — разбор строки команды администратора в структуру Command.
//
//  Парсер отделён от консоли, чтобы его можно было покрыть юнит-тестами (§11)
//  независимо от ввода/вывода: даём строку — получаем разобранную команду или
//  понятную ошибку.
// ============================================================================
#include <cstddef>
#include <optional>
#include <string>

#include "atmsim/core/Types.hpp"

namespace atmsim {

enum class CommandType {
    Empty,       // пустая строка
    Help,
    Status,
    Queue,
    Client,      // client <id>
    Balance,     // balance <id>
    Operations,  // operations [--client N] [--type T] [--last N]
    Atm,
    Stats,
    Pause,
    Resume,
    MaintenanceStart,  // maintenance start [сек]
    MaintenanceStop,   // maintenance stop
    Stop,        // stop / exit / quit
    Export,      // export <file>
    Unknown      // нераспознанная команда
};

struct Command {
    CommandType type{CommandType::Empty};
    std::optional<ClientId> clientId;      // для client/balance и operations --client
    std::optional<std::size_t> last;       // operations --last N
    std::optional<OperationType> opType;   // operations --type T
    std::optional<int> seconds;            // maintenance start [сек]
    std::string filename;                  // export <file>
    std::string word;                      // исходное первое слово (для сообщений)
    std::string error;                     // непусто, если разбор не удался
};

// Разбирает строку. Никогда не бросает исключений: об ошибке сообщает через
// поле Command::error (ошибка ввода администратора — не аварийная ситуация, §6.5).
Command parseCommand(const std::string& line);

}  // namespace atmsim
