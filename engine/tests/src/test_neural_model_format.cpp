// SentenceModel 的格式与数值对拍，不依赖发布的模型文件：在内存里拼一个极小的 safetensors 模型
// （1 层、2 头、宽 4、词表 6），权重按公式生成，对拍 next_log_probabilities 与 numpy float64 参考实现
// 的结果。
//
// 主要守的是 config 里的两个开关。`gelu_tanh`：rerank-v2 的 6x256 是用 tanh 形式的 GELU 训练的，8x448 和
// model-v1 是精确形式。两者在这个模型上只差 1e-4 到 1e-3 量级，用错一种不会报错也不会出乱码，只会
// 让分数悄悄偏一点，所以必须逐项对拍、容差压在差值之下。`self_normalized`：按原始 logit 打分，
// 不做整词表 softmax。
//
// 期望值的生成脚本与这里的权重公式一一对应：tok/pos/各层矩阵与 bias 为
// amp * sin(0.37 * (j + 1) + 1.3 * k)，LayerNorm 增益（ln1/ln2）为 1 + 该值，k 是下面 kTensors 的序号，
// j 是张量内的行主序下标。改公式或幅度就得重新生成期望值。

#include "neural/sentence_model.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

struct TensorSpec
{
    const char *name;
    std::vector<std::size_t> shape;
    bool gain;
    double amplitude;
};

constexpr std::size_t kVocab = 6;
constexpr std::size_t kEmbd = 4;
constexpr std::size_t kContext = 8;

const std::vector<TensorSpec> kTensors = {
    {"tok.weight", {kVocab, kEmbd}, false, 1.0},
    {"pos.weight", {kContext, kEmbd}, false, 0.3},
    {"blocks.0.ln1.weight", {kEmbd}, true, 0.1},
    {"blocks.0.ln1.bias", {kEmbd}, false, 0.1},
    {"blocks.0.qkv.weight", {3 * kEmbd, kEmbd}, false, 0.8},
    {"blocks.0.qkv.bias", {3 * kEmbd}, false, 0.1},
    {"blocks.0.proj.weight", {kEmbd, kEmbd}, false, 0.8},
    {"blocks.0.proj.bias", {kEmbd}, false, 0.1},
    {"blocks.0.ln2.weight", {kEmbd}, true, 0.1},
    {"blocks.0.ln2.bias", {kEmbd}, false, 0.1},
    {"blocks.0.fc.weight", {4 * kEmbd, kEmbd}, false, 2.0},
    {"blocks.0.fc.bias", {4 * kEmbd}, false, 0.5},
    {"blocks.0.out.weight", {kEmbd, 4 * kEmbd}, false, 3.0},
    {"blocks.0.out.bias", {kEmbd}, false, 0.1},
    {"ln_f.weight", {kEmbd}, false, 6.0},
    {"ln_f.bias", {kEmbd}, false, 0.1},
};

// numpy 参考实现在上下文「ab」（即 <bos> a b）之后下一个字的 log 概率，按 token id 排列。
constexpr double kExpectedErf[kVocab] = {-0.641267, -3.100171, -13.634696, -13.086156, -2.452157, -1.072293};
constexpr double kExpectedTanh[kVocab] = {-0.641098, -3.100623, -13.635675, -13.086611, -2.451989, -1.072537};

std::string escape(const std::string &text)
{
    std::string out;
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

// extra 是追加进 config 的字段（以逗号开头）；为空时就是 format 版本 1 的文件，两个开关都不写。
std::vector<std::uint8_t> build_model(const std::string &extra)
{
    const std::string config =
        "{\"vocab\":6,\"n_layer\":1,\"n_head\":2,\"n_embd\":4,\"context\":8,\"dropout\":0.0" + extra + "}";
    const std::string vocab = "[\"<pad>\",\"<unk>\",\"<end>\",\"a\",\"b\",\"c\"]";

    std::string header = "{\"__metadata__\":{\"format\":\"chinese-ime-lm\",\"version\":\"1\",\"config\":\"" +
                         escape(config) + "\",\"vocab\":\"" + escape(vocab) + "\"}";
    std::vector<float> data;
    for (std::size_t k = 0; k < kTensors.size(); ++k)
    {
        const TensorSpec &tensor = kTensors[k];
        std::size_t count = 1;
        std::string shape;
        for (std::size_t dim : tensor.shape)
        {
            count *= dim;
            shape += (shape.empty() ? "" : ",") + std::to_string(dim);
        }
        const std::size_t begin = data.size() * sizeof(float);
        for (std::size_t j = 0; j < count; ++j)
        {
            const double value =
                tensor.amplitude * std::sin(0.37 * static_cast<double>(j + 1) + 1.3 * static_cast<double>(k));
            data.push_back(static_cast<float>(tensor.gain ? 1.0 + value : value));
        }
        const std::size_t end = data.size() * sizeof(float);
        header += ",\"" + std::string(tensor.name) + "\":{\"dtype\":\"F32\",\"shape\":[" + shape +
                  "],\"data_offsets\":[" + std::to_string(begin) + "," + std::to_string(end) + "]}";
    }
    header += "}";

    std::vector<std::uint8_t> bytes(8);
    std::uint64_t length = header.size();
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t>(length >> (8 * i));
    bytes.insert(bytes.end(), header.begin(), header.end());
    const std::size_t offset = bytes.size();
    bytes.resize(offset + data.size() * sizeof(float));
    std::memcpy(bytes.data() + offset, data.data(), data.size() * sizeof(float));
    return bytes;
}

// self_normalized 的模型按原始 logit 打分（6x256 就是这样训练和评测的），同一组 tanh 权重下的原始 logit。
constexpr double kExpectedTanhRaw[kVocab] = {6.899905, 4.440380, -6.094672, -5.545608, 5.089014, 6.468466};

// 两种 GELU 在这里差 1.7e-4 到 9.8e-4，容差得压在下面，否则用错一种也能过。
constexpr double kTolerance = 5e-5;

void check_distribution(const std::string &extra, const double (&expected)[kVocab], const char *label)
{
    const auto model = neural::SentenceModel::load(build_model(extra));
    expect(model != nullptr, std::string(label) + ": model should load");
    if (model == nullptr)
        return;
    const std::vector<float> distribution = model->next_log_probabilities("ab");
    expect(distribution.size() == kVocab, std::string(label) + ": one log-probability per token");
    if (distribution.size() != kVocab)
        return;
    for (std::size_t id = 0; id < kVocab; ++id)
    {
        const double got = distribution[id];
        if (std::fabs(got - expected[id]) > kTolerance)
        {
            std::fprintf(stderr, "FAIL: %s token %zu: got %.6f, want %.6f\n", label, id, got, expected[id]);
            ++failures;
        }
    }
    // 批量打分走的是另一条输出路径（self_normalized 时只算目标那一行），单字候选「c」的分数必须
    // 等于上面分布里 c 那一项。
    const std::vector<double> scores = model->score_sentences("ab", {"c"});
    if (scores.size() != 1 || std::fabs(scores[0] - expected[5]) > kTolerance)
    {
        std::fprintf(stderr, "FAIL: %s score_sentences(ab, c): got %.6f, want %.6f\n", label,
                     scores.empty() ? 0.0 : scores[0], expected[5]);
        ++failures;
    }
}
} // namespace

int main()
{
    // 两个开关都不写的文件（model-v1）：精确 GELU、softmax。
    check_distribution("", kExpectedErf, "format version 1");
    check_distribution(",\"gelu_tanh\":false,\"self_normalized\":false", kExpectedErf, "both flags false");
    check_distribution(",\"gelu_tanh\":true", kExpectedTanh, "gelu_tanh");
    // rerank 6x256 的组合。
    check_distribution(",\"gelu_tanh\":true,\"self_normalized\":true", kExpectedTanhRaw, "gelu_tanh + self_normalized");
    // 认不出的取值不能悄悄按默认算：那正是分数变样却不报错的情形。
    expect(neural::SentenceModel::load(build_model(",\"gelu_tanh\":1")) == nullptr,
           "a non-boolean gelu_tanh must be rejected");
    expect(neural::SentenceModel::load(build_model(",\"gelu_tanh\":\"tanh\"")) == nullptr,
           "a string gelu_tanh must be rejected");
    expect(neural::SentenceModel::load(build_model(",\"self_normalized\":1")) == nullptr,
           "a non-boolean self_normalized must be rejected");

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("neural model format: OK\n");
    return 0;
}
