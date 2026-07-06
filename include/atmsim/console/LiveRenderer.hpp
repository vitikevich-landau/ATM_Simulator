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
//  консоль НИЖЕ дашборда. Рендерер сохраняет/восстанавливает позицию курсора
//  (ESC 7 / ESC 8) вокруг перерисовки и не трогает строку ввода — поэтому
//  набираемый текст не затирается (портируемая модель ввода без raw-режима, §4.8.5).
// ============================================================================
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atmsim/config/Config.hpp"
#include "atmsim/engine/AtmEngine.hpp"

namespace atmsim {

class LiveRenderer {
public:
    LiveRenderer(AtmEngine& engine, const Config& cfg);

    void start();   // запустить render-поток
    void stop();    // остановить и присоединить
    void pause();   // приостановить перерисовку (для полноэкранных ответов)
    void resume();  // возобновить перерисовку

    // Число строк, которые занимает дашборд (строку ввода консоль рисует ниже).
    int height() const { return height_; }

    // Общий мьютекс вывода в терминал: и рендерер, и консоль пишут через него,
    // чтобы кадр и эхо/ответы не перемешивались.
    std::mutex& outputMutex() { return outMutex_; }

    // Собирает кадр как список строк (с ANSI-цветом, если включён). Публичный,
    // чтобы можно было проверить содержимое кадра юнит-тестом без терминала.
    std::vector<std::string> composeLines() const;

private:
    void renderLoop(std::stop_token stopToken);
    void paintFrame();

    AtmEngine& engine_;
    Config cfg_;

    std::jthread thread_;
    std::atomic<bool> paused_{false};

    std::mutex outMutex_;              // сериализация вывода в stdout
    std::mutex sleepMutex_;            // для прерываемого сна render-цикла
    std::condition_variable_any sleepCv_;

    int height_{0};                    // высота дашборда (число строк)
};

}  // namespace atmsim
