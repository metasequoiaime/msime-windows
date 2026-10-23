#include "tests/includes/test_framework.h"

#include <cstdio>
#include <exception>

namespace test
{
std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char *name, std::function<void()> fn)
{
    registry().push_back({name, std::move(fn)});
}
} // namespace test

int main()
{
    // Unbuffered, so a test that crashes the process still leaves the results before it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto &tests = test::registry();

    // A suite that registers nothing would otherwise exit successfully and look
    // exactly like a suite that passed, which is worse than having no suite.
    if (tests.empty())
    {
        std::printf("No tests were registered.\n");
        return 1;
    }

    int failures = 0;
    for (const auto &test_case : tests)
    {
        try
        {
            test_case.fn();
            std::printf("[PASS] %s\n", test_case.name.c_str());
        }
        catch (const std::exception &ex)
        {
            ++failures;
            std::printf("[FAIL] %s: %s\n", test_case.name.c_str(), ex.what());
        }
    }

    std::printf("%zu test(s) run, %d failed\n", tests.size(), failures);
    return failures == 0 ? 0 : 1;
}
