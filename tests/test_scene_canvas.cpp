// ============================================================================
//  test_scene_canvas.cpp — тесты клеточной канвы сцены и белого списка глифов.
//
//  Канва — чистая структура данных, поэтому проверяется без терминала,
//  как и composeLines() (см. test_renderer.cpp). Ключевые контракты:
//  точные размеры, клип по краям, прозрачность спрайтов, run-length SGR,
//  отсутствие ESC при color=false, «1 кодовая точка = 1 колонка».
// ============================================================================
#include <string>
#include <vector>

#include "atmsim/console/scene/GlyphSet.hpp"
#include "atmsim/console/scene/SceneCanvas.hpp"
#include "simple_test.hpp"

using namespace atmsim::scene;

namespace {

// Сколько раз substr входит в строку (для подсчёта SGR-последовательностей).
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// Свежая канва: ровно height строк по ровно width кодовых точек, все — пробелы.
TEST(scene_canvas_blank_has_exact_dimensions) {
    SceneCanvas c(20, 5);
    const std::vector<std::string> lines = c.toLines(false);
    CHECK_EQ(lines.size(), std::size_t{5});
    for (const std::string& l : lines) {
        CHECK_EQ(utf8::decode(l).size(), std::size_t{20});
        CHECK(l.find_first_not_of(' ') == std::string::npos);
    }
}

// put: запись в углы работает, координаты вне канвы молча клипаются.
TEST(scene_canvas_put_writes_and_clips) {
    SceneCanvas c(10, 3);
    c.put(0, 0, U'A');
    c.put(9, 2, U'Z');
    // Всё это должно быть проигнорировано без падений и без порчи соседей:
    c.put(-1, 0, U'X');
    c.put(10, 0, U'X');
    c.put(0, -1, U'X');
    c.put(0, 3, U'X');
    const std::vector<std::string> lines = c.toLines(false);
    CHECK_EQ(lines[0][0], 'A');
    CHECK_EQ(lines[2][9], 'Z');
    std::string all;
    for (const std::string& l : lines) all += l;
    CHECK(all.find('X') == std::string::npos);
    // Инвариант ширины сохранён после клипов.
    for (const std::string& l : lines) CHECK_EQ(utf8::decode(l).size(), std::size_t{10});
}

// text: кириллица пишется по одной клетке на БУКВУ (не на байт), правый край
// клипается, инвариант ширины держится.
TEST(scene_canvas_text_cyrillic_one_cell_per_letter) {
    SceneCanvas c(10, 2);
    c.text(0, 0, "Очередь");   // 7 букв = 7 клеток (14 байт UTF-8)
    c.text(6, 1, "банкомат");  // хвост уйдёт за край и обрежется
    const std::vector<std::string> lines = c.toLines(false);
    CHECK(lines[0].find("Очередь") != std::string::npos);
    CHECK_EQ(utf8::decode(lines[0]).size(), std::size_t{10});
    CHECK_EQ(utf8::decode(lines[1]).size(), std::size_t{10});
    // Влезли ровно 4 буквы «банк» (колонки 6..9).
    CHECK(lines[1].find("банк") != std::string::npos);
    CHECK(lines[1].find("банко") == std::string::npos);
}

// blit: пробелы спрайта прозрачны (фон сохраняется), непробельные глифы
// затирают фон; выход за края клипается.
TEST(scene_canvas_blit_transparency_and_clipping) {
    SceneCanvas c(8, 4);
    // Фон: вся канва заполнена точками.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) c.put(x, y, U'.');
    // Человечек 3x3 с прозрачными углами.
    const std::vector<std::string> sprite = {" o ", "/|\\", "/ \\"};
    c.blit(2, 0, sprite);
    const std::vector<std::string> lines = c.toLines(false);
    CHECK_EQ(lines[0], "...o....");   // углы " o " прозрачны — точки на месте
    CHECK_EQ(lines[1], "../|\\...");
    CHECK_EQ(lines[2], "../.\\...");  // зазор между ногами прозрачен: фон (точка) виден
    CHECK_EQ(lines[3], "........");
    // Блит за краем не падает и не портит размеры.
    c.blit(7, 3, sprite);
    c.blit(-2, -2, sprite);
    for (const std::string& l : c.toLines(false)) {
        CHECK_EQ(utf8::decode(l).size(), std::size_t{8});
    }
}

// Сериализация с цветом: подряд идущие клетки одного цвета образуют ОДИН
// SGR-прогон; в конце окрашенной строки цвет закрыт reset'ом.
TEST(scene_canvas_tolines_color_runs_and_reset) {
    SceneCanvas c(10, 1);
    // 4 зелёные клетки подряд, потом 2 обычные, потом 1 красная жирная.
    for (int x = 0; x < 4; ++x) c.put(x, 0, U'█', Tint::Green);
    c.put(4, 0, U'-');
    c.put(5, 0, U'-');
    c.put(6, 0, U'!', Tint::Red, /*bold=*/true);
    const std::string line = c.toLines(true)[0];
    // Зелёный открыт один раз на весь прогон, а не по разу на клетку.
    CHECK_EQ(countOccurrences(line, "\033[32m"), 1);
    CHECK_EQ(countOccurrences(line, "\033[31m"), 1);
    CHECK_EQ(countOccurrences(line, "\033[1m"), 1);
    // Последний непробельный участок окрашен — строка обязана закрыть цвет.
    CHECK(line.rfind("\033[0m") != std::string::npos);
    CHECK(line.rfind("\033[0m") > line.find("\033[31m"));
    // Ширина считается по кодовым точкам БЕЗ учёта ESC-последовательностей —
    // декодер вернёт больше символов, поэтому сверяем через версию без цвета.
    CHECK_EQ(utf8::decode(c.toLines(false)[0]).size(), std::size_t{10});
}

// При color=false в выводе нет ни единого байта ESC — как у дашборда (§4.8.7).
TEST(scene_canvas_no_ansi_when_color_disabled) {
    SceneCanvas c(6, 2);
    c.put(0, 0, U'█', Tint::Green, true);
    c.text(0, 1, "текст", Tint::Red);
    for (const std::string& l : c.toLines(false)) {
        CHECK(l.find('\033') == std::string::npos);
    }
}

// Белый список: рабочие глифы сцены разрешены, «опасные» (двухколоночные,
// комбинируемые, braille, эмодзи) — запрещены и заменяются на '?'.
TEST(scene_glyphset_whitelist) {
    // Разрешённые: ASCII, кириллица, box-drawing, блоки, стрелки, спецзнаки.
    CHECK(isAllowedGlyph(U'A'));
    CHECK(isAllowedGlyph(U'я'));
    CHECK(isAllowedGlyph(U'Ё'));
    CHECK(isAllowedGlyph(U'─'));
    CHECK(isAllowedGlyph(U'║'));
    CHECK(isAllowedGlyph(U'█'));
    CHECK(isAllowedGlyph(U'▓'));
    CHECK(isAllowedGlyph(U'→'));
    CHECK(isAllowedGlyph(U'✓'));
    CHECK(isAllowedGlyph(U'●'));
    CHECK(isAllowedGlyph(U'…'));
    CHECK(isAllowedGlyph(U'ρ'));
    // Запрещённые: эмодзи (2 колонки), CJK (2 колонки), braille (непредсказуем
    // в conhost), комбинируемый акцент (0 колонок), управляющий ESC.
    CHECK(!isAllowedGlyph(static_cast<char32_t>(0x1F600)));  // 😀
    CHECK(!isAllowedGlyph(static_cast<char32_t>(0x4E00)));   // 一
    CHECK(!isAllowedGlyph(static_cast<char32_t>(0x2800)));   // braille
    CHECK(!isAllowedGlyph(static_cast<char32_t>(0x0301)));   // combining acute
    CHECK(!isAllowedGlyph(static_cast<char32_t>(0x001B)));   // ESC
    // Release-путь: запрещённый глиф превращается в '?', разрешённый — не трогается.
    // char32_t не выводится в поток в C++20 (operator<< удалён), поэтому CHECK.
    CHECK(sanitizeGlyph(static_cast<char32_t>(0x1F600)) == U'?');
    CHECK(sanitizeGlyph(U'ы') == U'ы');
}

// UTF-8 хелперы: decode/append — обратные друг другу, битые байты не роняют.
TEST(scene_utf8_roundtrip_and_garbage) {
    const std::string src = "aЯ─█→…";
    const std::u32string cps = utf8::decode(src);
    CHECK_EQ(cps.size(), std::size_t{6});
    std::string back;
    for (char32_t cp : cps) utf8::append(back, cp);
    CHECK_EQ(back, src);
    // Оборванная многобайтовая последовательность → U+FFFD, не крэш.
    const std::string broken = "a\xD0";  // начало кириллицы без продолжения
    const std::u32string b = utf8::decode(broken);
    CHECK_EQ(b.size(), std::size_t{2});
    CHECK(b[1] == static_cast<char32_t>(0xFFFD));
}
