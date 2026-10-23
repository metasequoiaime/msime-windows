#include "session/engine_input_session.h"
#include "settings/neural_model.h"
#include "tests/includes/test_utf8_path.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

class DataRoot
{
  public:
    DataRoot()
    {
        wchar_t *previous = nullptr;
        size_t size = 0;
        if (_wdupenv_s(&previous, &size, L"METASEQUOIA_IME_DATA_DIR") != 0)
        {
            throw std::runtime_error("Cannot read benchmark data directory");
        }
        if (previous != nullptr)
        {
            previous_ = previous;
            had_previous_ = true;
            std::free(previous);
        }
        root_ = test::TempRoot() / (L"msime-model-path-bench-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(GetTickCount64()));
        fs::create_directories(root_ / "models");
        if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root_.c_str()) != 0 || metasequoia::data_directory() != root_)
        {
            _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", had_previous_ ? previous_.c_str() : L"");
            std::error_code error;
            fs::remove_all(root_, error);
            throw std::runtime_error("Cannot isolate benchmark data directory");
        }
    }

    ~DataRoot()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", had_previous_ ? previous_.c_str() : L"");
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path &path() const
    {
        return root_;
    }

  private:
    fs::path root_;
    std::wstring previous_;
    bool had_previous_ = false;
};

void CreateModel(const fs::path &file)
{
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    output.put('x');
    if (!output)
    {
        throw std::runtime_error("Cannot create benchmark model file");
    }
}

size_t PercentileIndex(size_t count, size_t percentile)
{
    return ((count - 1) * percentile + 99) / 100;
}

template <typename Fn>
void Measure(const std::string &name, size_t iterations, bool expect_found, Fn fn, size_t warmup = 1000)
{
    std::vector<long long> samples;
    samples.reserve(iterations);
    for (size_t index = 0; index < warmup; ++index)
    {
        (void)fn(index);
    }
    size_t found = 0;
    for (size_t index = 0; index < iterations; ++index)
    {
        const auto start = Clock::now();
        const auto result = fn(index);
        const auto end = Clock::now();
        found += !result.empty();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    if (found != (expect_found ? iterations : 0))
    {
        throw std::runtime_error("Unexpected resolver result in " + name);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << name << ',' << iterations << ',' << samples[PercentileIndex(iterations, 50)] << ','
              << samples[PercentileIndex(iterations, 95)] << ',' << samples[PercentileIndex(iterations, 99)] << ','
              << samples.back() << ',' << found << '\n';
}

class DirectoryContention
{
  public:
    explicit DirectoryContention(const fs::path &root)
    {
        worker_ = std::thread([this, root] {
            size_t index = 0;
            while (!stop_.load(std::memory_order_relaxed))
            {
                const auto file = root / "models" / ("churn-" + std::to_string(index++ % 128));
                std::error_code error;
                {
                    std::ofstream output(file, std::ios::binary);
                    output.put('x');
                }
                fs::remove(file, error);
            }
        });
    }

    ~DirectoryContention()
    {
        stop_.store(true, std::memory_order_relaxed);
        worker_.join();
    }

  private:
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

void RunScenarios(size_t iterations, const std::vector<std::string> &present, const std::vector<std::string> &missing,
                  const std::string &prefix)
{
    std::vector<std::string> first_touch;
    first_touch.reserve(iterations);
    for (size_t index = 0; index < iterations; ++index)
    {
        first_touch.push_back(prefix + "first-touch-" + std::to_string(index));
    }
    Measure(prefix + "collocation-present-hot", iterations, true,
            [&](size_t) { return ResolveCollocationModelPath(present.front()); });
    Measure(prefix + "collocation-missing-hot", iterations, false,
            [&](size_t) { return ResolveCollocationModelPath(missing.front()); });
    Measure(prefix + "collocation-present-rotating", iterations, true,
            [&](size_t index) { return ResolveCollocationModelPath(present[index % present.size()]); });
    Measure(prefix + "collocation-missing-rotating", iterations, false,
            [&](size_t index) { return ResolveCollocationModelPath(missing[index % missing.size()]); });
    Measure(
        prefix + "collocation-missing-first-touch", iterations, false,
        [&](size_t index) { return ResolveCollocationModelPath(first_touch[index]); }, 0);
    Measure(prefix + "neural-present-hot", iterations, true, [](size_t) { return ResolveNeuralDesktopModelPath(); });
    Measure(prefix + "data-directory-only", iterations, true, [](size_t) { return metasequoia::data_directory(); });
    Measure(prefix + "disabled-empty-id", iterations, false, [](size_t) { return ResolveCollocationModelPath(""); });
}
} // namespace

int RunModelPathBenchmark(int argc, char **argv)
{
    try
    {
        size_t iterations = 10000;
        if (argc > 2)
        {
            throw std::invalid_argument("Usage: --benchmark-model-path [iterations]");
        }
        if (argc > 1)
        {
            const std::string argument(argv[1]);
            if (argument.empty() ||
                !std::all_of(argument.begin(), argument.end(), [](char digit) { return digit >= '0' && digit <= '9'; }))
            {
                throw std::invalid_argument("Iteration count must contain digits only");
            }
            const auto requested = std::stoull(argument);
            if (requested == 0 || requested > 100000)
            {
                throw std::invalid_argument("Iteration count must be between 1 and 100000");
            }
            iterations = static_cast<size_t>(requested);
        }

        DataRoot data;
        std::vector<std::string> present, missing;
        for (size_t index = 0; index < 128; ++index)
        {
            present.push_back("present-" + std::to_string(index));
            missing.push_back("missing-" + std::to_string(index));
            const auto &id = present.back();
            CreateModel(data.path() / "models" / id / (id + ".gram"));
        }
        CreateModel(neural_model::ModelFile());
        std::cout << "scenario,iterations,p50_ns,p95_ns,p99_ns,worst_ns,found\n";
        RunScenarios(iterations, present, missing, "idle-");
        fs::remove(neural_model::ModelFile());
        Measure("idle-neural-missing-hot", iterations, false, [](size_t) { return ResolveNeuralDesktopModelPath(); });
        CreateModel(neural_model::ModelFile());
        {
            DirectoryContention contention(data.path());
            RunScenarios(iterations, present, missing, "contended-");
            fs::remove(neural_model::ModelFile());
            Measure("contended-neural-missing-hot", iterations, false,
                    [](size_t) { return ResolveNeuralDesktopModelPath(); });
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Model path benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
