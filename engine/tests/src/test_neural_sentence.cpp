// 神经整句重排的正确性与延迟测试。
//
// 模型现在只重排词格的 n-best，不再自己造句（见 engine/neural/neural_decoder.h）。快是靠两件
// 事换来的：前文的 K/V 缓存下来不重算，以及所有候选拼成一批一起过每一层。这两件事都可能算错
// 而不报错——位置编码串行、padding 行漏掉、缓存没失效，都只会让分数悄悄变样。所以主检查项是
// 对拍：score_sentences 的结果必须等于逐字调用 next_log_probabilities 累加出来的参考值，后者
// 是与 Rust 参考实现对齐过的那条老路径。
//
// 第三件事是打分挪到了后台线程（engine/neural/rescore_worker.h），所以走词典的那段集成检查只看得
// 到静态顺序——重排要等下一次重查。后台那条路单独测：请求一次必落空，回调到了再请求必命中。
//
// 缺 sentence-model*.safetensors 时与其它集成测试一样优雅跳过（返回 0）。用
// METASEQUOIA_IME_DATA_DIR 指到备好资源的目录来实际跑通。

#include "core/sentence_association_options.h"
#include "core/input_session.h"
#include "core/word_item.h"
#include "neural/neural_decoder.h"
#include "neural/rescore_worker.h"
#include "neural/sentence_model.h"
#include "quanpin/quanpin_dictionary.h"
#include "shuangpin/shuangpin_dictionary.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void expect_near(double actual, double expected, double tolerance, const std::string &message)
{
    if (!(std::fabs(actual - expected) <= tolerance))
    {
        std::fprintf(stderr, "FAIL: %s (got %.6f, want %.6f)\n", message.c_str(), actual, expected);
        ++failures;
    }
}

std::vector<std::string> characters(const std::string &text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();)
    {
        std::size_t width = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0xF8) == 0xF0)
            width = 4;
        else if ((c & 0xF0) == 0xE0)
            width = 3;
        else if ((c & 0xE0) == 0xC0)
            width = 2;
        width = std::min(width, text.size() - i);
        out.push_back(text.substr(i, width));
        i += width;
    }
    return out;
}

char32_t code_point(const std::string &character)
{
    const unsigned char c = static_cast<unsigned char>(character[0]);
    if (c < 0x80)
        return c;
    char32_t code;
    std::size_t extra;
    if ((c & 0xE0) == 0xC0)
    {
        code = c & 0x1F;
        extra = 1;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        code = c & 0x0F;
        extra = 2;
    }
    else
    {
        code = c & 0x07;
        extra = 3;
    }
    for (std::size_t j = 1; j <= extra && j < character.size(); ++j)
        code = (code << 6) | (static_cast<unsigned char>(character[j]) & 0x3F);
    return code;
}

// 参考实现：一个字一个字地问「下一个字的分布」，取出实际那个字的 log 概率累加。每一步都重跑
// 整个前缀，正是重构掉的那条慢路径——留在这里当标尺。
double reference_score(const neural::SentenceModel &model, const std::string &context, const std::string &text)
{
    double total = 0.0;
    std::string prefix = context;
    for (const std::string &character : characters(text))
    {
        const std::vector<float> distribution = model.next_log_probabilities(prefix);
        // 词表外的字按 <unk> 记，与 SentenceModel::encode 一致。
        const std::optional<std::uint32_t> id = model.token(code_point(character));
        total += static_cast<double>(distribution[id ? *id : 1u]);
        prefix += character;
    }
    return total;
}
} // namespace

int run_test()
{
    const metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy();
    const neural::SentenceModel *model = neural::shared_sentence_model(
        metasequoia::path_to_utf8(paths.resource(metasequoia::assets::neural_model_keyboard)));
    const neural::SentenceModel *desktop_model = neural::shared_sentence_model(
        metasequoia::path_to_utf8(paths.resource(metasequoia::assets::neural_model_desktop)));
    if (model == nullptr)
    {
        std::printf("Skipped: neural model (sentence-model.safetensors) not present in data directory.\n");
        return 0;
    }

    // ---- 对拍：批量 + 缓存的结果必须等于逐字参考值 ----
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"", {"你好世界", "拟好世界"}},
        {"我今天想去", {"上海", "伤害", "吃饭"}},
        // 长短不一，逼出 padding 行；最短的一条只有一个字。
        {"明天的会议", {"推迟到下周一", "推迟", "退", "推迟到下周义"}},
    };
    for (const auto &[context, texts] : cases)
    {
        const std::vector<double> actual = model->score_sentences(context, texts);
        expect(actual.size() == texts.size(), "score_sentences should return one score per text");
        if (actual.size() != texts.size())
            continue;
        for (std::size_t i = 0; i < texts.size(); ++i)
        {
            // 容差放在 1e-3：两条路径的浮点累加次序不同，但结构上必须是同一个数。
            expect_near(actual[i], reference_score(*model, context, texts[i]), 1e-3,
                        "batched score should match the per-character reference for '" + texts[i] + "'");
            expect(actual[i] < 0.0, "a log probability should be negative for '" + texts[i] + "'");
        }
    }

    // Empty candidates have no targets. They must not read a padding logit.
    {
        const std::vector<std::string> texts = {"", "上海", "", "伤害"};
        const std::vector<double> scores = model->score_sentences("我今天想去", texts);
        expect(scores.size() == texts.size(), "empty candidates should keep their batch positions");
        if (scores.size() == texts.size())
        {
            for (std::size_t i = 0; i < texts.size(); ++i)
            {
                expect_near(scores[i], reference_score(*model, "我今天想去", texts[i]), 1e-3,
                            "empty candidates should not affect other scores");
            }
        }
        expect(model->score_sentences("", {""}) == std::vector<double>{0.0},
               "a batch without targets should score zero");
    }

    // ---- 批量与单条一致：padding 行不能污染同批的其它行 ----
    {
        const std::string context = "我今天想去";
        const std::vector<std::string> texts = {"上海", "伤害", "吃饭"};
        const std::vector<double> batched = model->score_sentences(context, texts);
        for (std::size_t i = 0; i < texts.size(); ++i)
        {
            const std::vector<double> alone = model->score_sentences(context, {texts[i]});
            expect_near(alone[0], batched[i], 1e-4, "scoring alone should match scoring in a batch");
        }
    }

    // ---- 前文缓存必须随前文失效，且换回来后结果不变 ----
    {
        const std::vector<std::string> texts = {"上海"};
        const double first = model->score_sentences("我今天想去", texts)[0];
        const double other = model->score_sentences("我昨天去了", texts)[0];
        const double again = model->score_sentences("我今天想去", texts)[0];
        expect_near(again, first, 1e-6, "the same context must score the same after the cache was evicted");
        expect(std::fabs(other - first) > 1e-6, "a different context must actually change the score");
    }

    // ---- 超长输入不能崩：前文与候选都会被截断 ----
    {
        std::string long_context;
        for (int i = 0; i < 80; ++i)
            long_context += "很长的前文。";
        expect(model->score_sentences(long_context, {"上海"})[0] < 0.0, "an over-long context should still score");
        std::string long_text;
        for (int i = 0; i < 200; ++i)
            long_text += "字";
        expect(model->score_sentences("", {long_text})[0] < 0.0, "an over-long candidate should still score");
    }

    // ---- rerank_order 的契约 ----
    {
        const std::vector<std::string> sentences = {"伤害", "上海"};
        // 词格分（log10）打平，让神经分说了算。
        const std::vector<double> flat = {-5.0, -5.0};
        const std::vector<std::size_t> order = neural::rerank_order(*model, "我今天想去", sentences, flat);
        expect(order.size() == sentences.size(), "rerank_order should return a full permutation");
        if (order.size() == sentences.size())
        {
            expect(order[0] == 1, "with the lattice tied, the model should prefer 上海 after 我今天想去");
        }
        // lambda 为 0 等于不重排，调用方必须能看出「没排」。
        neural::RerankOptions off;
        off.lambda = 0.0;
        expect(neural::rerank_order(*model, "", sentences, flat, off).empty(),
               "lambda of zero should report that no reranking happened");
        expect(neural::rerank_order(*model, "", {"只有一条"}, {-5.0}).empty(), "a single path cannot be reranked");
    }

    // ---- 前文确实在起作用，且按字数（不是字节数）裁剪 ----
    {
        // 前文不是摆设：同一个候选换个前文，分数必须跟着变。上屏历史一路传到这里的
        // 链路（InputSession -> QueryRequest -> 词典 -> make_neural_reranker）断在任何
        // 一环，模型拿到的都是空串，这一条就会挂。
        const double after_go = model->score_sentences("我今天想去", {"上海"})[0];
        const double after_hurt = model->score_sentences("他对我造成了很大的", {"上海"})[0];
        expect(after_go != after_hurt, "the committed text must reach the model, not be dropped on the way");
        expect(after_go > after_hurt, "上海 should fit 我今天想去 better than 他对我造成了很大的");

        // 上屏历史整段给进来，模型只看末尾若干字；裁剪按 UTF-8 字符边界，不能切出半个汉字。
        expect(neural::last_characters("我今天想去上海", 2) == "上海", "the tail should be counted in characters");
        expect(neural::last_characters("上海", 99) == "上海", "a short context should come back whole");
        expect(neural::last_characters("上海", 0).empty(), "a zero-character window should yield no context");
    }

    // ---- 后台线程：第一次必落空，算完之后同一批必命中 ----
    {
        const std::vector<std::string> sentences = {"伤害", "上海"};
        const std::vector<double> flat = {-5.0, -5.0};
        std::mutex mutex;
        std::condition_variable ready;
        bool fired = false;
        neural::RescoreWorker &worker = neural::RescoreWorker::instance();
        worker.clear();
        // notify 放在锁内：等待方一拿到锁就会离开作用域、析构 ready，锁外 notify 可能碰到已析构的对象。
        worker.set_ready_callback([&] {
            std::lock_guard<std::mutex> lock(mutex);
            fired = true;
            ready.notify_all();
        });

        // 按键路径上的那一次：不等模型，立刻返回空，调用方照词格顺序出候选。
        expect(!worker.order_for(*model, "我今天想去", sentences, flat, {}).has_value(),
               "the first request must not block the caller waiting for the model");

        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(ready.wait_for(lock, std::chrono::seconds(30), [&] { return fired; }),
                   "the background thread should finish and call back");
        }

        // 重查那一次：结果表里已经有顺序了。
        const auto order = worker.order_for(*model, "我今天想去", sentences, flat, {});
        expect(order.has_value(), "the finished order should be waiting for the requery");
        if (order && order->size() == 2)
        {
            expect((*order)[0] == 1, "the background result should be the same ordering as the direct call");
        }
        worker.set_ready_callback(nullptr);
        worker.clear();
    }

    // ---- 回调抛异常不能让后台线程死掉 ----
    //
    // run() 跑在 detach 出来的线程上，异常逃出线程函数就是 std::terminate，整个输入法跟着没。
    // 回调是调用方（server 的 EnqueueRescoredCandidates）给的代码，引擎管不住它会不会抛。
    //
    // 断言的不是「异常被吞了」，而是线程还活着：抛完这一轮之后还能不能接下一批活。顺带钉住
    // busy_workers_ 的收尾——异常路径上漏掉它，shutdown() 会永远等一个已经死掉的线程。
    if (model != nullptr)
    {
        neural::RescoreWorker &worker = neural::RescoreWorker::instance();
        worker.clear();
        std::mutex mutex;
        std::condition_variable done;
        int fired = 0;
        worker.set_ready_callback([&] {
            bool throw_now = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                throw_now = (++fired == 1);
                done.notify_all();
            }
            if (throw_now)
            {
                throw std::runtime_error("the host callback failed on purpose");
            }
        });

        const std::vector<std::string> first = {"伤害", "上海"};
        const std::vector<double> flat = {-5.0, -5.0};
        expect(!worker.order_for(*model, "我今天想去", first, flat, {}).has_value(),
               "the throwing run should be enqueued like any other");
        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(done.wait_for(lock, std::chrono::seconds(30), [&] { return fired >= 1; }),
                   "the callback should have been reached");
        }

        // 线程必须还活着。同一模型同一批候选会命中已经存好的结果，所以换一批，强制它再打一次分。
        const std::vector<std::string> second = {"吃饭", "上海"};
        expect(!worker.order_for(*model, "我今天想去", second, flat, {}).has_value(),
               "a fresh batch should be enqueued after the throwing one");
        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(done.wait_for(lock, std::chrono::seconds(30), [&] { return fired >= 2; }),
                   "the worker must survive a throwing callback and keep serving");
        }
        expect(worker.order_for(*model, "我今天想去", second, flat, {}).has_value(),
               "the batch after the throwing one should still be cached");

        worker.set_ready_callback(nullptr);
        worker.clear();
    }

    // 两个模型共用一个后台线程，但待办和结果必须按模型隔离，不能后提交的覆盖前一个。
    if (desktop_model != nullptr)
    {
        const std::vector<std::string> sentences = {"伤害", "上海"};
        const std::vector<double> flat = {-5.0, -5.0};
        std::mutex mutex;
        std::condition_variable ready;
        int fired = 0;
        neural::RescoreWorker &worker = neural::RescoreWorker::instance();
        worker.clear();
        worker.set_ready_callback([&] {
            std::lock_guard<std::mutex> lock(mutex);
            ++fired;
            ready.notify_all();
        });

        expect(!worker.order_for(*desktop_model, "我今天想去", sentences, flat, {}).has_value(),
               "the desktop model should enqueue its own first request");
        expect(!worker.order_for(*model, "我今天想去", sentences, flat, {}).has_value(),
               "the keyboard model should enqueue without replacing the desktop request");
        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(ready.wait_for(lock, std::chrono::seconds(30), [&] { return fired >= 2; }),
                   "both neural models should finish independently");
        }
        expect(worker.order_for(*desktop_model, "我今天想去", sentences, flat, {}).has_value(),
               "the desktop result should remain in its own cache entry");
        expect(worker.order_for(*model, "我今天想去", sentences, flat, {}).has_value(),
               "the keyboard result should remain in its own cache entry");
        worker.set_ready_callback(nullptr);
        worker.clear();
    }

    // ---- 正在算的那一批，重查时不能再排一份 ----
    //
    // 回调触发的重查会问到「刚排上、已被后台取走、还没算完」的这一批：表里没有、待办位空着。用户连
    // 续打字时这正是常态，不拦就是每个回调白算一次。先用第一批的回调卡住执行槽，让目标批次排在后
    // 面；放开后它一进入计算就去重查，之后只能再等到它自己那一次回调。
    {
        const std::string context = "今天下午的会议讨论了输入法的排序问题，大家觉得整句转换还可以再准一些，";
        const std::vector<std::string> blocker = {"伤害", "上海"};
        const std::vector<std::string> target = {"我们明天再讨论一下", "我们明天在讨论一下", "我们名天再讨论一下",
                                                 "我门明天再讨论一下", "我们明天再讨论以下", "我们明天再讨论一夏",
                                                 "我们明天再讨论一吓", "我们明天再讨论议下", "我们明天在讨论以下",
                                                 "我们名天在讨论一下", "我们明天再讨论一霞", "我们明天再讨论一侠"};
        const std::vector<double> blocker_scores(blocker.size(), -5.0);
        const std::vector<double> target_scores(target.size(), -5.0);
        std::mutex mutex;
        std::condition_variable changed;
        int fired = 0;
        bool released = false;
        neural::RescoreWorker &worker = neural::RescoreWorker::instance();
        worker.clear();
        worker.set_ready_callback([&] {
            std::unique_lock<std::mutex> lock(mutex);
            ++fired;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        });

        expect(!worker.order_for(*model, "门卫", blocker, blocker_scores, {}).has_value(),
               "the blocking batch should be enqueued");
        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(changed.wait_for(lock, std::chrono::seconds(30), [&] { return fired >= 1; }),
                   "the blocking batch should reach its callback");
        }
        // 执行槽还被第一批的回调占着，这一批只能排队。
        const auto request_started = std::chrono::steady_clock::now();
        expect(!worker.order_for(*model, context, target, target_scores, {}).has_value(),
               "the target batch should be enqueued behind the blocker");
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
            changed.notify_all();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool requery_missed = !worker.order_for(*model, context, target, target_scores, {}).has_value();
        {
            std::unique_lock<std::mutex> lock(mutex);
            expect(changed.wait_for(lock, std::chrono::seconds(30), [&] { return fired >= 2; }),
                   "the target batch should finish and call back");
        }
        const auto target_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - request_started).count();
        if (requery_missed)
        {
            // 若重查又排了一份，它会在大约一次打分的时间后回调。多等几倍，确认没有第三次。
            const auto extra = std::chrono::milliseconds(static_cast<long long>(target_ms * 3) + 50);
            std::unique_lock<std::mutex> lock(mutex);
            expect(!changed.wait_for(lock, extra, [&] { return fired >= 3; }),
                   "a requery while the batch is being scored must not enqueue it again");
        }
        expect(worker.order_for(*model, context, target, target_scores, {}).has_value(),
               "the in-flight batch should land in the result table");
        worker.set_ready_callback(nullptr);
        worker.clear();
    }

    // ---- 延迟：这次重构的全部意义 ----
    {
        const std::string context = "今天下午的会议讨论了输入法的排序问题，大家觉得整句转换还可以再准一些，";
        const std::vector<std::string> texts = {"我们明天再讨论一下", "我们明天在讨论一下", "我们名天再讨论一下",
                                                "我门明天再讨论一下", "我们明天再讨论以下", "我们明天再讨论一夏",
                                                "我们明天再讨论一吓", "我们明天再讨论议下", "我们明天在讨论以下",
                                                "我们名天在讨论一下", "我们明天再讨论一霞", "我们明天再讨论一侠"};
        model->score_sentences(context, texts); // 预热，把前文灌进缓存
        const auto start = std::chrono::steady_clock::now();
        constexpr int kRuns = 10;
        for (int i = 0; i < kRuns; ++i)
            model->score_sentences(context, texts);
        const double cached_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kRuns;

        const auto cold_start = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i)
            model->score_sentences(context + std::to_string(i), texts); // 每次换前文，逼它重算缓存
        const double cold_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_start).count() / kRuns;

        std::printf("12 candidates, 34-char context: %.2f ms cached, %.2f ms recomputed\n", cached_ms, cold_ms);
    }

    // Sharing texts between sentence and dictionary scoring must not reuse incompatible results.
    {
        auto &worker = neural::RescoreWorker::instance();
        const std::vector<std::string> texts = {"学", "雪"};
        const auto wait_order = [&](const std::vector<double> &priors, const neural::RerankOptions &options) {
            std::optional<std::vector<std::size_t>> order;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            do
            {
                order = worker.order_for(*model, "冬天会下大", texts, priors, options);
                if (order)
                    return *order;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (std::chrono::steady_clock::now() < deadline);
            return std::vector<std::size_t>{};
        };
        expect(wait_order({0.0, 0.0}, {}) == std::vector<std::size_t>({1, 0}), "flat priors should favor snow");
        expect(wait_order({1000000.0, 0.0}, {}) == std::vector<std::size_t>({0, 1}),
               "changed priors must not reuse an earlier cached order");
        neural::RerankOptions one;
        one.max_paths = 1;
        expect(wait_order({0.0, 0.0}, one).empty(), "changed options must not reuse an earlier cached order");
    }

    for (const char *context :
         {"冬天会下大", "我们正在学校里上", "他对我造成了很大的", "我明天要上", "冬天会下大雪我明天要上"})
    {
        const std::vector<std::string> words = {"学", "雪", "血", "上海", "伤害"};
        const auto scores = model->score_sentences(context, words);
        std::printf("Context %s:", context);
        for (std::size_t i = 0; i < scores.size(); ++i)
            std::printf(" %s=%.4f", words[i].c_str(), scores[i]);
        std::printf("\n");
        if (desktop_model != nullptr)
        {
            const auto desktop_scores = desktop_model->score_sentences(context, words);
            std::printf("Desktop %s:", context);
            for (std::size_t i = 0; i < desktop_scores.size(); ++i)
                std::printf(" %s=%.4f", words[i].c_str(), desktop_scores[i]);
            std::printf("\n");
        }
    }

    // Dictionary reranking must move existing rows, preserving their identity and other sources.
    {
        std::vector<WordItem> original = {
            {"xue", "学", 10000, CandidateSource::Database, "xue"},
            {"xue", "额外候选", 1, CandidateSource::CloudSuggestion, "xue"},
            {"xue", "雪", 8000, CandidateSource::Database, "xue"},
            {"xue", "血", 1000, CandidateSource::Database, "xue"},
            {"x", "下", 10000, CandidateSource::Database, "xia"},
        };
        original[2].fixed_position = 3;
        const auto unchanged = [&](const std::string &key, const neural::SentenceModel *selected,
                                   const std::string &context) {
            auto rows = original;
            quanpin::rerank_dictionary_candidates(rows, key, selected, context);
            for (std::size_t i = 0; i < rows.size(); ++i)
                expect(rows[i].word == original[i].word, "ineligible request should keep the original order");
        };
        unchanged("xue", model, "");
        unchanged("xue", nullptr, "冬天会下大");
        unchanged("x", model, "冬天会下大");
        std::vector<WordItem> rows;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            rows = original;
            quanpin::rerank_dictionary_candidates(rows, "xue", model, "冬天会下大");
            if (rows.front().word == "雪")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        std::printf("Dictionary helper winner: %s\n", rows.front().word.c_str());
        expect(rows.front().word == "雪", "committed winter context should promote dictionary 雪 over 学");
        expect(rows[1].word == original[1].word && rows[4].word == original[4].word,
               "online and prefix rows must retain their slots");
        for (const auto &row : rows)
        {
            const auto before =
                std::find_if(original.begin(), original.end(), [&](const auto &item) { return item.word == row.word; });
            expect(before != original.end() && row.weight == before->weight && row.source == before->source &&
                       row.pinyin == before->pinyin && row.canonical_pinyin == before->canonical_pinyin &&
                       row.fixed_position == before->fixed_position,
                   "reranking must preserve dictionary metadata and stored weights");
        }
    }

    // Real dictionary paths: both full pinyin and shuangpin, including a cached requery.
    {
        QuanpinDictionary dictionary;
        ShuangpinDictionary shuangpin;
        SentenceAssociationOptions options;
        const auto baseline = dictionary.query("xue");
        expect(!baseline.empty(), "dictionary fixture must contain xue");
        options.context_candidate_rerank = true;
        dictionary.set_sentence_association(options);
        shuangpin.set_sentence_association(options);
        dictionary.set_rescoring_context("冬天会下大");
        shuangpin.set_rescoring_context("冬天会下大");
        const auto wait_first = [&](auto query, auto reset, const std::string &wanted) {
            std::vector<WordItem> rows;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            do
            {
                reset(); // Same invalidation used by the server's ready callback.
                rows = query();
                if (!rows.empty() && rows.front().word == wanted)
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        };
        expect(wait_first([&] { return dictionary.query("xue"); }, [&] { dictionary.reset_sentence_cache(); }, "雪"),
               "quanpin xue should promote 雪 after winter context");
        // Default shuangpin profile: xt = xue.
        expect(wait_first([&] { return shuangpin.generateSeries("xt", "xt"); },
                          [&] { shuangpin.reset_sentence_cache(); }, "雪"),
               "shuangpin xt should promote 雪 after winter context");
        dictionary.set_rescoring_context("我明天要上");
        expect(wait_first([&] { return dictionary.query("xue"); }, [&] { dictionary.reset_sentence_cache(); }, "学"),
               "a fresh tomorrow-school context should promote 学");
        dictionary.set_rescoring_context("我们正在学校里上");
        expect(wait_first([&] { return dictionary.query("xue"); }, [&] { dictionary.reset_sentence_cache(); }, "学"),
               "a changed context should replace the cached snow order with 学");
        dictionary.set_rescoring_context("我今天想去");
        expect(wait_first([&] { return dictionary.query("shanghai"); }, [&] { dictionary.reset_sentence_cache(); },
                          "上海"),
               "multi-syllable dictionary words should also use context");
        dictionary.set_rescoring_context("他对我造成了很大的");
        expect(wait_first([&] { return dictionary.query("shanghai"); }, [&] { dictionary.reset_sentence_cache(); },
                          "伤害"),
               "the same word candidates should follow changed context");
        dictionary.set_rescoring_context("");
        auto no_context = dictionary.query("xue");
        expect(no_context.size() == baseline.size(), "clearing context must restore the full baseline list");
        for (std::size_t i = 0; i < std::min(no_context.size(), baseline.size()); ++i)
            expect(no_context[i].word == baseline[i].word, "empty context should restore frequency order");
        options.context_candidate_rerank = false;
        dictionary.set_sentence_association(options);
        dictionary.set_rescoring_context("冬天会下大");
        const auto disabled = dictionary.query("xue");
        expect(disabled.size() == baseline.size(), "disabling reranking must preserve all candidates");
        for (std::size_t i = 0; i < std::min(disabled.size(), baseline.size()); ++i)
            expect(disabled[i].word == baseline[i].word, "disabled reranking must restore frequency order");
    }

    // The product uses InputSession; fixed positions must still override contextual ranking.
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, false, true, false);
        SentenceAssociationOptions options;
        options.context_candidate_rerank = true;
        session.set_sentence_association(options);
        session.set_rescoring_context("冬天会下大");
        for (char c : std::string("xue"))
            session.handle_character(c);
        const auto started = std::chrono::steady_clock::now();
        do
        {
            session.reset_sentence_cache();
            session.recompute_candidates();
            if (!session.candidates().empty() && session.candidates().front().word == "雪")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        expect(!session.candidates().empty() && session.candidates().front().word == "雪",
               "the product input session should expose contextual dictionary order");
        session.enable_fixed_positions();
        const auto found = std::find_if(session.candidates().begin(), session.candidates().end(),
                                        [](const auto &item) { return item.word == "学"; });
        expect(found != session.candidates().end(), "the frequency favorite must remain selectable");
        if (found != session.candidates().end())
        {
            const auto index = static_cast<std::size_t>(found - session.candidates().begin());
            expect(session.set_candidate_position(index, 1).handled, "pinning a dictionary candidate should succeed");
            expect(session.candidates().front().word == "学", "a manual first position must override the model");
            session.reset_sentence_cache();
            session.recompute_candidates();
            expect(session.candidates().front().word == "学", "an async requery must preserve a manual first position");
            expect(session.set_candidate_position(0, 0).handled, "the test pin should be removed");
        }
    }

    // ---- 集成：两种神经模式可同时提交重排，且不泄漏词格自己的首选 ----
    {
        QuanpinDictionary dictionary;
        SentenceAssociationOptions options;
        options.neural_desktop = true;
        options.neural_desktop_model =
            metasequoia::path_to_utf8(paths.resource(metasequoia::assets::neural_model_desktop));
        options.neural_keyboard = true;
        dictionary.set_sentence_association(options);
        for (const char *query : {"nihaoshijie", "womenzaijianmian", "jintiantianqizhenhao"})
        {
            const std::vector<WordItem> result = dictionary.query(query);
            expect(!result.empty(), std::string("reranked query should still produce candidates for ") + query);
            int lattice_rows = 0;
            int desktop_rows = 0;
            int keyboard_rows = 0;
            for (const WordItem &item : result)
            {
                expect(!item.word.empty(), std::string("no candidate should be empty for ") + query);
                if (item.source == CandidateSource::Generated)
                    ++lattice_rows;
                else if (item.source == CandidateSource::NeuralDesktop)
                    ++desktop_rows;
                else if (item.source == CandidateSource::NeuralKeyboard)
                    ++keyboard_rows;
            }
            // Trigram 显示开关没开，内部生成的路径只能交给神经模型，不能先冒充可见候选。
            expect(lattice_rows == 0, std::string("neural-only mode exposed a lattice row for ") + query);
            expect(desktop_rows <= 1, std::string("reranking must not add duplicate desktop rows for ") + query);
            expect(keyboard_rows <= 1, std::string("reranking must not add duplicate keyboard rows for ") + query);
        }
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::printf("Neural sentence reranking test passed.\n");
    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
