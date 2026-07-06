# third_party

Сторонние библиотеки, включённые в репозиторий (vendored), чтобы проект
собирался офлайн, без доступа к сети.

## nlohmann/json

- Файл: `nlohmann/json.hpp` (single-header).
- Версия: **3.11.3**.
- Источник: https://github.com/nlohmann/json (release v3.11.3, `single_include/nlohmann/json.hpp`).
- Лицензия: **MIT** (текст лицензии — в шапке самого файла, SPDX-License-Identifier: MIT).
- Назначение: чтение файла конфигурации (§4.7, §12 ТЗ).

Подключается как **системный** заголовок (`-isystem third_party` / `SYSTEM` в
CMake), чтобы предупреждения из библиотеки не смешивались с нашими. В коде
проекта JSON используется только в `src/config/ConfigLoader.cpp` — остальной
код от библиотеки не зависит.
