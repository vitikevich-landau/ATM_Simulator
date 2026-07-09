#include "atmsim/console/scene/SceneCanvas.hpp"

#include <cassert>

#include "atmsim/console/Terminal.hpp"  // ansi::* — единственное место, где Tint встречает ANSI
#include "atmsim/console/scene/GlyphSet.hpp"

namespace atmsim::scene {
namespace {

// SGR-код цвета для Tint. Default не имеет кода — он выражается через reset.
const char* tintCode(Tint t) {
    switch (t) {
        case Tint::Grey:   return ansi::grey();
        case Tint::Red:    return ansi::red();
        case Tint::Green:  return ansi::green();
        case Tint::Yellow: return ansi::yellow();
        case Tint::Blue:   return ansi::blue();
        case Tint::Cyan:   return ansi::cyan();
        case Tint::Default: break;
    }
    return "";
}

}  // namespace

SceneCanvas::SceneCanvas(int width, int height)
    : width_(width < 1 ? 1 : width),
      height_(height < 1 ? 1 : height),
      cells_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {}

void SceneCanvas::clear() {
    for (Cell& c : cells_) c = Cell{};
}

void SceneCanvas::put(int x, int y, char32_t glyph, Tint tint, bool bold) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;  // клип по краям
    // Debug-сборка бьёт по рукам сразу (ошибка в спрайте/подписи видна на
    // первом же кадре теста), release тихо чинит глиф, сохраняя раскладку.
    assert(isAllowedGlyph(glyph) && "глиф вне белого списка GlyphSet (см. GlyphSet.hpp)");
    Cell& c = at(x, y);
    c.glyph = sanitizeGlyph(glyph);
    c.tint = tint;
    c.bold = bold;
}

void SceneCanvas::text(int x, int y, std::string_view utf8Text, Tint tint, bool bold) {
    if (y < 0 || y >= height_) return;
    int col = x;
    for (char32_t cp : utf8::decode(utf8Text)) {
        put(col, y, cp, tint, bold);
        ++col;
    }
}

void SceneCanvas::blit(int x, int y, const std::vector<std::string>& spriteRows, Tint tint,
                       bool bold) {
    for (std::size_t row = 0; row < spriteRows.size(); ++row) {
        const std::u32string glyphs = utf8::decode(spriteRows[row]);
        for (std::size_t colIdx = 0; colIdx < glyphs.size(); ++colIdx) {
            const char32_t cp = glyphs[colIdx];
            if (cp == U' ') continue;  // пробел спрайта = прозрачность
            put(x + static_cast<int>(colIdx), y + static_cast<int>(row), cp, tint, bold);
        }
    }
}

std::vector<std::string> SceneCanvas::toLines(bool color) const {
    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>(height_));
    for (int y = 0; y < height_; ++y) {
        std::string line;
        line.reserve(static_cast<std::size_t>(width_) * 3 + 16);
        // Текущее «открытое» состояние SGR в строке. Меняем его лениво:
        // подряд идущие клетки одного цвета выводятся одним прогоном без
        // повторных escape-кодов (run-length).
        Tint curTint = Tint::Default;
        bool curBold = false;
        for (int x = 0; x < width_; ++x) {
            const Cell& c = at(x, y);
            if (color && (c.tint != curTint || c.bold != curBold)) {
                // Смена состояния: сбрасываем предыдущее (если было не по
                // умолчанию) и открываем новое. Reset перед новым цветом нужен,
                // чтобы жирность не «текла» из прошлого прогона.
                if (curTint != Tint::Default || curBold) line += ansi::reset();
                if (c.bold) line += ansi::bold();
                if (c.tint != Tint::Default) line += tintCode(c.tint);
                curTint = c.tint;
                curBold = c.bold;
            }
            utf8::append(line, c.glyph);
        }
        if (color && (curTint != Tint::Default || curBold)) line += ansi::reset();
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace atmsim::scene
