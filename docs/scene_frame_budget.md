# Бюджет кадра терминала — результаты этапа 0 фичи «сцена»

Прежде чем строить анимированную сцену (ветка `feature/scene`), мы измерили,
сколько стоит вывод кадра в реальных терминалах на машине разработки
(Windows 10 Pro 19045, MSVC 19.51, сборка `/O2`). Инструмент —
[`tools/term_bench.cpp`](../tools/term_bench.cpp), самодостаточный бинарь,
прод-код не затрагивает.

## Методика

Кадр 100×30 клеток (близко к будущей сцене в окне 120×40). Два сценария:

* **storm** — худший случай: каждый кадр меняются ВСЕ клетки, у каждой своя
  SGR-смена цвета (~20 КБ ANSI-текста на кадр). Эмулирует полную перерисовку
  цветной сцены БЕЗ diff-рендера.
* **sparse** — целевой случай diff-рендера: обновляются только ~40 клеток
  (~0.6 КБ на кадр). Так выглядит устоявшаяся сцена, где двигаются
  только человечки.

Каждая комбинация «сценарий × fps × режим таймера» гонится ~4 секунды с
дедлайновым пейсингом (`sleep_until(start + N*period)` — та же схема, что
пойдёт в renderLoop на этапе 5). `hires` = `timeBeginPeriod(1)`.

## Результаты

### Windows Terminal (целевая среда)

```
scenario fps timer avg_write_ms max_write_ms effective_fps missed
storm   10 default    3.370   27.229    10.00    0
storm   10 hires      2.779    5.504    10.00    0
storm   15 default    2.489    7.463    14.99    0
storm   15 hires      2.185    5.127    15.00    0
storm   30 default    1.542    6.678    29.97    0
storm   30 hires      1.699    6.100    29.99    0
sparse  10 default    0.656   17.359     9.97    0
sparse  15 default    0.207    1.122    14.99    0
sparse  30 default    0.172    0.853    29.94    0
```

### Legacy conhost (режим деградации)

```
scenario fps timer avg_write_ms max_write_ms effective_fps missed
storm   10 default   50.367   80.237     9.98    0
storm   10 hires     45.782   62.732    10.00    0
storm   15 default   46.890   63.331    14.99    3
storm   15 hires     42.646   67.104    15.00    1
storm   30 default   42.534   57.746    23.44  119
storm   30 hires     42.822   59.287    23.34  119
sparse  10 default    1.392    3.812     9.97    0
sparse  15 default    0.996    3.099    14.95    0
sparse  30 default    0.993   16.239    29.93    0
```

## Выводы (зафиксированные решения)

1. **Целевой fps сцены — 15** (`ui.scene_fps`, дефолт 15, кламп 5..30).
   В Windows Terminal полный repaint на 15 fps стоит ~2.5 мс (4 % бюджета
   кадра 66 мс) — сцена заработает плавно УЖЕ на этапах 2–4, до появления
   diff-рендера.
2. **FrameDiffer (этап 5) обязателен для conhost, не для WT.** В conhost
   полный repaint упирается в ~23 fps физически (запись 42–50 мс), на 15 fps
   съедает ~70 % бюджета. Diff-кадр (~0.6 КБ) стоит ~1 мс — с ним conhost
   комфортно тянет и 30 fps. WT переваривает всё и без диффа, но dифф
   снижает время удержания `outMutex_` — делаем в любом случае.
3. **`timeBeginPeriod(1)` полезен, но не критичен**: в WT он заметно срезает
   worst-case дрожание записи (27 мс → 5.5 мс на storm-10), просроченных
   дедлайнов и без него нет. Решение по этапу 5: RAII `TimerResolutionGuard`
   через winmm — берём (зависимость приемлемая, включается только на время
   live-режима).
4. **30 fps не нужен**: визуальной разницы для 3-строчных спрайтов между 15 и
   30 fps практически нет, а бюджет ест вдвое. 15 fps — баланс.

## Как перегнать бенчмарк

```
# MSVC (из корня репо, окно закроется само; отчёт — в указанный файл):
"C:\...\vcvars64.bat" && cl /std:c++20 /O2 /EHsc /utf-8 tools\term_bench.cpp /Fe:build\term_bench.exe
build\term_bench.exe report.txt

# g++ (Docker, компилируемость/Linux):
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread tools/term_bench.cpp -o build/term_bench
```

Либо CMake-целью: `cmake -DATMSIM_BUILD_TOOLS=ON ...` → цель `term_bench`.
