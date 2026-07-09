// ============================================================================
//  test_frame_differ.cpp — тесты построчного диффа кадров (этап 5).
//
//  Чисто строковая логика: ни терминала, ни движка. Контракт: после
//  invalidate() дифф эквивалентен полной перерисовке; без изменений — пусто;
//  меняется строка — переписывается ровно она.
// ============================================================================
#include <string>
#include <vector>

#include "atmsim/console/FrameDiffer.hpp"
#include "atmsim/console/Terminal.hpp"
#include "simple_test.hpp"

using namespace atmsim;

namespace {

// Полная перерисовка кадра — эталон для контрактного теста (тот же формат
// «moveTo + строка + clearToLineEnd», что исторически писал paintFrame).
std::string fullPaint(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += ansi::moveTo(static_cast<int>(i) + 1, 1);
        out += lines[i];
        out += ansi::clearToLineEnd();
    }
    return out;
}

}  // namespace

// Первый кадр (и кадр после invalidate) — полная перерисовка, побайтно
// эквивалентная историческому полному repaint.
TEST(differ_first_and_invalidated_frames_are_full) {
    FrameDiffer d;
    const std::vector<std::string> frame = {"строка А", "строка Б", "строка В"};
    CHECK_EQ(d.diff(frame), fullPaint(frame));

    d.diff(frame);  // прогрев
    d.invalidate();
    CHECK_EQ(d.diff(frame), fullPaint(frame));
}

// Кадр не изменился — дифф пуст (терминал можно не трогать вовсе).
TEST(differ_identical_frame_yields_empty_diff) {
    FrameDiffer d;
    const std::vector<std::string> frame = {"один", "два"};
    d.diff(frame);
    CHECK_EQ(d.diff(frame), std::string{});
}

// Изменилась одна строка — переписывается ровно она (одна moveTo).
TEST(differ_rewrites_only_changed_lines) {
    FrameDiffer d;
    std::vector<std::string> frame = {"шапка", "очередь: 3", "подвал"};
    d.diff(frame);

    frame[1] = "очередь: 4";
    const std::string out = d.diff(frame);
    CHECK_EQ(out, ansi::moveTo(2, 1) + "очередь: 4" + ansi::clearToLineEnd());

    // И после точечной перерисовки состояние согласовано: повтор пуст.
    CHECK_EQ(d.diff(frame), std::string{});
}

// Кадр стал короче (при §4.8.5 не случается, но контракт не должен на это
// полагаться): лишние строки прошлого кадра стираются.
TEST(differ_clears_tail_when_frame_shrinks) {
    FrameDiffer d;
    d.diff({"a", "b", "c"});
    const std::string out = d.diff({"a", "b"});
    CHECK(out.find(ansi::moveTo(3, 1)) != std::string::npos);
    CHECK(out.find(ansi::clearToLineEnd()) != std::string::npos);
}

// TimerResolutionGuard: конструирование/разрушение безопасно на всех
// платформах (на POSIX — no-op; на Windows парный timeBeginPeriod/EndPeriod).
TEST(timer_resolution_guard_smoke) {
    {
        TimerResolutionGuard g1;
        TimerResolutionGuard g2;  // вложенные тоже допустимы (счётчик у ОС)
    }
    CHECK(true);
}
