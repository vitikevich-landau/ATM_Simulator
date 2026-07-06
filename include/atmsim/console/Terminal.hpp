#pragma once
// ============================================================================
//  Terminal.hpp — тонкий кроссплатформенный слой над терминалом (§4.8.7).
//
//  Здесь и ТОЛЬКО здесь живёт платформенно-зависимый код: включение VT-режима
//  на Windows, проверка «stdout — это интерактивный терминал?», размеры окна.
//  Остальной код рисует дашборд обычными ANSI-последовательностями, не зная,
//  на какой ОС он работает.
// ============================================================================
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
