#include "lattice_rerank.h"

#include "../neural/rescore_worker.h"
#include "quanpin_utils.h"
#include "quanpin_query.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace quanpin
{

LatticeReranker make_neural_reranker(const neural::SentenceModel *model, const std::string &context,
                                     const neural::RerankOptions &options)
{
    if (model == nullptr)
    {
        return {};
    }
    // 在这里就把前文裁到模型真正会看的那几十个字：后台重排的结果表以「前文 + 候选句」为键，
    // 拿没裁过的整段历史当键的话，每上屏一个字键就变了，缓存永远打不中。
    const std::string trimmed = neural::last_characters(context, options.context_chars);
    return [model, context = trimmed, options](std::vector<LatticePath> &paths) -> bool {
        if (paths.size() < 2)
        {
            return false;
        }
        std::vector<std::string> sentences;
        std::vector<double> scores;
        sentences.reserve(paths.size());
        scores.reserve(paths.size());
        for (const LatticePath &path : paths)
        {
            sentences.push_back(path.sentence);
            scores.push_back(path.log_prob);
        }

        // 不在这里打分：交给后台线程，第一次必然落空，按词格的静态顺序先出候选。线程算完会叫
        // 醒会话重查一次，那一次才在结果表里拿到顺序。
        const std::optional<std::vector<std::size_t>> order =
            neural::RescoreWorker::instance().order_for(*model, context, sentences, scores, options);
        if (!order || order->size() != paths.size())
        {
            return false; // 还没算出来，或者模型自己弃权了：词格的顺序原样留着
        }

        std::vector<LatticePath> reordered;
        reordered.reserve(paths.size());
        for (std::size_t index : *order)
        {
            reordered.push_back(paths[index]);
        }
        paths = std::move(reordered);
        return true;
    };
}

void rerank_dictionary_candidates(std::vector<WordItem> &candidates, const std::string &key,
                                  const neural::SentenceModel *model, const std::string &context)
{
    if (context.empty() || model == nullptr || key.empty() || !has_only_complete_pinyin_segments(split_segments(key)))
        return;

    neural::RerankOptions options;
    options.max_paths = 32;
    // Dictionary frequency is already the initial order; context needs room to overturn common homophones.
    options.lambda = 0.8;
    std::vector<std::size_t> slots;
    std::vector<std::string> texts;
    std::vector<double> priors;
    for (std::size_t i = 0; i < candidates.size() && slots.size() < options.max_paths; ++i)
    {
        const WordItem &item = candidates[i];
        if ((item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase) ||
            item.canonical_pinyin != key || item.fuzzy || !item.corrected_from.empty() || item.word.empty())
            continue;
        slots.push_back(i);
        texts.push_back(item.word);
        // Word frequencies are counts, not lattice log probabilities. Only their ratios matter.
        priors.push_back(std::log10(static_cast<double>(std::max<std::int64_t>(item.weight, 1))));
    }
    if (slots.size() < 2)
        return;
    const auto order = neural::RescoreWorker::instance().order_for(
        *model, neural::last_characters(context, options.context_chars), texts, priors, options);
    if (!order || order->size() != slots.size())
        return;
    std::vector<WordItem> ranked;
    ranked.reserve(slots.size());
    for (std::size_t index : *order)
        ranked.push_back(std::move(candidates[slots[index]]));
    for (std::size_t i = 0; i < slots.size(); ++i)
        candidates[slots[i]] = std::move(ranked[i]);
}

} // namespace quanpin
