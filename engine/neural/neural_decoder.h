#pragma once

// Neural reranking of whole-sentence candidates.
//
// The model does not produce sentences. The word lattice (engine/quanpin/word_lattice.*) already
// decodes the typed pinyin into its n-best sentences with a kenlm trigram; this layer asks the
// character-level Transformer how likely each of those sentences is given what the user has already
// committed, and reorders them by
//
//     score = static + lambda * (neural - static)
//
// both terms in nats. That is a log-linear interpolation of the two language models, with lambda
// controlling how much the neural model is allowed to override the lattice. It cannot invent a
// sentence the lattice did not propose, which is the point: the lattice is good at coverage and bad
// at long-range choices, and the Transformer is the reverse.
//
// This replaces an earlier design that ran a per-syllable beam search over the model to generate
// sentences directly. That needed one full forward pass per beam per syllable with no K/V reuse --
// on the order of 10^10 multiply-accumulates for a dozen syllables, seconds of wall clock on the
// typing path. Scoring a small batch of finished sentences against a cached context is far less work, because
// the sentences come from the lattice for free and the batch streams the weights once.
//
// The word lattice always supplies the paths when neural reranking is enabled. Its own candidate
// does not have to be enabled for display: in that mode the lattice is only an internal candidate
// generator and the user sees the neural model's pick.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace neural
{

class SentenceModel;

struct RerankOptions
{
    // How much of the model's opinion to apply. 0 leaves the lattice order untouched; 1 replaces
    // the lattice score outright. 0.5 keeps the lattice's own evidence (user dictionary weights,
    // reading priors) in play, which matters because the neural model has never seen them.
    double lambda = 0.5;
    // Paths to rescore. The lattice's own ordering past the first handful is already noise, and
    // every extra path is another row in the batch.
    std::size_t max_paths = 12;
    // Characters of committed text shown to the model as context. Bounded by the model's own
    // window; 64 matches what the model was trained to use.
    std::size_t context_chars = 64;
};

// Last `count` Unicode characters of `text`, not bytes: the context bound is in characters because
// the model's window is in characters. Exposed so a caller that caches results keyed by context can
// trim to exactly what will be scored -- two histories with the same tail are the same context, and
// keying on the untrimmed text would miss the cache on every commit.
std::string last_characters(const std::string &text, std::size_t count);

// Loads (once, process-wide) and returns the model at `path`, or nullptr if it is missing or
// invalid. The returned pointer is owned by the cache and lives for the process; a failed load is
// remembered so it is not retried on every keystroke.
const SentenceModel *shared_sentence_model(const std::string &path);

// A handle that defers shared_sentence_model() until the model is first asked for. The two models
// together are ~34 MB of heap once loaded; resolving them in the dictionary constructors paid for
// both even when the settings leave them off (the desktop model is off by default). Not thread-safe:
// resolve it on the thread that owns the dictionary and hand the raw pointer to worker threads.
class LazySentenceModel
{
  public:
    LazySentenceModel() = default;
    explicit LazySentenceModel(std::string path) : path_(std::move(path))
    {
    }

    const SentenceModel *get() const
    {
        if (!resolved_)
        {
            model_ = path_.empty() ? nullptr : shared_sentence_model(path_);
            resolved_ = true;
        }
        return model_;
    }

  private:
    std::string path_;
    mutable const SentenceModel *model_ = nullptr;
    mutable bool resolved_ = false;
};

// The order `sentences` should be shown in, as indices into it.
//
// `static_scores` are the lattice's own path scores in log10, as LatticePath::log_prob reports
// them; they are converted to nats here. `context` is the text already committed before the
// composition, and is truncated to the last `options.context_chars` characters.
//
// Returns an empty vector when the model could not be consulted at all (fewer than two sentences,
// lambda of zero, mismatched inputs). The caller must then leave its own order alone: a partial
// reorder is worse than none, because it mixes two incomparable rankings.
//
// Only the first `options.max_paths` sentences are rescored; any beyond that keep their relative
// order at the end.
std::vector<std::size_t> rerank_order(const SentenceModel &model, const std::string &context,
                                      const std::vector<std::string> &sentences,
                                      const std::vector<double> &static_scores, const RerankOptions &options = {});

} // namespace neural
