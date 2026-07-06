#pragma once
// ============================================================================
//  simple_test.hpp — минимальный тест-харнес (временный, на период без сети).
//
//  Почему свой, а не GoogleTest/Catch2: окружение разработки сейчас без доступа
//  к сети, а эти библиотеки подтягиваются через FetchContent. Как только сеть
//  появится — перейдём на GoogleTest (§11 ТЗ). Харнес намеренно крошечный и
//  прозрачный, чтобы его можно было прочитать целиком:
//    * макрос TEST(name)  — объявляет тест-функцию и регистрирует её ещё до main;
//    * CHECK(cond)        — проверяет условие, печатает файл:строку при провале;
//    * CHECK_EQ(a, b)     — то же, но печатает и сами значения;
//    * st::runAll()       — прогоняет все тесты, печатает итог, возвращает код
//                           возврата (0 — всё зелёное, 1 — были провалы).
// ============================================================================
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace st {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

// Единые счётчики и реестр. Обёртки со static-переменной внутри гарантируют, что
// на всю программу они одни, даже если заголовок включён в несколько .cpp
// (inline-функция имеет единственное определение и единственный static).
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& checks()   { static int c = 0; return c; }
inline int& failures() { static int f = 0; return f; }

// Регистратор: его конструктор кладёт тест в реестр во время инициализации
// глобальных объектов — то есть ещё до входа в main().
struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline void reportFail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::cerr << "    [FAIL] " << file << ':' << line << " — " << msg << '\n';
}

// Сравнение с печатью значений. Требует, чтобы типы поддерживали == и вывод в
// поток (operator<<). Для перечислений сравниваем через CHECK(a == b).
template <class A, class B>
inline void checkEq(const A& a, const B& b, const char* ea, const char* eb,
                    const char* file, int line) {
    ++checks();
    if (!(a == b)) {
        std::ostringstream os;
        os << "CHECK_EQ(" << ea << ", " << eb << ") — слева=" << a << ", справа=" << b;
        reportFail(file, line, os.str());
    }
}

inline int runAll() {
    int failedTests = 0;
    for (auto& tc : registry()) {
        const int before = failures();
        try {
            tc.fn();
        } catch (const std::exception& e) {
            reportFail(__FILE__, __LINE__, std::string("исключение: ") + e.what());
        } catch (...) {
            reportFail(__FILE__, __LINE__, "неизвестное исключение");
        }
        const bool ok = failures() == before;
        std::cout << (ok ? "[  OK  ] " : "[ FAIL ] ") << tc.name << '\n';
        if (!ok) ++failedTests;
    }
    std::cout << "\nИтог: тестов " << registry().size()
              << ", проверок " << checks()
              << ", провалов " << failures() << '\n';
    return failedTests == 0 ? 0 : 1;
}

}  // namespace st

// Объявляет тест-функцию и одновременно регистрирует её через глобальный объект.
#define TEST(name)                                            \
    static void name();                                       \
    static ::st::Registrar registrar_##name(#name, name);     \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++::st::checks();                                                      \
        if (!(cond)) ::st::reportFail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b) ::st::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
