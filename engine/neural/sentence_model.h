#pragma once

// Character-level Transformer "sentence model" from the chinese-ime-lm project, ported to C++.
//
// This is a faithful port of the Apache-2.0 Rust reference implementation
// (chinese-ime-lm/reference/src/{weights.rs,lib.rs}) and the format spec (docs/format.md). The
// model is a decoder-only Pre-LN Transformer with tied input/output embeddings; given some
// committed context it produces a log-probability distribution over the next character.
//
// The primitive the IME actually uses is score_sentences: log P(sentence | context) for a handful
// of already-formed sentences at once, which neural_decoder.* turns into a reranking of the word
// lattice's n-best paths. next_log_probabilities remains as the single-step reference API that the
// parity tests check against the Rust implementation.
//
// Two things make the scorer fast enough to run on the typing path, and both matter far more than
// the arithmetic itself. The weights are ~25 MB, which does not fit in cache, so a forward pass is
// memory-bandwidth bound: every matmul must stream the whole matrix. Therefore (1) the context's
// per-layer K/V is cached (PrefixCache) so an unchanged context is never recomputed, and (2) all
// candidates are run as one batch through each layer, so each weight matrix is streamed once for
// the whole batch instead of once per candidate per token.
//
// Numerical details that MUST match the reference or results are garbage: the GELU the model was
// trained with (exact erf-based unless its config sets `gelu_tanh`), LayerNorm eps 1e-5, attention
// scale 1/sqrt(n_embd/n_head), int8 per-output-row symmetric dequant (scale applied once after the
// dot product), and the rule that position i predicts token i+1.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neural
{

// A weight matrix kept in whatever precision the file stored it in. Quantized matrices are not
// expanded on load: the per-output-row scale is a constant factor for the whole row, so it lifts
// out of the dot product and multiplies the result once.
struct Matrix
{
    bool quantized = false;
    std::vector<float> floats;     // used when !quantized
    std::vector<std::int8_t> ints; // used when quantized
    std::vector<float> scales;     // per output row, used when quantized

    std::size_t len() const
    {
        return quantized ? ints.size() : floats.size();
    }

    // dot(x, row) * scale[row], where the scale is 1 for an unquantized matrix.
    float dot_row(const float *x, std::size_t row, std::size_t width) const;

    // One row as f32, for the token table read as an embedding as well as an output projection.
    void row_into(std::size_t index, std::size_t width, float *out) const;
};

struct Config
{
    std::size_t vocab = 0;
    std::size_t n_layer = 0;
    std::size_t n_head = 0;
    std::size_t n_embd = 0;
    std::size_t context = 0;
    // GELU's tanh form instead of the exact one (config key `gelu_tanh`, default false).
    bool gelu_tanh = false;
    // Trained so that a character's raw logit is its log-probability (config key `self_normalized`,
    // default false). Such a model is scored the way it was trained and evaluated: by the bare dot
    // product with the target's row, without the softmax over the vocabulary.
    bool self_normalized = false;
};

struct Block
{
    std::vector<float> ln1_weight;
    std::vector<float> ln1_bias;
    Matrix qkv_weight;
    std::vector<float> qkv_bias;
    Matrix proj_weight;
    std::vector<float> proj_bias;
    std::vector<float> ln2_weight;
    std::vector<float> ln2_bias;
    Matrix fc_weight;
    std::vector<float> fc_bias;
    Matrix out_weight;
    std::vector<float> out_bias;
};

// Per-layer keys and values for a fixed run of leading tokens, each [length, n_embd] row-major.
// Every batch row of a later segment attends to the same cache, so a context shared by all the
// candidates is paid for once.
struct PrefixCache
{
    std::vector<std::vector<float>> keys;   // one entry per layer
    std::vector<std::vector<float>> values; // one entry per layer
    std::size_t length = 0;                 // tokens covered, i.e. the position the next token takes
};

class SentenceModel
{
  public:
    // Loads a model from an in-memory safetensors file. Returns nullptr on any malformed input
    // rather than throwing, so a missing or corrupt model leaves the IME working without it.
    static std::unique_ptr<SentenceModel> load(const std::vector<std::uint8_t> &bytes);

    // Loads a model from disk. Returns nullptr when the file is absent or unreadable/invalid.
    static std::unique_ptr<SentenceModel> load_file(const std::string &path);

    std::size_t context_length() const
    {
        return config_.context;
    }
    const std::string &attribution() const
    {
        return attribution_;
    }
    const std::string &license() const
    {
        return license_;
    }

    // The token id this model uses for a character (a Unicode code point), if any.
    std::optional<std::uint32_t> token(char32_t character) const;

    // log P(next character | text), over the whole vocabulary. Index by token id. For a
    // self-normalized model these are the raw logits, the same reading score_sentences uses.
    std::vector<float> next_log_probabilities(const std::string &text) const;

    // Sum over characters of log P(character | context and everything before it in the text), one
    // entry per text. Characters outside the vocabulary score as <unk>, so a text is never dropped.
    //
    // The whole batch is one pass: `context` (minus its last token) becomes a PrefixCache reused
    // across calls while the context is unchanged, and the texts are padded to equal length and run
    // together. An empty `texts` returns empty. A text longer than the model's context window is
    // truncated from the left, as is the context when the two together do not fit.
    //
    // Thread-safe: the prefix cache is guarded, and concurrent callers with different contexts are
    // correct but will evict each other.
    std::vector<double> score_sentences(const std::string &context, const std::vector<std::string> &texts) const;

  private:
    struct Workspace;

    static constexpr std::uint32_t kUnk = 1;
    static constexpr std::uint32_t kBos = 2;

    Config config_;
    std::unordered_map<char32_t, std::uint32_t> index_;
    Matrix token_;
    std::vector<float> position_;
    std::vector<Block> blocks_;
    std::vector<float> final_weight_;
    std::vector<float> final_bias_;
    std::string attribution_;
    std::string license_;

    // Guards the cached prefix below. mutable so scoring stays const.
    mutable std::mutex cache_mutex_;
    mutable std::vector<std::uint32_t> cached_tokens_;
    mutable PrefixCache cached_prefix_;

    std::vector<std::uint32_t> encode(const std::string &text) const;
    std::vector<float> embed(const std::vector<std::uint32_t> &tokens, std::size_t width, std::size_t offset) const;

    // Runs `batch` rows of `width` tokens (row-major in `tokens`, padded to equal length by the
    // caller) at positions [past_len, past_len + width), where every row additionally attends to
    // `past`. Returns the final-LayerNorm'd hidden states, [batch * width, n_embd].
    //
    // When `record` is non-null the K/V produced here is stored into it, which requires batch == 1
    // (a cache is shared by later rows, so it may only ever describe one sequence).
    std::vector<float> forward(const std::vector<std::uint32_t> &tokens, std::size_t batch, std::size_t width,
                               const PrefixCache *past, PrefixCache *record) const;

    // Builds the K/V cache for `tokens`, which are taken to start at position 0.
    PrefixCache build_prefix(const std::vector<std::uint32_t> &tokens) const;

    // log-softmax over the vocabulary for one hidden state.
    std::vector<float> distribution(const std::vector<float> &hidden) const;

    // For each of `rows` hidden states, log P(targets[row]) under that state's distribution. Rows
    // with target kNoTarget are skipped and report 0.
    static constexpr std::uint32_t kNoTarget = 0xFFFFFFFFu;
    std::vector<float> target_log_probs(const std::vector<float> &hidden, std::size_t rows,
                                        const std::vector<std::uint32_t> &targets) const;

    void attention(const std::vector<float> &q, const std::vector<float> &k, const std::vector<float> &v,
                   std::size_t batch, std::size_t width, const std::vector<float> *past_k,
                   const std::vector<float> *past_v, std::size_t past_len, std::vector<float> &out,
                   std::vector<float> &scores) const;
    void feed_forward(std::vector<float> &x, const Block &block, Workspace &workspace) const;
};

} // namespace neural
