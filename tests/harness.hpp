// Minimal test harness. The ported tests are numeric assertions, so a framework
// dependency would buy little; this keeps the build offline and instant.
//
//   PFS_TEST(suite, name) { ... }
//   CHECK(cond); CHECK_NEAR(a, b, tol); CHECK_EQ(a, b); CHECK_THROWS(expr);
//
// Register a test by declaring it with PFS_TEST; main() in harness.cpp runs all
// of them. Pass a substring on the command line to run only matching tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pfs::test {

struct Case {
    const char* suite;
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

// Incremented by the CHECK macros; a test fails if it ends non-zero or throws.
extern int g_failures;

void report_failure(const char* file, int line, const std::string& what);

// Strict `<`, matching the Rust assertions this ports (`(a - b).abs() < tol`).
// A tolerance of exactly 0 means "must be equal", since `diff < 0` is never true.
template <class A, class B>
void check_near(const char* file, int line, const char* ea, const char* eb, A a, B b,
                double tol) {
    const double da = static_cast<double>(a);
    const double db = static_cast<double>(b);
    const double diff = std::fabs(da - db);
    if (!(tol == 0.0 ? diff == 0.0 : diff < tol)) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "CHECK_NEAR(%s, %s, %g): %.9g vs %.9g (diff %.3g)", ea,
                      eb, tol, da, db, std::fabs(da - db));
        report_failure(file, line, buf);
    }
}

}  // namespace pfs::test

#define PFS_TEST(suite, name)                                                            \
    static void pfs_test_##suite##_##name();                                             \
    static ::pfs::test::Registrar pfs_reg_##suite##_##name(#suite, #name,                \
                                                           &pfs_test_##suite##_##name);  \
    static void pfs_test_##suite##_##name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) ::pfs::test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_NEAR(a, b, tol) ::pfs::test::check_near(__FILE__, __LINE__, #a, #b, (a), (b), (tol))

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        if (!((a) == (b)))                                                               \
            ::pfs::test::report_failure(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")");  \
    } while (0)

#define CHECK_THROWS(expr)                                                               \
    do {                                                                                 \
        bool threw = false;                                                              \
        try {                                                                            \
            (void)(expr);                                                                \
        } catch (...) {                                                                  \
            threw = true;                                                                \
        }                                                                                \
        if (!threw)                                                                      \
            ::pfs::test::report_failure(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")");   \
    } while (0)
