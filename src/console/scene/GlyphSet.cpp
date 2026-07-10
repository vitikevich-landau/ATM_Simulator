#include "atmsim/console/scene/GlyphSet.hpp"

namespace atmsim::scene {

bool isAllowedGlyph(char32_t cp) {
    // Печатный ASCII (включая пробел). Управляющие (в т.ч. ESC) запрещены:
    // ANSI-последовательности вставляет ТОЛЬКО сериализатор toLines, в клетках
    // канвы им делать нечего.
    if (cp >= 0x20 && cp <= 0x7E) return true;
    // Кириллица (базовый блок: А..я, Ё/ё и украинские/сербские буквы).
    // Все глифы блока узкие во всех целевых терминалах.
    if (cp >= 0x0400 && cp <= 0x04FF) return true;
    // Box-drawing: рамки банкомата, разделители (─│┌┐└┘├┤┬┴┼, двойные ═║ и т.д.).
    if (cp >= 0x2500 && cp <= 0x257F) return true;
    // Блочные элементы: полосы прогресса и заливка (▀▄█░▒▓ и т.д.).
    if (cp >= 0x2580 && cp <= 0x259F) return true;
    // Стрелки направления движения (← ↑ → ↓) — подписи walk-in/walk-out.
    if (cp >= 0x2190 && cp <= 0x2193) return true;
    // Точечный набор узких знаков, уже используемых в проекте или нужных сцене:
    switch (cp) {
        case 0x00AB: case 0x00BB:   // « » — кавычки в подписях
        case 0x00B7:                // · — разделитель-точка
        case 0x00D7:                // × — знак закрытия/отказа
        case 0x2014:                // — (тире)
        case 0x2026:                // … — «ещё N»
        case 0x2713:                // ✓ — успех (уже в подвале дашборда)
        case 0x25AA: case 0x25AE:   // ▪ ▮ — маркеры PIN, слот карты
        case 0x25CB: case 0x25CF:   // ○ ● — точки/маркеры
        case 0x03BB: case 0x03BC: case 0x03C1:  // λ μ ρ — обозначения СМО
            return true;
        default:
            return false;
    }
}

char32_t sanitizeGlyph(char32_t cp) {
    return isAllowedGlyph(cp) ? cp : U'?';
}

namespace utf8 {

std::u32string decode(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;  // заменитель для битых последовательностей
        std::size_t len = 1;
        if ((c & 0x80u) == 0x00u) {            // 0xxxxxxx — ASCII
            cp = c;
        } else if ((c & 0xE0u) == 0xC0u) {     // 110xxxxx 10xxxxxx
            len = 2;
            if (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0u) == 0x80u) {
                cp = (static_cast<char32_t>(c & 0x1Fu) << 6) |
                     (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            }
        } else if ((c & 0xF0u) == 0xE0u) {     // 1110xxxx 10xxxxxx 10xxxxxx
            len = 3;
            if (i + 2 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0u) == 0x80u &&
                (static_cast<unsigned char>(s[i + 2]) & 0xC0u) == 0x80u) {
                cp = (static_cast<char32_t>(c & 0x0Fu) << 12) |
                     ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            }
        } else if ((c & 0xF8u) == 0xF0u) {     // 11110xxx ... (4 байта)
            len = 4;
            if (i + 3 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0u) == 0x80u &&
                (static_cast<unsigned char>(s[i + 2]) & 0xC0u) == 0x80u &&
                (static_cast<unsigned char>(s[i + 3]) & 0xC0u) == 0x80u) {
                cp = (static_cast<char32_t>(c & 0x07u) << 18) |
                     ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                     ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            }
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

void append(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

}  // namespace utf8
}  // namespace atmsim::scene
