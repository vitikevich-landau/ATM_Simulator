#pragma once
// ============================================================================
//  Terminal.hpp — тонкий кроссплатформенный слой над терминалом (§4.8.7).
//
//  Здесь и ТОЛЬКО здесь живёт платформенно-зависимый код: включение VT-режима
//  на Windows, проверка «stdout — это интерактивный терминал?», размеры окна.
//  Остальной код рисует дашборд обычными ANSI-последовательностями, не зная,
//  на какой ОС он работает.
// ============================================================================
#include <cstddef>
#include <string>

namespace atmsim {

class Terminal {
public:
    // Является ли stdout интерактивным терминалом (а не файлом/пайпом)?
    // Если нет — live-режим отключается, чтобы не сорить ANSI-кодами (§4.8.7).
    static bool isStdoutTty();

    // Включает поддержку ANSI-последовательностей. На Windows это перевод
    // консоли в VT-режим + UTF-8 вывод; на Linux/mac ANSI работает нативно.
    static bool enableAnsi();

    static int width();   // ширина терминала в столбцах (fallback 80)
    static int height();  // высота терминала в строках (fallback 24)
};

// RAII-повышение разрешения системного таймера Windows до 1 мс на время жизни
// объекта (timeBeginPeriod/timeEndPeriod из winmm). Зачем: штатная
// гранулярность ~15.6 мс заставляет wait_until кадрового цикла промахиваться
// мимо дедлайнов на высоких fps (см. docs/scene_frame_budget.md, режим hires).
// Заводится ТОЛЬКО в render-потоке на время live-режима — глобально настройку
// системы не трогаем. На POSIX — no-op (там таймеры и так точные).
class TimerResolutionGuard {
public:
    TimerResolutionGuard();
    ~TimerResolutionGuard();
    TimerResolutionGuard(const TimerResolutionGuard&) = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;

private:
    bool active_ = false;
};

// --- Интерактивный ввод (raw-режим) — §4.8 v2: горячие клавиши / прокрутка ------
// Распознаваемые клавиши. Char — обычный печатный символ (возвращается отдельно),
// None — нераспознанная управляющая, Eof — ввод закрыт.
enum class Key {
    None, Char, Enter, Escape, Backspace, Delete,
    Up, Down, Left, Right, PageUp, PageDown, Home, End, Eof
};

// RAII-перевод stdin в raw-режим (без построчной буферизации и эха) на время жизни
// объекта; деструктор восстанавливает исходный режим. На Windows ввод читается
// через _getch (raw по своей природе), поэтому там конструктор — почти no-op.
// Если stdin не терминал (пайп/файл), active() == false — интерактив недоступен.
class RawInputMode {
public:
    RawInputMode();
    ~RawInputMode();
    RawInputMode(const RawInputMode&) = delete;
    RawInputMode& operator=(const RawInputMode&) = delete;
    bool active() const { return active_; }

private:
    bool active_ = false;
    void* saved_ = nullptr;  // исходные termios (POSIX); на Windows не используется
};

// Блокирующе читает одну клавишу. Для Key::Char символ возвращается в ch.
Key readKey(char& ch);

// Как readKey, но с ТАЙМ-АУТОМ: ждёт клавишу не дольше timeoutMs миллисекунд и,
// если за это время её не нажали, возвращает Key::None (не блокируясь дальше).
// Нужен «живому» просмотрщику (queue): он обязан сам перечитывать данные и
// перерисовываться на частоте дашборда, а не зависать в ожидании нажатия. На
// Windows опрашивает _kbhit каждые ~10 мс (WaitForSingleObject на хендле ввода
// не годится: он сигналит и на события мыши/фокуса/ресайза, которых _getch не
// читает); на POSIX ждёт готовности stdin через select. При timeoutMs <= 0 —
// неблокирующий опрос «есть ли клавиша прямо сейчас».
Key readKeyTimeout(char& ch, int timeoutMs);

// Ограничивает смещение прокрутки списка диапазоном [0, max(0, total - viewRows)].
// Вынесено отдельной чистой функцией ради юнит-тестов пагинации.
int clampScrollOffset(int offset, int total, int viewRows);

// Результат применения клавиши к редактируемой строке ввода.
enum class LineEdit { Continue, Submit, Cancel };

// Применяет одну клавишу к строке buf и позиции курсора cur (в байтах): печатный
// символ вставляется по позиции курсора, Backspace/Delete удаляют, стрелки ←/→ и
// Home/End двигают курсор, Esc очищает строку. Навигация и удаление — ПО КОДОВЫМ
// ТОЧКАМ UTF-8 (кириллица = 2 байта: стрелка/Backspace работают с целым символом,
// а не с полбайта). Возвращает Submit при Enter, Cancel при Eof, иначе Continue.
// Чистая функция — покрыта юнит-тестами.
LineEdit editLine(std::string& buf, std::size_t& cur, Key key, char ch);

// Сколько «печатных» колонок (кодовых точек UTF-8) в первых nbytes байтах s — то
// есть в какой колонке терминала стоял бы курсор после этих байтов. Байты-
// продолжения UTF-8 (0x80..0xBF) не считаются. Нужно для позиции курсора при
// наборе кириллицы: иначе курсор уезжал бы вправо на лишний байт на символ.
std::size_t displayColumns(const std::string& s, std::size_t nbytes);

// --- ANSI-последовательности управления терминалом --------------------------
// Escape-код 0x1B записываем восьмеричным "\033". Функции возвращают короткие
// строки, которые печатаются в stdout.
namespace ansi {

inline const char* clearScreen()   { return "\033[2J"; }
inline const char* home()          { return "\033[H"; }
inline const char* clearToLineEnd() { return "\033[K"; }
inline const char* saveCursor()    { return "\0337"; }  // ESC 7 — сохранить позицию
inline const char* restoreCursor() { return "\0338"; }  // ESC 8 — восстановить
inline const char* hideCursor()    { return "\033[?25l"; }
inline const char* showCursor()    { return "\033[?25h"; }
inline const char* altScreenOn()   { return "\033[?1049h"; }  // альтернативный буфер
inline const char* altScreenOff()  { return "\033[?1049l"; }
// Synchronized output (DEC 2026): терминал показывает кадр целиком, без
// «полусобранных» промежуточных состояний. Windows Terminal поддерживает,
// legacy conhost молча игнорирует — слать можно безусловно.
inline const char* syncBegin()     { return "\033[?2026h"; }
inline const char* syncEnd()       { return "\033[?2026l"; }

inline std::string moveTo(int row, int col) {
    return "\033[" + std::to_string(row) + ';' + std::to_string(col) + 'H';
}

// Цвета (SGR). reset() обязательно закрывает окрашенный участок.
inline const char* reset()  { return "\033[0m"; }
inline const char* bold()   { return "\033[1m"; }
inline const char* red()    { return "\033[31m"; }
inline const char* green()  { return "\033[32m"; }
inline const char* yellow() { return "\033[33m"; }
inline const char* blue()   { return "\033[34m"; }
inline const char* cyan()   { return "\033[36m"; }
inline const char* grey()   { return "\033[90m"; }

}  // namespace ansi
}  // namespace atmsim
