#pragma once
// ============================================================================
//  LiveRenderer.hpp — живой дашборд в консоли (§4.8).
//
//  Это ТРЕТИЙ поток-читатель поверх модели «один писатель, много читателей»
//  (§6.1). Он с частотой refresh_hz опрашивает snapshot-методы AtmEngine и
//  перерисовывает экран. ЖЁСТКОЕ ПРАВИЛО: рендерер НИКОГДА не мутирует состояние
//  — только const-снимки. Поэтому он не может ни сломать ядро, ни создать гонку
//  записи (проверяется под ThreadSanitizer).
//
//  Дашборд занимает верхние height() строк экрана; строку ввода команд рисует
//  консоль НИЖЕ дашборда. В конце каждого кадра рендерер прячет курсор на время
//  отрисовки и ЯВНО ставит его в позицию ввода (её сообщает консоль через
//  setCursorTarget). Так курсор не «прыгает» в таблицу на переходах (старт, после
//  отчёта/очереди), когда экран очищен и позиция курсора иначе оказалась бы (1,1).
// ============================================================================
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atmsim/config/Config.hpp"
#include "atmsim/console/FrameDiffer.hpp"
// Полный тип нужен здесь (а не forward-декларация), потому что деструктор
// LiveRenderer определён inline в этом заголовке (обход LNK2005 на MSVC), а
// inline-деструктор разрушает unique_ptr<ScenePresenter> — тип обязан быть
// полным в точке включения.
#include "atmsim/console/scene/ScenePresenter.hpp"
#include "atmsim/engine/AtmEngine.hpp"

namespace atmsim {

/// \brief Живой дашборд: отдельный render-поток, который с частотой refresh_hz
///        читает снимки движка и перерисовывает экран. Только чтение — состояние
///        ядра не мутирует (§4.8).
class LiveRenderer {
public:
    // forcedWidth/forcedHeight > 0 подменяют реальные размеры терминала.
    // Нужны ТОЛЬКО тестам: вне TTY (Docker, CI) Terminal::width()/height()
    // возвращают запасные 80x24, и сцена (требующая ~84x30) была бы
    // непроверяемой. Обычный код передаёт нули (= автоопределение).
    LiveRenderer(AtmEngine& engine, const Config& cfg, int forcedWidth = 0,
                 int forcedHeight = 0);
    // Деструктор определён inline (в заголовке) намеренно: так он получает
    // COMDAT-компоновку и линкер сам сливает копии из разных единиц трансляции.
    // Внешнее (out-of-line) определение спец-члена в статической библиотеке на
    // MSVC приводило к LNK2005 «уже определён». Тело — просто гарантированная
    // остановка потока (RAII).
    ~LiveRenderer() { stop(); }

    LiveRenderer(const LiveRenderer&) = delete;             // не копируется/не перемещается —
    LiveRenderer& operator=(const LiveRenderer&) = delete;  // владеет потоком и мьютексами

    void start();   // запустить render-поток
    void stop();    // остановить и присоединить (идемпотентно)
    void pause();   // приостановить перерисовку (для полноэкранных ответов)
    // Возобновить перерисовку. Рендер сам (в своём потоке) сделает полный
    // repaint (overlay затёр экран) и расставит актёров сцены по текущему
    // снимку мгновенно — «догоняющие» анимации за время паузы не проигрываются.
    void resume();

    // Куда рендер-поток ставит видимый курсор в конце каждого кадра — текущая
    // позиция ввода команды (row/col, 1-based). Сообщает консоль при каждой
    // перерисовке строки ввода и при входе в live-режим. Потокобезопасно (atomic).
    void setCursorTarget(int row, int col) { cursorRow_.store(row); cursorCol_.store(col); }

    // Число строк, которые занимает дашборд (строку ввода консоль рисует ниже).
    int height() const { return height_; }

    // Поместится ли сцена в терминал (та же формула, что в конструкторе).
    // Статический — чтобы консоль могла проверить ДО перезапуска live-сессии
    // и внятно отказать на маленьком окне, а не молча показать таблицу.
    // termWidth/termHeight > 0 подменяют реальные размеры (тесты).
    static bool sceneFits(const Config& cfg, int termWidth = 0, int termHeight = 0);

    // Общий мьютекс вывода в терминал: и рендерер, и консоль пишут через него,
    // чтобы кадр и эхо/ответы не перемешивались.
    std::mutex& outputMutex() { return outMutex_; }

    // Собирает кадр как список строк (с ANSI-цветом, если включён), снимая
    // СВЕЖИЕ снимки движка. Публичный, чтобы можно было проверить содержимое
    // кадра юнит-тестом без терминала.
    std::vector<std::string> composeLines() const;

private:
    void renderLoop();
    // Фактическая частота КАДРОВ: со сценой — не ниже scene_fps и не ниже
    // refresh_hz (дифф-рендер делает «лишние» кадры почти бесплатными, зато
    // снимки честно опрашиваются на refresh_hz); без сцены — refresh_hz.
    // Одна формула для цикла кадров и подписи в шапке.
    int frameRate() const;
    // Сборка кадра из ГОТОВЫХ снимков — рабочая лошадка composeLines() и
    // render-цикла (тот держит снимки в кэше и опрашивает движок на
    // refresh_hz, а кадры рисует на scene_fps — частоты разделены).
    std::vector<std::string> composeLinesFrom(const AtmSnapshot& s, const StatsSnapshot& st,
                                              const std::vector<ClientSnapshot>& q,
                                              const std::vector<OperationRecord>& ops) const;
    void paintFrame(const std::vector<std::string>& lines);

    AtmEngine& engine_;
    Config cfg_;

    // Простая и портируемая модель потока: обычный std::thread + флаг running_ +
    // обычная condition_variable для прерываемого сна. Намеренно БЕЗ jthread/
    // stop_token/condition_variable_any — чтобы не зависеть от их поведения на
    // конкретной платформе (у рендерера это лишнее; ядро §6.2 живёт отдельно).
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    // Позиция, куда рендер-поток возвращает видимый курсор после отрисовки кадра
    // (строка ввода). По умолчанию (1,1); консоль задаёт реальную через
    // setCursorTarget. Атомарные — пишет консольный поток, читает render-поток.
    std::atomic<int> cursorRow_{1};
    std::atomic<int> cursorCol_{1};

    std::mutex outMutex_;              // сериализация вывода в stdout
    std::mutex sleepMutex_;            // для прерываемого сна render-цикла
    std::condition_variable sleepCv_;

    // Размеры терминала читаем один раз при создании (обработку ресайза на лету
    // оставляем на v2). По ним подстраиваем ширину колонок и число строк очереди.
    int width_{80};
    int termHeight_{24};
    int height_{0};                    // высота дашборда (число строк)

    // --- Анимированная сцена (feature/scene) ---------------------------------
    // Решение «влезает ли сцена» принимается ОДИН раз в конструкторе (как и
    // размеры): при ui.scene=true, но маленьком терминале sceneActive_ остаётся
    // false и дашборд молча работает в табличном режиме — высота кадра в
    // пределах live-сессии постоянна в любом случае (§4.8.5).
    bool sceneActive_{false};
    int sceneRows_{0};                 // высота сценической полосы (строки)

    // Презентационный слой актёров (этап 3). Существует только при активной
    // сцене. ПРИНАДЛЕЖИТ render-потоку: tick() зовётся из renderLoop() перед
    // отрисовкой кадра; извне его дергают только тесты (без запущенного
    // потока). Движок он не трогает — работает с копиями-снимками.
    std::unique_ptr<scene::ScenePresenter> presenter_;

    // --- Этап 5: дифф-рендер и кэш снимков ------------------------------------
    // Всё ниже ПРИНАДЛЕЖИТ render-потоку (как presenter_): консоль общается с
    // ним только атомарным флагом teleportOnResume_.
    FrameDiffer differ_;               // перерисовываем только изменившиеся строки
    AtmSnapshot cachedSnap_;           // снимки опрашиваются на refresh_hz...
    StatsSnapshot cachedStats_;        // ...а кадры рисуются на scene_fps из кэша
    std::vector<ClientSnapshot> cachedQueue_;
    std::vector<OperationRecord> cachedOps_;

    // resume() (консольный поток) просит render-поток: полный repaint (overlay
    // затёр экран) + мгновенная расстановка актёров. Атомарный флаг вместо
    // прямых вызовов differ_/presenter_ — они не потокобезопасны.
    std::atomic<bool> teleportOnResume_{false};
};

}  // namespace atmsim
