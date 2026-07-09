#pragma once
// ============================================================================
//  SceneCanvas.hpp — клеточная канва анимированной сцены (этап 1 feature/scene).
//
//  Замена модели «строка = std::string с вкраплёнными ESC» для рисования
//  сцены: наложение спрайта поверх фона требует врезки по колонкам, а ручная
//  склейка подстрок с ANSI — источник багов усечения. Здесь всё проще:
//  канва — это сетка клеток {кодовая точка, цвет, жирность}. Спрайты и текст
//  ПИШУТСЯ В КЛЕТКИ (композитинг тривиален, края клипаются), а сериализатор
//  toLines() один раз собирает готовые строки с run-length сменой SGR-цвета.
//
//  Контракты (закреплены тестами):
//    * toLines() всегда возвращает ровно height() строк по ровно width()
//      кодовых точек — совместимо с fit() и инвариантом высоты кадра §4.8.5;
//    * при color=false в строках нет ни одного байта ESC (деградация не-TTY);
//    * в клетки попадают только глифы из GlyphSet (debug-assert в put(),
//      release — замена на '?'), поэтому «1 кодовая точка = 1 колонка».
//
//  Канва ничего не знает ни о движке, ни о терминале — это чистая структура
//  данных, юнит-тестируемая без TTY (по образцу composeLines()).
// ============================================================================
#include <string>
#include <string_view>
#include <vector>

namespace atmsim::scene {

// Цвет клетки. Умышленно НЕ сырые SGR-коды: сериализатор сам превращает Tint
// в ANSI (и не превращает ни во что при color=false). 16-цветной палитры
// достаточно; truecolor — кандидат в v2.
enum class Tint : unsigned char { Default, Grey, Red, Green, Yellow, Blue, Cyan };

class SceneCanvas {
public:
    // Канва фиксированного размера, заполненная пробелами. Размер выбирается
    // один раз при создании кадра сцены — менять его нельзя (инвариант §4.8.5:
    // высота кадра дашборда постоянна в пределах live-сессии).
    SceneCanvas(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    // Очистить: все клетки — пробел цвета Default.
    void clear();

    // Записать один глиф в клетку (x, y). Координаты вне канвы молча
    // игнорируются (клип). Запрещённый GlyphSet'ом глиф в debug роняет assert,
    // в release заменяется на '?'. Пробел здесь НЕПРОЗРАЧЕН — он затирает
    // клетку (для прозрачности используйте blit()).
    void put(int x, int y, char32_t glyph, Tint tint = Tint::Default, bool bold = false);

    // Записать строку текста слева направо начиная с (x, y): подписи, счётчики.
    // Выход за правый край клипается. Пробелы непрозрачны (текст затирает фон).
    void text(int x, int y, std::string_view utf8Text, Tint tint = Tint::Default,
              bool bold = false);

    // Наложить многострочный спрайт левым верхним углом в (x, y). Пробел в
    // спрайте ПРОЗРАЧЕН: фон в этой клетке сохраняется — так человечек не
    // вырезает прямоугольную дыру в декорациях. Части спрайта за краями
    // канвы клипаются.
    void blit(int x, int y, const std::vector<std::string>& spriteRows,
              Tint tint = Tint::Default, bool bold = false);

    // Собрать канву в строки для вывода. color=true — с ANSI-цветом
    // (run-length: SGR эмитится только на смене цвета, в конце строки reset,
    // если строка что-то красила); color=false — чистый текст без ESC.
    std::vector<std::string> toLines(bool color) const;

private:
    struct Cell {
        char32_t glyph = U' ';
        Tint tint = Tint::Default;
        bool bold = false;
    };

    Cell& at(int x, int y) { return cells_[static_cast<std::size_t>(y) * width_ + x]; }
    const Cell& at(int x, int y) const {
        return cells_[static_cast<std::size_t>(y) * width_ + x];
    }

    int width_;
    int height_;
    std::vector<Cell> cells_;  // построчно: y * width_ + x
};

}  // namespace atmsim::scene
