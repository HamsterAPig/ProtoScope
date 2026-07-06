#include "test_registry.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <unordered_set>

int main()
{
    int failed = 0;
    const auto* tests = allTests();
    const int total = testCount();
    std::unordered_set<std::string_view> testNames;
    for (int i = 0; i < total; ++i) {
        const std::string_view name{tests[i].name};
        if (!testNames.insert(name).second) {
            std::cerr << "[FAIL] duplicate test name: " << name << "\n";
            return 1;
        }
    }

    const char* filterEnv = std::getenv("PROTOSCOPE_TEST_FILTER");
    const std::string_view filter = filterEnv == nullptr ? std::string_view{} : std::string_view{filterEnv};
    int selected = 0;

    for (int i = 0; i < total; ++i) {
        const std::string_view name{tests[i].name};
        if (!filter.empty() && name.find(filter) == std::string_view::npos) {
            continue;
        }
        ++selected;
        try {
            tests[i].run();
            std::cout << "[PASS] " << tests[i].name << "\n" << std::flush;
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << tests[i].name << ": " << ex.what() << "\n" << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << tests[i].name << ": unknown exception\n" << std::flush;
        }
    }

    std::cout << "total=" << total << " selected=" << selected << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
