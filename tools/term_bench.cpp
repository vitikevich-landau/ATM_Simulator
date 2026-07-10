// ============================================================================
//  term_bench.cpp — измеритель «бюджета кадра» терминала (этап 0 фичи «сцена»).
//
//  Зачем: прежде чем строить анимированную сцену (feature/scene), нужно знать,
//  сколько РЕАЛЬНО стоит вывод кадра в конкретном терминале (Windows Terminal /
//  legacy conhost / Linux) и укладываемся ли мы в целевые 10/15/30 fps. Этот
//  инструмент — отдельный бинарь, прод-код не трогает вообще.
//
//  Что меряем (каждая комбинация «сценарий × fps × режим таймера» ~4 секунды):
//    * storm  — худший случай: каждый кадр перерисовываются ВСЕ клетки экрана,
//               у каждой клетки своя SGR-смена цвета (эмуляция полного repaint
//               цветной сцены без diff-рендера);
//    * sparse — целевой случай diff-рендера: один полный кадр, затем каждый
//               кадр обновляются только ~40 клеток (moveTo + SGR + глиф).
//  Для каждой комбинации считаем: среднее/максимальное время записи кадра,
//  фактический fps, число просроченных дедлайнов (кадр не успел к сроку).
//
//  Режимы таймера (только Windows, на POSIX второй прогон пропускается):
//    * default — штатная гранулярность таймера (~15.6 мс);
//    * hires   — timeBeginPeriod(1): проверяем, лечит ли он дрожание дедлайнов
//                (это прямой вход для решения по этапу 5: winmm vs waitable).
//
//  Запуск:  term_bench [файл_отчёта]
//    Отчёт печатается в stderr в конце прогона и, если задан аргумент,
//    дублируется в файл (нужно при запуске в отдельном окне терминала,
//    которое закроется по завершении).
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")  // timeBeginPeriod/timeEndPeriod (MSVC)
#include <timeapi.h>
#endif

namespace {

// Размер тестового кадра. Фиксированный (не от реального окна), чтобы цифры
// разных терминалов/машин были сравнимы между собой. 100x30 — близко к
// реальной сцене (полоса сцены + таблица) в окне 120x40.
constexpr int kCols = 100;
constexpr int kRows = 30;

// Безопасные одноколоночные глифы (см. будущий GlyphSet): ASCII + блоки.
const char* kGlyphs[] = {"#", "o", "/", "\\", "|", "=", "-", "░", "▒", "▓", "█"};
constexpr int kGlyphCount = static_cast<int>(sizeof(kGlyphs) / sizeof(kGlyphs[0]));

std::string moveTo(int row, int col) {
    return "\033[" + std::to_string(row) + ';' + std::to_string(col) + 'H';
}

// Детерминированный «шум» вместо RNG: хэш от координат и номера кадра.
// splitmix64 — та же функция, что пойдёт в idle-анимации сцены.
unsigned long long splitmix64(unsigned long long x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Кадр «шторма»: все клетки экрана, каждая со своей сменой цвета SGR.
// Возвращает готовую ANSI-строку кадра (собирается заранее, вне замера —
// мы меряем именно ВЫВОД, а не композицию).
std::string buildStormFrame(int frameIdx) {
    std::string out;
    out.reserve(static_cast<std::size_t>(kCols) * kRows * 16);
    for (int r = 0; r < kRows; ++r) {
        out += moveTo(r + 1, 1);
        for (int c = 0; c < kCols; ++c) {
            const unsigned long long h =
                splitmix64(static_cast<unsigned long long>(frameIdx) * 1000003ull +
                           static_cast<unsigned long long>(r) * kCols + static_cast<unsigned long long>(c));
            // Смена цвета на КАЖДОЙ клетке — максимальная нагрузка на VT-парсер.
            out += "\033[3";
            out += static_cast<char>('1' + static_cast<int>(h % 7));  // 31..37
            out += 'm';
            out += kGlyphs[h % kGlyphCount];
        }
    }
    out += "\033[0m";
    return out;
}

// Кадр «разрежённого» обновления: ~40 точечных правок (позиция+цвет+глиф) —
// так будет выглядеть типичный кадр FrameDiffer в устоявшейся сцене.
std::string buildSparseFrame(int frameIdx) {
    std::string out;
    out.reserve(40 * 24);
    for (int i = 0; i < 40; ++i) {
        const unsigned long long h =
            splitmix64(static_cast<unsigned long long>(frameIdx) * 40u + static_cast<unsigned long long>(i));
        const int r = static_cast<int>(h % kRows) + 1;
        const int c = static_cast<int>((h >> 8) % kCols) + 1;
        out += moveTo(r, c);
        out += "\033[3";
        out += static_cast<char>('1' + static_cast<int>(h % 7));
        out += 'm';
        out += kGlyphs[h % kGlyphCount];
    }
    out += "\033[0m";
    return out;
}

struct RunResult {
    const char* scenario;
    int targetFps;
    const char* timerMode;
    double avgWriteMs;
    double maxWriteMs;
    double effectiveFps;
    int missedDeadlines;
    std::size_t bytesPerFrame;
};

// RAII-повышение разрешения системного таймера Windows до 1 мс.
// Ровно тот механизм, что предложен для этапа 5 (TimerResolutionGuard).
class HiResTimer {
public:
    explicit HiResTimer(bool enable) : enabled_(enable) {
#ifdef _WIN32
        if (enabled_) timeBeginPeriod(1);
#endif
    }
    ~HiResTimer() {
#ifdef _WIN32
        if (enabled_) timeEndPeriod(1);
#endif
    }
    HiResTimer(const HiResTimer&) = delete;
    HiResTimer& operator=(const HiResTimer&) = delete;

private:
    bool enabled_;
};

RunResult runScenario(const char* scenario, int targetFps, bool hires) {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::microseconds(1000000 / targetFps);
    const int frames = targetFps * 4;  // ~4 секунды на комбинацию

    // Кадры собираем ЗАРАНЕЕ: меряем стоимость вывода, а не генерации.
    std::vector<std::string> prebuilt;
    prebuilt.reserve(static_cast<std::size_t>(frames));
    std::size_t totalBytes = 0;
    const bool storm = (std::strcmp(scenario, "storm") == 0);
    for (int i = 0; i < frames; ++i) {
        prebuilt.push_back(storm ? buildStormFrame(i) : buildSparseFrame(i));
        totalBytes += prebuilt.back().size();
    }

    HiResTimer guard(hires);

    // Полная очистка перед сценарием (для sparse — базовый кадр).
    std::fputs("\033[2J\033[H", stdout);
    if (!storm) {
        const std::string base = buildStormFrame(0);
        std::fwrite(base.data(), 1, base.size(), stdout);
    }
    std::fflush(stdout);

    double sumWriteMs = 0.0, maxWriteMs = 0.0;
    int missed = 0;
    const auto start = clock::now();
    for (int i = 0; i < frames; ++i) {
        // Дедлайновая пейсинг-схема (как в будущем renderLoop): следующий кадр
        // строго в start + (i+1)*period, а не «сон на номинал после работы».
        const auto w0 = clock::now();
        std::fwrite(prebuilt[static_cast<std::size_t>(i)].data(), 1,
                    prebuilt[static_cast<std::size_t>(i)].size(), stdout);
        std::fflush(stdout);
        const auto w1 = clock::now();
        const double writeMs = std::chrono::duration<double, std::milli>(w1 - w0).count();
        sumWriteMs += writeMs;
        if (writeMs > maxWriteMs) maxWriteMs = writeMs;

        const auto deadline = start + (i + 1) * period;
        if (clock::now() > deadline) ++missed;
        else std::this_thread::sleep_until(deadline);
    }
    const auto end = clock::now();
    const double totalSec = std::chrono::duration<double>(end - start).count();

    RunResult res;
    res.scenario = scenario;
    res.targetFps = targetFps;
    res.timerMode = hires ? "hires" : "default";
    res.avgWriteMs = sumWriteMs / frames;
    res.maxWriteMs = maxWriteMs;
    res.effectiveFps = frames / totalSec;
    res.missedDeadlines = missed;
    res.bytesPerFrame = totalBytes / static_cast<std::size_t>(frames);
    return res;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Те же приготовления, что делает Terminal::enableAnsi() в проде:
    // VT-режим для ANSI-последовательностей + UTF-8 кодовая страница вывода.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::vector<RunResult> results;
    const int fpsList[] = {10, 15, 30};
    const char* scenarios[] = {"storm", "sparse"};
#ifdef _WIN32
    const bool timerModes[] = {false, true};
#else
    const bool timerModes[] = {false};  // на POSIX hires не нужен
#endif
    for (const char* sc : scenarios) {
        for (int fps : fpsList) {
            for (bool hires : timerModes) {
                results.push_back(runScenario(sc, fps, hires));
            }
        }
    }

    // Отчёт — в stderr (не затирается сценой) и, по желанию, в файл.
    std::string report = "scenario fps timer avg_write_ms max_write_ms effective_fps missed bytes/frame\n";
    for (const RunResult& r : results) {
        char line[160];
        std::snprintf(line, sizeof(line), "%-6s %3d %-7s %8.3f %8.3f %8.2f %4d %8zu\n",
                      r.scenario, r.targetFps, r.timerMode, r.avgWriteMs, r.maxWriteMs,
                      r.effectiveFps, r.missedDeadlines, r.bytesPerFrame);
        report += line;
    }
    std::fputs("\033[2J\033[H\033[0m", stdout);
    std::fflush(stdout);
    std::fputs(report.c_str(), stderr);
    if (argc > 1) {
        if (std::FILE* f = std::fopen(argv[1], "w")) {
            std::fputs(report.c_str(), f);
            std::fclose(f);
        }
    }
    return 0;
}
