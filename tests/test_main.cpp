#include "test_registry.hpp"

#include <exception>
#include <iostream>

int main() {
    int failed = 0;
    const auto* tests = allTests();
    const int total = testCount();

    for (int i = 0; i < total; ++i) {
        try {
            tests[i].run();
            std::cout << "[PASS] " << tests[i].name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << tests[i].name << ": " << ex.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << tests[i].name << ": unknown exception\n";
        }
    }

    std::cout << "total=" << total << " failed=" << failed << "\n";
    return failed == 0 ? 0 : 1;
}
