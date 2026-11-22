#include "v_mode_query.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace metasequoia::local_modes
{
namespace
{
constexpr const char *kReadingDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
constexpr const char *kFinancialDigits[] = {"零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"};
constexpr const char *kReadingUnits[] = {"", "十", "百", "千"};
constexpr const char *kFinancialUnits[] = {"", "拾", "佰", "仟"};
constexpr const char *kGroupUnits[] = {"", "万", "亿", "万亿"};
// 万亿以上没有通用的读法，读法和大写都只到 16 位。
constexpr std::size_t kMaxIntegerDigits = 16;
// 括号嵌套的上限，防止病态输入把递归下降打爆栈。
constexpr int kMaxExpressionDepth = 64;

bool all_digits(const std::string &text)
{
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

std::string strip_leading_zeros(const std::string &digits)
{
    const std::size_t first = digits.find_first_not_of('0');
    return first == std::string::npos ? std::string{} : digits.substr(first);
}

// 四位以内的一节，节内的零按「中间有零读一个零，末尾的零不读」处理。
std::string group_text(unsigned group, const char *const *digits, const char *const *units)
{
    static constexpr unsigned kPowers[] = {1000, 100, 10, 1};
    std::string text;
    bool pending_zero = false;
    for (int position = 0; position < 4; ++position)
    {
        const unsigned digit = group / kPowers[position] % 10;
        if (digit == 0)
        {
            pending_zero = !text.empty();
            continue;
        }
        if (pending_zero)
        {
            text += digits[0];
            pending_zero = false;
        }
        text += digits[digit];
        text += units[3 - position];
    }
    return text;
}

// 按四位一节从高到低拼，节与节之间：前面有数、这一节不满千或中间隔了全零的节，就补一个零。
std::string integer_text(const std::string &digits_text, bool financial)
{
    const char *const *digits = financial ? kFinancialDigits : kReadingDigits;
    const char *const *units = financial ? kFinancialUnits : kReadingUnits;
    const std::string digits_only = strip_leading_zeros(digits_text);
    if (digits_only.empty())
        return digits[0];
    if (digits_only.size() > kMaxIntegerDigits)
        return {};

    const std::size_t group_count = (digits_only.size() + 3) / 4;
    const std::size_t head = digits_only.size() - (group_count - 1) * 4;
    std::string text;
    bool need_zero = false;
    for (std::size_t index = 0; index < group_count; ++index)
    {
        const std::size_t start = index == 0 ? 0 : head + (index - 1) * 4;
        const std::size_t length = index == 0 ? head : 4;
        const unsigned group = static_cast<unsigned>(std::stoul(digits_only.substr(start, length)));
        if (group == 0)
        {
            need_zero = true;
            continue;
        }
        if (!text.empty() && (need_zero || group < 1000))
            text += digits[0];
        need_zero = false;
        text += group_text(group, digits, units);
        text += kGroupUnits[group_count - 1 - index];
    }
    // 读法里打头的「一十」说成「十」：十五、十万；大写照写「壹拾」。
    if (!financial && text.rfind("一十", 0) == 0)
        text.erase(0, std::strlen("一"));
    return text;
}

std::string spelled_digits(const std::string &digits, const char *const *table)
{
    std::string text;
    for (const char digit : digits)
        text += table[digit - '0'];
    return text;
}

std::string with_thousands_separators(const std::string &digits)
{
    std::string text;
    for (std::size_t index = 0; index < digits.size(); ++index)
    {
        if (index > 0 && (digits.size() - index) % 3 == 0)
            text += ',';
        text += digits[index];
    }
    return text;
}

// 纯数字：整数，或「整数.小数」，各至少一位。
bool split_number(const std::string &input, std::string &integer, std::string &fraction)
{
    const std::size_t dot = input.find('.');
    integer = input.substr(0, dot);
    fraction = dot == std::string::npos ? std::string{} : input.substr(dot + 1);
    return all_digits(integer) && (dot == std::string::npos || all_digits(fraction));
}

// 罗马数字：只到 3999，零和更大的数没有通用写法，返回空串。
std::string roman_numeral(const std::string &digits)
{
    const std::string trimmed = strip_leading_zeros(digits);
    if (trimmed.empty() || trimmed.size() > 4)
        return {};
    unsigned value = static_cast<unsigned>(std::stoul(trimmed));
    if (value == 0 || value > 3999)
        return {};
    static constexpr struct
    {
        unsigned value;
        const char *symbol;
    } kTable[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
                  {40, "XL"},  {10, "X"},   {9, "IX"},  {5, "V"},    {4, "IV"},  {1, "I"}};
    std::string text;
    for (const auto &entry : kTable)
    {
        for (; value >= entry.value; value -= entry.value)
            text += entry.symbol;
    }
    return text;
}

// 金额：恰好两位小数时按分读；超过两位（算式结果常常是 3.333333）先四舍五入到分。
std::string money_for_parts(const std::string &integer, const std::string &fraction)
{
    if (fraction.size() <= 2)
        return chinese_money(integer, fraction);
    const double value = std::strtod((integer + "." + fraction).c_str(), nullptr);
    if (!std::isfinite(value) || std::fabs(value) >= 1e13)
        return {};
    const long long cents = std::llround(value * 100.0);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02lld", cents % 100);
    return chinese_money(std::to_string(cents / 100), std::string(buffer, 2));
}

// 候选顺序固定（issue #639）：大写读法、小写读法、货币、罗马数字、千分位。不调频，也不随使用次数
// 改序——这批候选不进词库，顺序只由输入决定。
std::vector<std::string> number_candidates(const std::string &integer, const std::string &fraction)
{
    std::vector<std::string> texts;
    const std::string financial = integer_text(integer, true);
    const std::string reading = integer_text(integer, false);
    const std::string digits_only = strip_leading_zeros(integer);
    if (fraction.empty())
    {
        texts.push_back(financial);
        texts.push_back(reading);
        texts.push_back(chinese_money(integer, {}));
        texts.push_back(roman_numeral(digits_only));
        if (digits_only.size() >= 4)
            texts.push_back(with_thousands_separators(digits_only));
    }
    else
    {
        // 带小数时多半是金额，金额排第一；两种读法按位读小数。
        texts.push_back(money_for_parts(integer, fraction));
        if (!financial.empty())
            texts.push_back(financial + "点" + spelled_digits(fraction, kFinancialDigits));
        if (!reading.empty())
            texts.push_back(reading + "点" + spelled_digits(fraction, kReadingDigits));
    }
    return texts;
}

// 算式候选顺序固定：阿拉伯结果、算式=结果、大写读法、小写读法、货币。负结果读作「负…」，
// 不出金额。
std::vector<std::string> expression_candidates(const std::string &input, const std::string &result)
{
    std::vector<std::string> texts{result, input + "=" + result};
    const bool negative = !result.empty() && result.front() == '-';
    std::string integer;
    std::string fraction;
    if (!split_number(negative ? result.substr(1) : result, integer, fraction))
        return texts;
    const std::string sign = negative ? std::string("负") : std::string{};
    const std::string financial = integer_text(integer, true);
    const std::string reading = integer_text(integer, false);
    const std::string fraction_suffix_financial =
        fraction.empty() ? std::string{} : "点" + spelled_digits(fraction, kFinancialDigits);
    const std::string fraction_suffix_reading =
        fraction.empty() ? std::string{} : "点" + spelled_digits(fraction, kReadingDigits);
    if (!financial.empty())
        texts.push_back(sign + financial + fraction_suffix_financial);
    if (!reading.empty())
        texts.push_back(sign + reading + fraction_suffix_reading);
    if (!negative)
        texts.push_back(money_for_parts(integer, fraction));
    return texts;
}

// + - 一层、* / 一层、乘方 ** 右结合、一元正负号和括号，按通常的优先级求值。
class ExpressionParser
{
  public:
    explicit ExpressionParser(const std::string &text) : text_(text)
    {
    }

    bool parse(double &value)
    {
        return expression(value, 0) && position_ == text_.size();
    }

  private:
    bool expression(double &value, int depth)
    {
        if (!term(value, depth))
            return false;
        while (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
        {
            const char op = text_[position_++];
            double right = 0;
            if (!term(right, depth))
                return false;
            value = op == '+' ? value + right : value - right;
        }
        return true;
    }

    bool term(double &value, int depth)
    {
        if (!unary(value, depth))
            return false;
        while (position_ < text_.size() && (text_[position_] == '*' || text_[position_] == '/'))
        {
            const char op = text_[position_++];
            double right = 0;
            if (!unary(right, depth))
                return false;
            if (op == '/' && right == 0)
                return false;
            value = op == '*' ? value * right : value / right;
        }
        return true;
    }

    bool unary(double &value, int depth)
    {
        if (depth > kMaxExpressionDepth || position_ >= text_.size())
            return false;
        const char ch = text_[position_];
        if (ch == '+' || ch == '-')
        {
            ++position_;
            if (!unary(value, depth + 1))
                return false;
            value = ch == '-' ? -value : value;
            return true;
        }
        return power(value, depth);
    }

    // ** 右结合：2**3**2 = 2**(3**2)。溢出留空。
    bool power(double &value, int depth)
    {
        if (!primary(value, depth))
            return false;
        if (position_ + 1 < text_.size() && text_[position_] == '*' && text_[position_ + 1] == '*')
        {
            position_ += 2;
            double exponent = 0;
            if (!unary(exponent, depth + 1))
                return false;
            value = std::pow(value, exponent);
            return std::isfinite(value);
        }
        return true;
    }

    bool primary(double &value, int depth)
    {
        if (depth > kMaxExpressionDepth || position_ >= text_.size())
            return false;
        if (text_[position_] == '(')
        {
            ++position_;
            if (!expression(value, depth + 1) || position_ >= text_.size() || text_[position_] != ')')
                return false;
            ++position_;
            return true;
        }
        return number(value);
    }

    // 数字至少一位，小数点两边都要有数字：1.、.5、1.2.3 都不认。
    bool number(double &value)
    {
        const std::size_t start = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            ++position_;
        if (position_ == start)
            return false;
        if (position_ < text_.size() && text_[position_] == '.')
        {
            const std::size_t fraction_start = ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                ++position_;
            if (position_ == fraction_start)
                return false;
        }
        value = std::strtod(text_.substr(start, position_ - start).c_str(), nullptr);
        return true;
    }

    const std::string &text_;
    std::size_t position_ = 0;
};

// 至多十位小数，去掉末尾的零：0.1+0.2 给 0.3，1/3 给 0.3333333333。太大的数用有效数字表示。
std::string format_result(double value)
{
    if (!std::isfinite(value))
        return {};
    char buffer[64] = {};
    if (std::fabs(value) >= 1e15)
    {
        std::snprintf(buffer, sizeof(buffer), "%.15g", value);
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.10f", value);
    std::string text = buffer;
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

bool has_operator(const std::string &input)
{
    return input.find_first_of("+-*/()") != std::string::npos;
}
} // namespace

std::string chinese_number_reading(const std::string &digits)
{
    return all_digits(digits) ? integer_text(digits, false) : std::string{};
}

std::string chinese_financial_number(const std::string &digits)
{
    return all_digits(digits) ? integer_text(digits, true) : std::string{};
}

std::string chinese_money(const std::string &integer_digits, const std::string &fraction_digits)
{
    if (!all_digits(integer_digits) || fraction_digits.size() > 2 ||
        (!fraction_digits.empty() && !all_digits(fraction_digits)))
        return {};
    const std::string integer = integer_text(integer_digits, true);
    if (integer.empty())
        return {};
    const bool integer_zero = strip_leading_zeros(integer_digits).empty();
    const unsigned jiao = fraction_digits.size() >= 1 ? static_cast<unsigned>(fraction_digits[0] - '0') : 0;
    const unsigned fen = fraction_digits.size() >= 2 ? static_cast<unsigned>(fraction_digits[1] - '0') : 0;

    std::string text = integer_zero ? std::string{} : integer + "元";
    if (jiao == 0 && fen == 0)
        return integer_zero ? std::string(kFinancialDigits[0]) + "元整" : text + "整";
    if (jiao > 0)
        text += std::string(kFinancialDigits[jiao]) + "角";
    else if (!integer_zero)
        text += kFinancialDigits[0]; // 壹佰元零伍分
    if (fen > 0)
        text += std::string(kFinancialDigits[fen]) + "分";
    return text;
}

bool evaluate_v_mode_expression(const std::string &expression, std::string &result)
{
    double value = 0;
    if (expression.empty() || !ExpressionParser(expression).parse(value))
        return false;
    result = format_result(value);
    return !result.empty();
}

std::vector<WordItem> query_v_mode(const std::string &input, int limit)
{
    if (limit <= 0 || input.empty())
        return {};

    std::vector<std::string> texts;
    std::string integer;
    std::string fraction;
    if (!has_operator(input) && split_number(input, integer, fraction))
    {
        texts = number_candidates(integer, fraction);
    }
    else if (has_operator(input))
    {
        std::string result;
        if (evaluate_v_mode_expression(input, result))
            texts = expression_candidates(input, result);
    }

    texts.erase(std::remove(texts.begin(), texts.end(), std::string{}), texts.end());
    std::vector<WordItem> results;
    for (std::string &text : texts)
    {
        if (std::find_if(results.begin(), results.end(), [&](const WordItem &item) { return item.word == text; }) !=
            results.end())
            continue;
        if (static_cast<int>(results.size()) >= limit)
            break;
        results.emplace_back("", std::move(text), 0, CandidateSource::Generated);
    }
    for (std::size_t index = 0; index < results.size(); ++index)
        results[index].weight = static_cast<std::int64_t>(results.size() - index);
    return results;
}

std::string v_mode_money_text(const std::string &input)
{
    if (input.empty())
        return {};
    std::string integer;
    std::string fraction;
    if (!has_operator(input))
    {
        if (!split_number(input, integer, fraction))
            return {};
        return money_for_parts(integer, fraction);
    }
    std::string result;
    if (!evaluate_v_mode_expression(input, result))
        return {};
    const bool negative = !result.empty() && result.front() == '-';
    if (negative || !split_number(result, integer, fraction))
        return {};
    return money_for_parts(integer, fraction);
}
} // namespace metasequoia::local_modes
