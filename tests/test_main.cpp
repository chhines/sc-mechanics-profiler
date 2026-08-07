#include "test_framework.h"

#include <exception>
#include <iostream>

int main() {
    int failed = 0;
    for (const auto& item : test::cases()) {
        try {
            item.function();
            std::cout << "[PASS] " << item.name << std::endl;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << item.name << "\n       " << error.what() << std::endl;
        }
    }
    std::cout << "\n"
              << test::cases().size() - static_cast<std::size_t>(failed) << '/' << test::cases().size()
              << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
