#include "atmsim/console/Terminal.hpp"

// Платформенно-зависимая часть. Windows-ветка компилируется только на Windows;
// на Linux (где идёт сборка/проверка) работает POSIX-ветка.
#include <cstdio>   // EOF (Windows _getch)
#include <cstdlib>  // getenv / atoi — резервный источник размеров терминала

#ifdef _WIN32
#include <conio.h>   // _getch — чтение клавиши без эха/Enter
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>  // select — «есть ли ещё байты» при разборе ESC-последовательностей
#include <termios.h>     // raw-режим stdin (ICANON/ECHO off)
#include <unistd.h>
#endif

namespace atmsim {
namespace {
// Резерв: если размер не удалось узнать у системы, пробуем переменные окружения.
int envSize(const char* name) {
    if (const char* v = std::getenv(name)) {
        const int n = std::atoi(v);
        if (n > 0) return n;
    }
    return 0;
}
}  // namespace

bool Terminal::isStdoutTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

bool Terminal::enableAnsi() {
#ifdef _WIN32
    // Переводим вывод консоли в UTF-8 (у нас русские подписи и символы рамок/полос)
    // и включаем обработку VT-последовательностей (ANSI), доступную с Windows 10.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;  // на Linux/mac терминал понимает ANSI без дополнительных действий
#endif
}

int Terminal::width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    if (const int e = envSize("COLUMNS")) return e;
    return 80;
#endif
}

int Terminal::height() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    return 24;
#else
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    if (const int e = envSize("LINES")) return e;
    return 24;
#endif
}

// --- Интерактивный ввод (raw-режим) ----------------------------------------
RawInputMode::RawInputMode() {
#ifdef _WIN32
    // _getch читает клавишу напрямую, без эха и без Enter, независимо от режима
    // консоли — отдельно переключать режим не нужно.
    active_ = true;
#else
    if (!::isatty(STDIN_FILENO)) return;  // не терминал (пайп/файл) — интерактив невозможен
    termios* orig = new termios;
    if (tcgetattr(STDIN_FILENO, orig) != 0) { delete orig; return; }
    termios raw = *orig;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));  // без построчной буферизации и эха
    raw.c_cc[VMIN] = 1;    // read блокируется до первого байта
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { delete orig; return; }
    saved_ = orig;
    active_ = true;
#endif
}

RawInputMode::~RawInputMode() {
#ifndef _WIN32
    if (saved_) {
        tcsetattr(STDIN_FILENO, TCSANOW, static_cast<termios*>(saved_));
        delete static_cast<termios*>(saved_);
    }
#endif
}

#ifndef _WIN32
namespace {
// Готов ли stdin отдать байт в ближайшие ms миллисекунд. Нужно, чтобы отличить
// одиночный ESC (выход) от начала CSI-последовательности стрелки (ESC [ A …).
bool stdinPending(int ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}
}  // namespace
#endif

Key readKey(char& ch) {
    ch = 0;
#ifdef _WIN32
    const int c = _getch();
    if (c == 0 || c == 0xE0) {  // расширенная клавиша: следом идёт скан-код
        switch (_getch()) {
            case 72: return Key::Up;
            case 80: return Key::Down;
            case 75: return Key::Left;
            case 77: return Key::Right;
            case 73: return Key::PageUp;
            case 81: return Key::PageDown;
            case 71: return Key::Home;
            case 79: return Key::End;
            case 83: return Key::Delete;
            default: return Key::None;
        }
    }
    if (c == EOF) return Key::Eof;
    if (c == '\r' || c == '\n') return Key::Enter;
    if (c == 8) return Key::Backspace;  // Backspace на Windows
    if (c == 27) return Key::Escape;
    ch = static_cast<char>(c);
    return Key::Char;
#else
    char c = 0;
    if (::read(STDIN_FILENO, &c, 1) <= 0) return Key::Eof;
    if (c == '\r' || c == '\n') return Key::Enter;
    if (c == 0x7F || c == 0x08) return Key::Backspace;  // DEL/BS от клавиши Backspace
    if (c == 0x1B) {  // ESC: либо выход, либо начало CSI-последовательности стрелки
        if (!stdinPending(20)) return Key::Escape;  // одиночный ESC
        char b1 = 0;
        if (::read(STDIN_FILENO, &b1, 1) <= 0) return Key::Escape;
        if (b1 != '[' && b1 != 'O') return Key::None;
        char b2 = 0;
        if (::read(STDIN_FILENO, &b2, 1) <= 0) return Key::Escape;
        switch (b2) {
            case 'A': return Key::Up;
            case 'B': return Key::Down;
            case 'C': return Key::Right;
            case 'D': return Key::Left;
            case 'H': return Key::Home;
            case 'F': return Key::End;
            case '3': { char t; if (stdinPending(20)) (void)::read(STDIN_FILENO, &t, 1); return Key::Delete; }    // ESC[3~
            case '5': { char t; if (stdinPending(20)) (void)::read(STDIN_FILENO, &t, 1); return Key::PageUp; }    // ESC[5~
            case '6': { char t; if (stdinPending(20)) (void)::read(STDIN_FILENO, &t, 1); return Key::PageDown; }  // ESC[6~
            default:  return Key::None;
        }
    }
    ch = c;
    return Key::Char;
#endif
}

int clampScrollOffset(int offset, int total, int viewRows) {
    const int maxOff = (total > viewRows) ? total - viewRows : 0;
    if (offset < 0) return 0;
    if (offset > maxOff) return maxOff;
    return offset;
}

namespace {
// --- UTF-8: навигация по кодовым точкам в редактируемой строке ---------------
inline bool isUtf8Cont(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;  // байт-продолжение 10xxxxxx
}
// Начало предыдущей кодовой точки перед байтовой позицией pos (для ← и Backspace).
std::size_t prevCodePoint(const std::string& s, std::size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && isUtf8Cont(s[pos])) --pos;
    return pos;
}
// Начало следующей кодовой точки после байтовой позиции pos (для → и Delete).
std::size_t nextCodePoint(const std::string& s, std::size_t pos) {
    const std::size_t n = s.size();
    if (pos >= n) return n;
    ++pos;
    while (pos < n && isUtf8Cont(s[pos])) ++pos;
    return pos;
}
}  // namespace

std::size_t displayColumns(const std::string& s, std::size_t nbytes) {
    if (nbytes > s.size()) nbytes = s.size();
    std::size_t cols = 0;
    for (std::size_t i = 0; i < nbytes; ++i) {
        if (!isUtf8Cont(s[i])) ++cols;  // считаем только начала символов
    }
    return cols;
}

LineEdit editLine(std::string& buf, std::size_t& cur, Key key, char ch) {
    if (cur > buf.size()) cur = buf.size();  // страховка от рассинхрона
    switch (key) {
        case Key::Enter:  return LineEdit::Submit;
        case Key::Eof:    return LineEdit::Cancel;
        case Key::Char:
            // Печатный байт вставляем по позиции курсора. Многобайтный символ
            // (кириллица) приходит несколькими Char-событиями — байты складываются
            // подряд, cur остаётся на границе символа после последнего из них.
            buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cur), ch);
            ++cur;
            break;
        case Key::Backspace:
            if (cur > 0) {
                const std::size_t start = prevCodePoint(buf, cur);  // удаляем ВЕСЬ символ
                buf.erase(start, cur - start);
                cur = start;
            }
            break;
        case Key::Delete:
            if (cur < buf.size()) {
                const std::size_t end = nextCodePoint(buf, cur);
                buf.erase(cur, end - cur);
            }
            break;
        case Key::Left:  cur = prevCodePoint(buf, cur); break;  // на символ, не на байт
        case Key::Right: cur = nextCodePoint(buf, cur); break;
        case Key::Home:  cur = 0; break;
        case Key::End:   cur = buf.size(); break;
        case Key::Escape: buf.clear(); cur = 0; break;  // очистить строку
        default: break;  // None, PageUp/Down, ↑/↓ — в строке ввода игнорируем
    }
    return LineEdit::Continue;
}

}  // namespace atmsim
