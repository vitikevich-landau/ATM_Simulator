#include "atmsim/console/FrameDiffer.hpp"

#include <utility>  // std::move

#include "atmsim/console/Terminal.hpp"

namespace atmsim {

std::string FrameDiffer::diff(std::vector<std::string> lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // Строка не изменилась с прошлого кадра — пропускаем.
        if (valid_ && i < prev_.size() && prev_[i] == lines[i]) continue;
        out += ansi::moveTo(static_cast<int>(i) + 1, 1);
        out += lines[i];
        out += ansi::clearToLineEnd();
    }
    // Кадр стал короче прошлого (при постоянной высоте §4.8.5 не случается,
    // но контракт диффера не должен на это полагаться) — стираем хвост.
    if (valid_) {
        for (std::size_t i = lines.size(); i < prev_.size(); ++i) {
            out += ansi::moveTo(static_cast<int>(i) + 1, 1);
            out += ansi::clearToLineEnd();
        }
    }
    prev_ = std::move(lines);
    valid_ = true;
    return out;
}

}  // namespace atmsim
