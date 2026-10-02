#include "harness.hpp"

#include <cstring>
#include <exception>

namespace pfs::test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int g_failures = 0;

void report_failure(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace pfs::test

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;

    for (const auto& c : pfs::test::registry()) {
        const std::string full = std::string(c.suite) + "." + c.name;
        if (filter && full.find(filter) == std::string::npos) continue;

        ++run;
        pfs::test::g_failures = 0;
        std::printf("[ RUN  ] %s\n", full.c_str());
        try {
            c.fn();
        } catch (const std::exception& e) {
            pfs::test::report_failure(__FILE__, __LINE__,
                                      std::string("uncaught exception: ") + e.what());
        } catch (...) {
            pfs::test::report_failure(__FILE__, __LINE__, "uncaught non-standard exception");
        }
        if (pfs::test::g_failures == 0) {
            std::printf("[  OK  ] %s\n", full.c_str());
        } else {
            std::printf("[ FAIL ] %s (%d failed check(s))\n", full.c_str(),
                        pfs::test::g_failures);
            ++failed;
        }
    }

    std::printf("\n%d test(s) run, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
