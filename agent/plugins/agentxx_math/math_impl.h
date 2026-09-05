/// agentxx_math 插件 —— 数学表达式解析与计算引擎 (纯函数, 不含 C ABI 胶水)
/// - 头文件-only: 插件入口与测试
///   ([test_math_tools.cpp](/agent/test/core/test_math_tools.cpp))
///   共同包含, 保证插件行为与测试覆盖一致
/// - 依赖: neograph (json) + fmt + C++ 标准数学库
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <neograph/json.h>
#include <numbers>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx_math_plugin {

/// 角度单位枚举
enum class AngleUnit {
    Radians,
    Degrees
};

/// 表达式计算异常类
class MathEvalException : public std::runtime_error {
public:

    explicit MathEvalException(const std::string& msg, size_t pos = 0) :
        std::runtime_error(
            pos > 0 ? fmt::format("[Error] {} (at position {})", msg, pos)
                    : fmt::format("[Error] {}", msg)
        ),
        pos_(pos) {}

    size_t position() const noexcept {
        return pos_;
    }

private:

    size_t pos_ = 0;
};

namespace detail {

/// Token 类型
enum class TokenType {
    Number,
    Identifier,
    Plus,            // +
    Minus,           // -
    Star,            // *
    Slash,           // /
    DoubleSlash,     // //
    Percent,         // %
    Caret,           // ^
    StarStar,        // **
    Exclamation,     // !
    Tilde,           // ~
    Ampersand,       // &
    Pipe,            // |
    DoubleAmpersand, // &&
    DoublePipe,      // ||
    Shl,             // <<
    Shr,             // >>
    Eq,              // ==
    Ne,              // != or <>
    Lt,              // <
    Le,              // <=
    Gt,              // >
    Ge,              // >=
    Question,        // ?
    Colon,           // :
    LParen,          // (
    RParen,          // )
    LBracket,        // [
    RBracket,        // ]
    Comma,           // ,
    EndOfInput
};

/// Token 结构
struct Token {
    TokenType   type         = TokenType::EndOfInput;
    double      number_value = 0.0;
    std::string text;
    size_t      pos = 0;
};

/// 字符串转小写
inline std::string toLower(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

/// 阶乘计算
inline double calculateFactorial(double n, size_t pos) {
    if (n < 0.0) {
        throw MathEvalException("Factorial is not defined for negative numbers", pos);
    }
    if (std::abs(n - std::round(n)) > 1e-9) {
        // 非整数使用 Gamma 函数: n! = tgamma(n + 1)
        double res = std::tgamma(n + 1.0);
        if (std::isinf(res) || std::isnan(res)) {
            throw MathEvalException("Factorial overflow", pos);
        }
        return res;
    }
    auto int_n = static_cast<int64_t>(std::round(n));
    if (int_n > 170) {
        throw MathEvalException("Factorial overflow (maximum supported integer is 170!)", pos);
    }
    double result = 1.0;
    for (int64_t i = 2; i <= int_n; ++i) {
        result *= static_cast<double>(i);
    }
    return result;
}

/// 组合数 nCr
inline double calculateComb(int64_t n, int64_t r, size_t pos) {
    if (n < 0 || r < 0 || r > n) {
        return 0.0;
    }
    if (r == 0 || r == n) {
        return 1.0;
    }
    if (r > n / 2) {
        r = n - r;
    }
    double res = 1.0;
    for (int64_t i = 1; i <= r; ++i) {
        res = res * static_cast<double>(n - i + 1) / static_cast<double>(i);
    }
    if (std::isinf(res) || std::isnan(res)) {
        throw MathEvalException("Combination calculation overflow", pos);
    }
    return std::round(res);
}

/// 排列数 nPr
inline double calculatePerm(int64_t n, int64_t r, size_t pos) {
    if (n < 0 || r < 0 || r > n) {
        return 0.0;
    }
    if (r == 0) {
        return 1.0;
    }
    double res = 1.0;
    for (int64_t i = 0; i < r; ++i) {
        res *= static_cast<double>(n - i);
    }
    if (std::isinf(res) || std::isnan(res)) {
        throw MathEvalException("Permutation calculation overflow", pos);
    }
    return std::round(res);
}

/// 最大公约数 gcd
inline int64_t gcd(int64_t a, int64_t b) {
    a = std::abs(a);
    b = std::abs(b);
    while (b != 0) {
        int64_t t = b;
        b         = a % b;
        a         = t;
    }
    return a;
}

/// 最小公倍数 lcm
inline int64_t lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return (std::abs(a) / gcd(a, b)) * std::abs(b);
}

/// 词法分析器 (Lexer)
class Lexer {
public:

    explicit Lexer(std::string_view input) :
        input_(input),
        pos_(0) {}

    std::vector<Token> tokenize() {
        std::vector<Token> raw_tokens;

        while (pos_ < input_.size()) {
            char c = input_[pos_];

            // 跳过空白字符
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
                continue;
            }

            // 处理数字
            if (std::isdigit(static_cast<unsigned char>(c))
                || (c == '.' && pos_ + 1 < input_.size()
                    && std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])))) {
                raw_tokens.push_back(readNumber());
                continue;
            }

            // 处理标识符 (字母、下划线)
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                raw_tokens.push_back(readIdentifier());
                continue;
            }

            // 处理双字符或单字符运算符
            size_t cur_pos = pos_;
            if (c == '+') {
                raw_tokens.push_back(Token{TokenType::Plus, 0.0, "+", cur_pos});
                ++pos_;
            } else if (c == '-') {
                raw_tokens.push_back(Token{TokenType::Minus, 0.0, "-", cur_pos});
                ++pos_;
            } else if (c == '*') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '*') {
                    raw_tokens.push_back(Token{TokenType::StarStar, 0.0, "**", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Star, 0.0, "*", cur_pos});
                    ++pos_;
                }
            } else if (c == '/') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '/') {
                    raw_tokens.push_back(Token{TokenType::DoubleSlash, 0.0, "//", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Slash, 0.0, "/", cur_pos});
                    ++pos_;
                }
            } else if (c == '%') {
                raw_tokens.push_back(Token{TokenType::Percent, 0.0, "%", cur_pos});
                ++pos_;
            } else if (c == '^') {
                raw_tokens.push_back(Token{TokenType::Caret, 0.0, "^", cur_pos});
                ++pos_;
            } else if (c == '!') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    raw_tokens.push_back(Token{TokenType::Ne, 0.0, "!=", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Exclamation, 0.0, "!", cur_pos});
                    ++pos_;
                }
            } else if (c == '~') {
                raw_tokens.push_back(Token{TokenType::Tilde, 0.0, "~", cur_pos});
                ++pos_;
            } else if (c == '&') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '&') {
                    raw_tokens.push_back(Token{TokenType::DoubleAmpersand, 0.0, "&&", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Ampersand, 0.0, "&", cur_pos});
                    ++pos_;
                }
            } else if (c == '|') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '|') {
                    raw_tokens.push_back(Token{TokenType::DoublePipe, 0.0, "||", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Pipe, 0.0, "|", cur_pos});
                    ++pos_;
                }
            } else if (c == '<') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '<') {
                    raw_tokens.push_back(Token{TokenType::Shl, 0.0, "<<", cur_pos});
                    pos_ += 2;
                } else if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    raw_tokens.push_back(Token{TokenType::Le, 0.0, "<=", cur_pos});
                    pos_ += 2;
                } else if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>') {
                    raw_tokens.push_back(Token{TokenType::Ne, 0.0, "<>", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Lt, 0.0, "<", cur_pos});
                    ++pos_;
                }
            } else if (c == '>') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>') {
                    raw_tokens.push_back(Token{TokenType::Shr, 0.0, ">>", cur_pos});
                    pos_ += 2;
                } else if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    raw_tokens.push_back(Token{TokenType::Ge, 0.0, ">=", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Gt, 0.0, ">", cur_pos});
                    ++pos_;
                }
            } else if (c == '=') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    raw_tokens.push_back(Token{TokenType::Eq, 0.0, "==", cur_pos});
                    pos_ += 2;
                } else {
                    raw_tokens.push_back(Token{TokenType::Eq, 0.0, "==", cur_pos});
                    ++pos_;
                }
            } else if (c == '?') {
                raw_tokens.push_back(Token{TokenType::Question, 0.0, "?", cur_pos});
                ++pos_;
            } else if (c == ':') {
                raw_tokens.push_back(Token{TokenType::Colon, 0.0, ":", cur_pos});
                ++pos_;
            } else if (c == '(') {
                raw_tokens.push_back(Token{TokenType::LParen, 0.0, "(", cur_pos});
                ++pos_;
            } else if (c == ')') {
                raw_tokens.push_back(Token{TokenType::RParen, 0.0, ")", cur_pos});
                ++pos_;
            } else if (c == '[') {
                raw_tokens.push_back(Token{TokenType::LBracket, 0.0, "[", cur_pos});
                ++pos_;
            } else if (c == ']') {
                raw_tokens.push_back(Token{TokenType::RBracket, 0.0, "]", cur_pos});
                ++pos_;
            } else if (c == ',') {
                raw_tokens.push_back(Token{TokenType::Comma, 0.0, ",", cur_pos});
                ++pos_;
            } else {
                throw MathEvalException(fmt::format("Unexpected character '{}'", c), pos_ + 1);
            }
        }

        // 隐式乘法处理 (例如 2pi -> 2 * pi, 2(3+4) -> 2 * (3+4), (1+2)(3+4) -> (1+2)*(3+4))
        std::vector<Token> tokens;
        tokens.reserve(raw_tokens.size() * 2);

        for (size_t i = 0; i < raw_tokens.size(); ++i) {
            tokens.push_back(raw_tokens[i]);
            if (i + 1 < raw_tokens.size()) {
                const auto& cur  = raw_tokens[i];
                const auto& next = raw_tokens[i + 1];

                bool cur_is_factorial = false;
                if (cur.type == TokenType::Exclamation && i > 0) {
                    const auto& prev = raw_tokens[i - 1];
                    if (prev.type == TokenType::Number || prev.type == TokenType::RParen
                        || prev.type == TokenType::RBracket || prev.type == TokenType::Identifier) {
                        cur_is_factorial = true;
                    }
                }

                bool cur_is_num_or_bracket
                    = (cur.type == TokenType::Number || cur.type == TokenType::RParen
                       || cur.type == TokenType::RBracket || cur_is_factorial);

                bool cur_is_ident
                    = (cur.type == TokenType::Identifier && !isKeywordOperator(cur.text));

                bool next_is_bracket
                    = (next.type == TokenType::LParen || next.type == TokenType::LBracket);
                bool next_is_num_or_ident
                    = (next.type == TokenType::Number
                       || (next.type == TokenType::Identifier && !isKeywordOperator(next.text)));

                // 规则 1: 数字/右括号/阶乘 紧跟 括号/数字/标识符 (例如 2(3), (1+2)(3), 2pi,
                // 3sqrt(4))
                if (cur_is_num_or_bracket && (next_is_bracket || next_is_num_or_ident)) {
                    tokens.push_back(Token{TokenType::Star, 0.0, "*", next.pos});
                }
                // 规则 2: 常量标识符 紧跟 数字/非括号标识符 (例如 pi 2 -> pi * 2, 注意排除
                // func(...) 调用)
                else if (cur_is_ident && next_is_num_or_ident) {
                    tokens.push_back(Token{TokenType::Star, 0.0, "*", next.pos});
                }
            }
        }

        tokens.push_back(Token{TokenType::EndOfInput, 0.0, "", input_.size()});
        return tokens;
    }

private:

    static bool isKeywordOperator(std::string_view name) {
        std::string lower = toLower(name);
        return lower == "and" || lower == "or" || lower == "not";
    }

    Token readNumber() {
        size_t start = pos_;
        // 检查十六进制 0x, 二进制 0b, 八进制 0o
        if (pos_ + 2 < input_.size() && input_[pos_] == '0') {
            char prefix = input_[pos_ + 1];
            if (prefix == 'x' || prefix == 'X') {
                pos_             += 2;
                size_t num_start  = pos_;
                while (pos_ < input_.size()
                       && std::isxdigit(static_cast<unsigned char>(input_[pos_]))) {
                    ++pos_;
                }
                if (pos_ == num_start) {
                    throw MathEvalException("Invalid hex number literal", start + 1);
                }
                std::string num_str(input_.substr(num_start, pos_ - num_start));
                uint64_t    val = 0;
                auto [ptr, ec]
                    = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val, 16);
                if (ec != std::errc()) {
                    throw MathEvalException("Hex number parsing failed or overflow", start + 1);
                }
                return Token{
                    TokenType::Number,
                    static_cast<double>(val),
                    std::string(input_.substr(start, pos_ - start)),
                    start
                };
            } else if (prefix == 'b' || prefix == 'B') {
                pos_             += 2;
                size_t num_start  = pos_;
                while (pos_ < input_.size() && (input_[pos_] == '0' || input_[pos_] == '1')) {
                    ++pos_;
                }
                if (pos_ == num_start) {
                    throw MathEvalException("Invalid binary number literal", start + 1);
                }
                std::string num_str(input_.substr(num_start, pos_ - num_start));
                uint64_t    val = 0;
                auto [ptr, ec]
                    = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val, 2);
                if (ec != std::errc()) {
                    throw MathEvalException("Binary number parsing failed or overflow", start + 1);
                }
                return Token{
                    TokenType::Number,
                    static_cast<double>(val),
                    std::string(input_.substr(start, pos_ - start)),
                    start
                };
            } else if (prefix == 'o' || prefix == 'O') {
                pos_             += 2;
                size_t num_start  = pos_;
                while (pos_ < input_.size() && (input_[pos_] >= '0' && input_[pos_] <= '7')) {
                    ++pos_;
                }
                if (pos_ == num_start) {
                    throw MathEvalException("Invalid octal number literal", start + 1);
                }
                std::string num_str(input_.substr(num_start, pos_ - num_start));
                uint64_t    val = 0;
                auto [ptr, ec]
                    = std::from_chars(num_str.data(), num_str.data() + num_str.size(), val, 8);
                if (ec != std::errc()) {
                    throw MathEvalException("Octal number parsing failed or overflow", start + 1);
                }
                return Token{
                    TokenType::Number,
                    static_cast<double>(val),
                    std::string(input_.substr(start, pos_ - start)),
                    start
                };
            }
        }

        // 十进制整数与浮点数
        bool seen_dot = false;
        bool seen_exp = false;

        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '.' && !seen_dot && !seen_exp) {
                seen_dot = true;
                ++pos_;
            } else if ((c == 'e' || c == 'E') && !seen_exp) {
                seen_exp = true;
                ++pos_;
                if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                    ++pos_;
                }
                if (pos_ >= input_.size()
                    || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                    throw MathEvalException("Invalid floating point exponent", start + 1);
                }
            } else {
                break;
            }
        }

        std::string num_str(input_.substr(start, pos_ - start));
        char*       end_ptr = nullptr;
        double      val     = std::strtod(num_str.c_str(), &end_ptr);
        if (end_ptr != num_str.c_str() + num_str.size()) {
            throw MathEvalException("Invalid number format", start + 1);
        }
        return Token{TokenType::Number, val, num_str, start};
    }

    Token readIdentifier() {
        size_t start = pos_;
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                ++pos_;
            } else {
                break;
            }
        }
        std::string name(input_.substr(start, pos_ - start));
        return Token{TokenType::Identifier, 0.0, name, start};
    }

    std::string_view input_;
    size_t           pos_;
};

/// 语法解析与求值器 (Parser & Evaluator)
class Parser {
public:

    Parser(std::vector<Token> tokens, AngleUnit angle_unit) :
        tokens_(std::move(tokens)),
        angle_unit_(angle_unit),
        index_(0) {}

    double evaluate() {
        if (tokens_.empty() || (tokens_.size() == 1 && tokens_[0].type == TokenType::EndOfInput)) {
            throw MathEvalException("Expression is empty");
        }
        double result = parseTernary();
        if (current().type != TokenType::EndOfInput) {
            throw MathEvalException(
                fmt::format("Unexpected extra input '{}'", current().text),
                current().pos + 1
            );
        }
        return result;
    }

private:

    const Token& current() const {
        if (index_ < tokens_.size()) {
            return tokens_[index_];
        }
        return tokens_.back();
    }

    const Token& advance() {
        if (index_ < tokens_.size()) {
            return tokens_[index_++];
        }
        return tokens_.back();
    }

    bool match(TokenType type) {
        if (current().type == type) {
            advance();
            return true;
        }
        return false;
    }

    // 1. 三元运算符 condition ? expr_true : expr_false
    double parseTernary() {
        double val = parseLogicalOr();
        if (match(TokenType::Question)) {
            size_t q_pos    = tokens_[index_ - 1].pos;
            double true_val = parseTernary();
            if (!match(TokenType::Colon)) {
                throw MathEvalException("Missing ':' in ternary operator", q_pos + 1);
            }
            double false_val = parseTernary();
            return (val != 0.0 && !std::isnan(val)) ? true_val : false_val;
        }
        return val;
    }

    // 2. 逻辑或 ||, or
    double parseLogicalOr() {
        double left = parseLogicalAnd();
        while (current().type == TokenType::DoublePipe
               || (current().type == TokenType::Identifier && toLower(current().text) == "or")) {
            advance();
            double right = parseLogicalAnd();
            bool   l     = (left != 0.0 && !std::isnan(left));
            bool   r     = (right != 0.0 && !std::isnan(right));
            left         = (l || r) ? 1.0 : 0.0;
        }
        return left;
    }

    // 3. 逻辑与 &&, and
    double parseLogicalAnd() {
        double left = parseBitwiseOr();
        while (current().type == TokenType::DoubleAmpersand
               || (current().type == TokenType::Identifier && toLower(current().text) == "and")) {
            advance();
            double right = parseBitwiseOr();
            bool   l     = (left != 0.0 && !std::isnan(left));
            bool   r     = (right != 0.0 && !std::isnan(right));
            left         = (l && r) ? 1.0 : 0.0;
        }
        return left;
    }

    // 4. 按位或 |
    double parseBitwiseOr() {
        double left = parseBitwiseAnd();
        while (current().type == TokenType::Pipe) {
            advance();
            double  right = parseBitwiseAnd();
            int64_t a     = toInt64(left, "bitwise OR operand");
            int64_t b     = toInt64(right, "bitwise OR operand");
            left          = static_cast<double>(a | b);
        }
        return left;
    }

    // 5. 按位与 &
    double parseBitwiseAnd() {
        double left = parseEquality();
        while (current().type == TokenType::Ampersand) {
            advance();
            double  right = parseEquality();
            int64_t a     = toInt64(left, "bitwise AND operand");
            int64_t b     = toInt64(right, "bitwise AND operand");
            left          = static_cast<double>(a & b);
        }
        return left;
    }

    // 6. 相等性比较 ==, !=, <>
    double parseEquality() {
        double left = parseRelational();
        while (current().type == TokenType::Eq || current().type == TokenType::Ne) {
            TokenType op = current().type;
            advance();
            double right = parseRelational();
            if (op == TokenType::Eq) {
                left = (std::abs(left - right) < 1e-12) ? 1.0 : 0.0;
            } else {
                left = (std::abs(left - right) >= 1e-12) ? 1.0 : 0.0;
            }
        }
        return left;
    }

    // 7. 关系比较 <, <=, >, >=
    double parseRelational() {
        double left = parseShift();
        while (current().type == TokenType::Lt || current().type == TokenType::Le
               || current().type == TokenType::Gt || current().type == TokenType::Ge) {
            TokenType op = current().type;
            advance();
            double right = parseShift();
            if (op == TokenType::Lt) {
                left = (left < right) ? 1.0 : 0.0;
            } else if (op == TokenType::Le) {
                left = (left <= right) ? 1.0 : 0.0;
            } else if (op == TokenType::Gt) {
                left = (left > right) ? 1.0 : 0.0;
            } else if (op == TokenType::Ge) {
                left = (left >= right) ? 1.0 : 0.0;
            }
        }
        return left;
    }

    // 8. 位移 <<, >>
    double parseShift() {
        double left = parseAdditive();
        while (current().type == TokenType::Shl || current().type == TokenType::Shr) {
            TokenType op     = current().type;
            size_t    op_pos = current().pos;
            advance();
            double  right = parseAdditive();
            int64_t a     = toInt64(left, "shift operand", op_pos);
            int64_t b     = toInt64(right, "shift amount", op_pos);
            if (b < 0 || b > 63) {
                throw MathEvalException(
                    fmt::format("Shift amount {} out of range [0, 63]", b),
                    op_pos + 1
                );
            }
            if (op == TokenType::Shl) {
                left = static_cast<double>(static_cast<uint64_t>(a) << b);
            } else {
                left = static_cast<double>(a >> b);
            }
        }
        return left;
    }

    // 9. 加减 +, -
    double parseAdditive() {
        double left = parseMultiplicative();
        while (current().type == TokenType::Plus || current().type == TokenType::Minus) {
            TokenType op = current().type;
            advance();
            double right = parseMultiplicative();
            if (op == TokenType::Plus) {
                left += right;
            } else {
                left -= right;
            }
        }
        return left;
    }

    // 10. 乘除模 *, /, //, %
    double parseMultiplicative() {
        double left = parseUnaryPrefix();
        while (current().type == TokenType::Star || current().type == TokenType::Slash
               || current().type == TokenType::DoubleSlash
               || current().type == TokenType::Percent) {
            TokenType op     = current().type;
            size_t    op_pos = current().pos;
            advance();
            double right = parseUnaryPrefix();
            if (op == TokenType::Star) {
                left *= right;
            } else if (op == TokenType::Slash) {
                if (right == 0.0) {
                    throw MathEvalException("Division by zero", op_pos + 1);
                }
                left /= right;
            } else if (op == TokenType::DoubleSlash) {
                if (right == 0.0) {
                    throw MathEvalException("Floor division by zero", op_pos + 1);
                }
                left = std::floor(left / right);
            } else if (op == TokenType::Percent) {
                if (right == 0.0) {
                    throw MathEvalException("Modulo by zero", op_pos + 1);
                }
                left = std::fmod(left, right);
            }
        }
        return left;
    }

    // 11. 一元前缀 +, -, ~, !, not
    double parseUnaryPrefix() {
        if (current().type == TokenType::Plus) {
            advance();
            return parseUnaryPrefix();
        }
        if (current().type == TokenType::Minus) {
            advance();
            return -parseUnaryPrefix();
        }
        if (current().type == TokenType::Tilde) {
            size_t op_pos = current().pos;
            advance();
            double  val     = parseUnaryPrefix();
            int64_t int_val = toInt64(val, "bitwise NOT operand", op_pos);
            return static_cast<double>(~int_val);
        }
        if (current().type == TokenType::Exclamation
            || (current().type == TokenType::Identifier && toLower(current().text) == "not")) {
            advance();
            double val = parseUnaryPrefix();
            bool   b   = (val != 0.0 && !std::isnan(val));
            return b ? 0.0 : 1.0;
        }
        return parsePower();
    }

    // 12. 乘方 ^, ** (右结合)
    double parsePower() {
        double left = parsePostfix();
        if (current().type == TokenType::Caret || current().type == TokenType::StarStar) {
            size_t op_pos = current().pos;
            advance();
            double right = parseUnaryPrefix(); // 右结合
            if (left < 0.0 && std::abs(right - std::round(right)) > 1e-9) {
                throw MathEvalException(
                    "Domain error: negative base with fractional exponent",
                    op_pos + 1
                );
            }
            if (left == 0.0 && right < 0.0) {
                throw MathEvalException(
                    "Division by zero in power (0 raised to negative exponent)",
                    op_pos + 1
                );
            }
            return std::pow(left, right);
        }
        return left;
    }

    // 13. 一元后缀 ! (阶乘)
    double parsePostfix() {
        double val = parsePrimary();
        while (current().type == TokenType::Exclamation) {
            size_t op_pos = current().pos;
            advance();
            val = calculateFactorial(val, op_pos + 1);
        }
        return val;
    }

    // 14. 初等表达式
    double parsePrimary() {
        const auto& tok = current();

        // 括号表达式 (...)
        if (match(TokenType::LParen)) {
            size_t open_pos = tokens_[index_ - 1].pos;
            double val      = parseTernary();
            if (!match(TokenType::RParen)) {
                throw MathEvalException("Unclosed parenthesis '('", open_pos + 1);
            }
            return val;
        }

        // 方括号表达式 [...]
        if (match(TokenType::LBracket)) {
            size_t open_pos = tokens_[index_ - 1].pos;
            double val      = parseTernary();
            if (!match(TokenType::RBracket)) {
                throw MathEvalException("Unclosed bracket '['", open_pos + 1);
            }
            return val;
        }

        // 数字字面量
        if (tok.type == TokenType::Number) {
            advance();
            return tok.number_value;
        }

        // 标识符 (常量或函数调用)
        if (tok.type == TokenType::Identifier) {
            std::string name     = tok.text;
            size_t      name_pos = tok.pos;
            advance();

            // 如果紧随括号，则为函数调用
            if (current().type == TokenType::LParen || current().type == TokenType::LBracket) {
                TokenType close_type = (current().type == TokenType::LParen) ? TokenType::RParen
                                                                             : TokenType::RBracket;
                size_t    open_pos   = current().pos;
                advance();

                std::vector<double> args;
                if (current().type != close_type) {
                    while (true) {
                        args.push_back(parseTernary());
                        if (match(TokenType::Comma)) {
                            continue;
                        }
                        break;
                    }
                }
                if (!match(close_type)) {
                    throw MathEvalException("Unclosed argument list for function", open_pos + 1);
                }
                return evaluateFunction(name, args, name_pos);
            }

            // 常量或特殊标识符解析
            return evaluateConstant(name, name_pos);
        }

        if (tok.type == TokenType::EndOfInput) {
            throw MathEvalException("Unexpected end of expression", tok.pos + 1);
        }

        throw MathEvalException(fmt::format("Unexpected token '{}'", tok.text), tok.pos + 1);
    }

    int64_t toInt64(double val, std::string_view desc, size_t pos = 0) {
        if (std::isnan(val) || std::isinf(val)) {
            throw MathEvalException(
                fmt::format("Invalid value for {}: non-finite number", desc),
                pos + 1
            );
        }
        return static_cast<int64_t>(std::round(val));
    }

    double evaluateConstant(std::string_view name, size_t pos) {
        std::string lower = toLower(name);
        if (lower == "pi") {
            return std::numbers::pi_v<double>;
        }
        if (lower == "e") {
            return std::numbers::e_v<double>;
        }
        if (lower == "tau") {
            return 2.0 * std::numbers::pi_v<double>;
        }
        if (lower == "phi") {
            return (1.0 + std::sqrt(5.0)) / 2.0;
        }
        if (lower == "inf" || lower == "infinity") {
            return std::numeric_limits<double>::infinity();
        }
        if (lower == "nan") {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (lower == "true") {
            return 1.0;
        }
        if (lower == "false") {
            return 0.0;
        }
        throw MathEvalException(fmt::format("Unknown constant or identifier '{}'", name), pos + 1);
    }

    double evaluateFunction(std::string_view name, const std::vector<double>& args, size_t pos) {
        std::string lower = toLower(name);

        auto checkArgs = [&](size_t expected) {
            if (args.size() != expected) {
                throw MathEvalException(
                    fmt::format(
                        "Function '{}' expects {} arguments, but got {}",
                        name,
                        expected,
                        args.size()
                    ),
                    pos + 1
                );
            }
        };

        auto checkArgsRange = [&](size_t min_count, size_t max_count) {
            if (args.size() < min_count || args.size() > max_count) {
                throw MathEvalException(
                    fmt::format(
                        "Function '{}' expects between {} and {} arguments, but got {}",
                        name,
                        min_count,
                        max_count,
                        args.size()
                    ),
                    pos + 1
                );
            }
        };

        // 基础数学函数
        if (lower == "abs") {
            checkArgs(1);
            return std::abs(args[0]);
        }
        if (lower == "floor") {
            checkArgs(1);
            return std::floor(args[0]);
        }
        if (lower == "ceil") {
            checkArgs(1);
            return std::ceil(args[0]);
        }
        if (lower == "round") {
            checkArgsRange(1, 2);
            if (args.size() == 1) {
                return std::round(args[0]);
            }
            int64_t decimals = toInt64(args[1], "round decimal places", pos);
            double  factor   = std::pow(10.0, static_cast<double>(decimals));
            return std::round(args[0] * factor) / factor;
        }
        if (lower == "trunc") {
            checkArgs(1);
            return std::trunc(args[0]);
        }
        if (lower == "sign" || lower == "sgn") {
            checkArgs(1);
            if (args[0] > 0.0) {
                return 1.0;
            }
            if (args[0] < 0.0) {
                return -1.0;
            }
            return 0.0;
        }

        // 幂与根号
        if (lower == "sqrt") {
            checkArgs(1);
            if (args[0] < 0.0) {
                throw MathEvalException(
                    fmt::format("Domain error: sqrt of negative value ({})", args[0]),
                    pos + 1
                );
            }
            return std::sqrt(args[0]);
        }
        if (lower == "cbrt") {
            checkArgs(1);
            return std::cbrt(args[0]);
        }
        if (lower == "root") {
            checkArgs(2);
            double x = args[0];
            double n = args[1];
            if (n == 0.0) {
                throw MathEvalException("0th root is undefined", pos + 1);
            }
            if (x < 0.0 && std::fmod(n, 2.0) == 0.0) {
                throw MathEvalException(
                    fmt::format("Domain error: even root of negative value ({})", x),
                    pos + 1
                );
            }
            if (x < 0.0) {
                return -std::pow(-x, 1.0 / n);
            }
            return std::pow(x, 1.0 / n);
        }
        if (lower == "pow") {
            checkArgs(2);
            if (args[0] < 0.0 && std::abs(args[1] - std::round(args[1])) > 1e-9) {
                throw MathEvalException(
                    "Domain error: pow with negative base and fractional exponent",
                    pos + 1
                );
            }
            return std::pow(args[0], args[1]);
        }
        if (lower == "exp") {
            checkArgs(1);
            return std::exp(args[0]);
        }
        if (lower == "exp2") {
            checkArgs(1);
            return std::exp2(args[0]);
        }
        if (lower == "expm1") {
            checkArgs(1);
            return std::expm1(args[0]);
        }

        // 对数
        if (lower == "ln" || lower == "log") {
            checkArgs(1);
            if (args[0] <= 0.0) {
                throw MathEvalException(
                    fmt::format("Domain error: ln of non-positive value ({})", args[0]),
                    pos + 1
                );
            }
            return std::log(args[0]);
        }
        if (lower == "log10") {
            checkArgs(1);
            if (args[0] <= 0.0) {
                throw MathEvalException(
                    fmt::format("Domain error: log10 of non-positive value ({})", args[0]),
                    pos + 1
                );
            }
            return std::log10(args[0]);
        }
        if (lower == "log2") {
            checkArgs(1);
            if (args[0] <= 0.0) {
                throw MathEvalException(
                    fmt::format("Domain error: log2 of non-positive value ({})", args[0]),
                    pos + 1
                );
            }
            return std::log2(args[0]);
        }
        if (lower == "logb" || lower == "logn") {
            checkArgs(2);
            double x    = args[0];
            double base = args[1];
            if (x <= 0.0 || base <= 0.0 || std::abs(base - 1.0) < 1e-12) {
                throw MathEvalException("Domain error: invalid base or argument for log", pos + 1);
            }
            return std::log(x) / std::log(base);
        }
        if (lower == "log1p") {
            checkArgs(1);
            if (args[0] <= -1.0) {
                throw MathEvalException("Domain error: log1p argument must be > -1", pos + 1);
            }
            return std::log1p(args[0]);
        }

        // 角度转换
        if (lower == "rad") {
            checkArgs(1);
            return args[0] * std::numbers::pi_v<double> / 180.0;
        }
        if (lower == "deg") {
            checkArgs(1);
            return args[0] * 180.0 / std::numbers::pi_v<double>;
        }

        // 三角函数 (受 angle_unit_ 控制)
        if (lower == "sin") {
            checkArgs(1);
            double x = (angle_unit_ == AngleUnit::Degrees)
                           ? (args[0] * std::numbers::pi_v<double> / 180.0)
                           : args[0];
            return std::sin(x);
        }
        if (lower == "cos") {
            checkArgs(1);
            double x = (angle_unit_ == AngleUnit::Degrees)
                           ? (args[0] * std::numbers::pi_v<double> / 180.0)
                           : args[0];
            return std::cos(x);
        }
        if (lower == "tan") {
            checkArgs(1);
            double x = (angle_unit_ == AngleUnit::Degrees)
                           ? (args[0] * std::numbers::pi_v<double> / 180.0)
                           : args[0];
            return std::tan(x);
        }
        if (lower == "asin" || lower == "arcsin") {
            checkArgs(1);
            if (args[0] < -1.0 || args[0] > 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: asin argument {} out of range [-1, 1]", args[0]),
                    pos + 1
                );
            }
            double res = std::asin(args[0]);
            return (angle_unit_ == AngleUnit::Degrees) ? (res * 180.0 / std::numbers::pi_v<double>)
                                                       : res;
        }
        if (lower == "acos" || lower == "arccos") {
            checkArgs(1);
            if (args[0] < -1.0 || args[0] > 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: acos argument {} out of range [-1, 1]", args[0]),
                    pos + 1
                );
            }
            double res = std::acos(args[0]);
            return (angle_unit_ == AngleUnit::Degrees) ? (res * 180.0 / std::numbers::pi_v<double>)
                                                       : res;
        }
        if (lower == "atan" || lower == "arctan") {
            checkArgs(1);
            double res = std::atan(args[0]);
            return (angle_unit_ == AngleUnit::Degrees) ? (res * 180.0 / std::numbers::pi_v<double>)
                                                       : res;
        }
        if (lower == "atan2") {
            checkArgs(2);
            double res = std::atan2(args[0], args[1]);
            return (angle_unit_ == AngleUnit::Degrees) ? (res * 180.0 / std::numbers::pi_v<double>)
                                                       : res;
        }

        // 显式角度三角函数 (sind, cosd, tand, asind, acosd, atand)
        if (lower == "sind") {
            checkArgs(1);
            return std::sin(args[0] * std::numbers::pi_v<double> / 180.0);
        }
        if (lower == "cosd") {
            checkArgs(1);
            return std::cos(args[0] * std::numbers::pi_v<double> / 180.0);
        }
        if (lower == "tand") {
            checkArgs(1);
            return std::tan(args[0] * std::numbers::pi_v<double> / 180.0);
        }
        if (lower == "asind") {
            checkArgs(1);
            if (args[0] < -1.0 || args[0] > 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: asind argument {} out of range [-1, 1]", args[0]),
                    pos + 1
                );
            }
            return std::asin(args[0]) * 180.0 / std::numbers::pi_v<double>;
        }
        if (lower == "acosd") {
            checkArgs(1);
            if (args[0] < -1.0 || args[0] > 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: acosd argument {} out of range [-1, 1]", args[0]),
                    pos + 1
                );
            }
            return std::acos(args[0]) * 180.0 / std::numbers::pi_v<double>;
        }
        if (lower == "atand") {
            checkArgs(1);
            return std::atan(args[0]) * 180.0 / std::numbers::pi_v<double>;
        }

        // 双曲函数
        if (lower == "sinh") {
            checkArgs(1);
            return std::sinh(args[0]);
        }
        if (lower == "cosh") {
            checkArgs(1);
            return std::cosh(args[0]);
        }
        if (lower == "tanh") {
            checkArgs(1);
            return std::tanh(args[0]);
        }
        if (lower == "asinh") {
            checkArgs(1);
            return std::asinh(args[0]);
        }
        if (lower == "acosh") {
            checkArgs(1);
            if (args[0] < 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: acosh argument {} < 1", args[0]),
                    pos + 1
                );
            }
            return std::acosh(args[0]);
        }
        if (lower == "atanh") {
            checkArgs(1);
            if (args[0] <= -1.0 || args[0] >= 1.0) {
                throw MathEvalException(
                    fmt::format("Domain error: atanh argument {} out of range (-1, 1)", args[0]),
                    pos + 1
                );
            }
            return std::atanh(args[0]);
        }

        // 组合与阶乘
        if (lower == "fact" || lower == "factorial") {
            checkArgs(1);
            return calculateFactorial(args[0], pos + 1);
        }
        if (lower == "comb" || lower == "ncr") {
            checkArgs(2);
            int64_t n = toInt64(args[0], "comb n", pos);
            int64_t r = toInt64(args[1], "comb r", pos);
            return calculateComb(n, r, pos + 1);
        }
        if (lower == "perm" || lower == "npr") {
            checkArgs(2);
            int64_t n = toInt64(args[0], "perm n", pos);
            int64_t r = toInt64(args[1], "perm r", pos);
            return calculatePerm(n, r, pos + 1);
        }
        if (lower == "gcd") {
            if (args.size() < 2) {
                throw MathEvalException("Function 'gcd' expects at least 2 arguments", pos + 1);
            }
            int64_t g = toInt64(args[0], "gcd arg", pos);
            for (size_t i = 1; i < args.size(); ++i) {
                g = gcd(g, toInt64(args[i], "gcd arg", pos));
            }
            return static_cast<double>(g);
        }
        if (lower == "lcm") {
            if (args.size() < 2) {
                throw MathEvalException("Function 'lcm' expects at least 2 arguments", pos + 1);
            }
            int64_t l = toInt64(args[0], "lcm arg", pos);
            for (size_t i = 1; i < args.size(); ++i) {
                l = lcm(l, toInt64(args[i], "lcm arg", pos));
            }
            return static_cast<double>(l);
        }
        if (lower == "mod" || lower == "fmod") {
            checkArgs(2);
            if (args[1] == 0.0) {
                throw MathEvalException("Modulo by zero", pos + 1);
            }
            return std::fmod(args[0], args[1]);
        }
        if (lower == "rem" || lower == "remainder") {
            checkArgs(2);
            if (args[1] == 0.0) {
                throw MathEvalException("Remainder by zero", pos + 1);
            }
            return std::remainder(args[0], args[1]);
        }

        // 统计 / 多参数函数
        if (lower == "min") {
            if (args.empty()) {
                throw MathEvalException("Function 'min' expects at least 1 argument", pos + 1);
            }
            return *std::min_element(args.begin(), args.end());
        }
        if (lower == "max") {
            if (args.empty()) {
                throw MathEvalException("Function 'max' expects at least 1 argument", pos + 1);
            }
            return *std::max_element(args.begin(), args.end());
        }
        if (lower == "sum") {
            if (args.empty()) {
                return 0.0;
            }
            return std::accumulate(args.begin(), args.end(), 0.0);
        }
        if (lower == "avg" || lower == "mean") {
            if (args.empty()) {
                throw MathEvalException("Function 'avg' expects at least 1 argument", pos + 1);
            }
            return std::accumulate(args.begin(), args.end(), 0.0)
                   / static_cast<double>(args.size());
        }
        if (lower == "hypot") {
            if (args.empty()) {
                return 0.0;
            }
            double sum_sq = 0.0;
            for (double v : args) {
                sum_sq += v * v;
            }
            return std::sqrt(sum_sq);
        }
        if (lower == "clamp") {
            checkArgs(3);
            double val     = args[0];
            double min_val = args[1];
            double max_val = args[2];
            if (min_val > max_val) {
                std::swap(min_val, max_val);
            }
            return std::clamp(val, min_val, max_val);
        }

        // 误差与特殊函数
        if (lower == "erf") {
            checkArgs(1);
            return std::erf(args[0]);
        }
        if (lower == "erfc") {
            checkArgs(1);
            return std::erfc(args[0]);
        }
        if (lower == "gamma" || lower == "tgamma") {
            checkArgs(1);
            return std::tgamma(args[0]);
        }
        if (lower == "lgamma") {
            checkArgs(1);
            return std::lgamma(args[0]);
        }

        // 位运算函数
        if (lower == "band") {
            if (args.size() < 2) {
                throw MathEvalException("Function 'band' expects at least 2 arguments", pos + 1);
            }
            int64_t res = toInt64(args[0], "band arg", pos);
            for (size_t i = 1; i < args.size(); ++i) {
                res &= toInt64(args[i], "band arg", pos);
            }
            return static_cast<double>(res);
        }
        if (lower == "bor") {
            if (args.size() < 2) {
                throw MathEvalException("Function 'bor' expects at least 2 arguments", pos + 1);
            }
            int64_t res = toInt64(args[0], "bor arg", pos);
            for (size_t i = 1; i < args.size(); ++i) {
                res |= toInt64(args[i], "bor arg", pos);
            }
            return static_cast<double>(res);
        }
        if (lower == "bxor" || lower == "xor") {
            if (args.size() < 2) {
                throw MathEvalException("Function 'bxor' expects at least 2 arguments", pos + 1);
            }
            int64_t res = toInt64(args[0], "bxor arg", pos);
            for (size_t i = 1; i < args.size(); ++i) {
                res ^= toInt64(args[i], "bxor arg", pos);
            }
            return static_cast<double>(res);
        }
        if (lower == "bnot") {
            checkArgs(1);
            return static_cast<double>(~toInt64(args[0], "bnot arg", pos));
        }
        if (lower == "shl") {
            checkArgs(2);
            int64_t a = toInt64(args[0], "shl value", pos);
            int64_t b = toInt64(args[1], "shl amount", pos);
            if (b < 0 || b > 63) {
                throw MathEvalException("Shift amount out of range [0, 63]", pos + 1);
            }
            return static_cast<double>(static_cast<uint64_t>(a) << b);
        }
        if (lower == "shr") {
            checkArgs(2);
            int64_t a = toInt64(args[0], "shr value", pos);
            int64_t b = toInt64(args[1], "shr amount", pos);
            if (b < 0 || b > 63) {
                throw MathEvalException("Shift amount out of range [0, 63]", pos + 1);
            }
            return static_cast<double>(a >> b);
        }

        throw MathEvalException(fmt::format("Unknown function '{}'", name), pos + 1);
    }

    std::vector<Token> tokens_;
    AngleUnit          angle_unit_;
    size_t             index_ = 0;
};

/// 格式化计算结果
inline std::string formatResult(double val, std::optional<int> precision) {
    if (std::isnan(val)) {
        return "NaN";
    }
    if (std::isinf(val)) {
        return (val > 0) ? "Infinity" : "-Infinity";
    }

    // 指定精度输出
    if (precision.has_value()) {
        int p = std::clamp(precision.value(), 0, 15);
        return fmt::format("{:.{}f}", val, p);
    }

    // 若接近整数，则直接输出整数形式
    if (std::abs(val) < 1e15 && std::abs(val - std::round(val)) < 1e-11) {
        return fmt::format("{}", static_cast<int64_t>(std::round(val)));
    }

    // 默认自适应浮点格式 (最多14位有效数字，去掉末尾多余0)
    std::string s = fmt::format("{:.14g}", val);
    return s;
}

} // namespace detail

/// 解析并计算表达式字符串
inline double evaluateExpression(std::string_view expr, AngleUnit angle_unit = AngleUnit::Radians) {
    detail::Lexer  lexer(expr);
    auto           tokens = lexer.tokenize();
    detail::Parser parser(std::move(tokens), angle_unit);
    return parser.evaluate();
}

/// agentxx_math_calculate 执行体
/// - 输入 arguments JSON:
///   - `expression` (string, required): 要计算的数学表达式
///   - `precision` (int, optional): 保留小数位数
///   - `angle_unit` (string, optional): 角度单位 ("rad" 或 "deg", 默认为 "rad")
inline std::string mathCalculateExecute(const neograph::json& arguments) {
    auto expr = arguments.value("expression", std::string{});
    if (expr.empty()) {
        return R"({"error":"Arg `expression` is empty"})";
    }

    std::optional<int> precision;
    if (arguments.contains("precision") && arguments["precision"].is_number()) {
        precision = arguments["precision"].get<int>();
    }

    AngleUnit angle_unit = AngleUnit::Radians;
    if (arguments.contains("angle_unit") && arguments["angle_unit"].is_string()) {
        std::string unit_str = detail::toLower(arguments["angle_unit"].get<std::string>());
        if (unit_str == "deg" || unit_str == "degree" || unit_str == "degrees") {
            angle_unit = AngleUnit::Degrees;
        }
    }

    try {
        double result = evaluateExpression(expr, angle_unit);
        return detail::formatResult(result, precision);
    } catch (const MathEvalException& e) {
        return e.what();
    } catch (const std::exception& e) {
        return fmt::format("[Error] {}", e.what());
    } catch (...) {
        return "[Error] Unknown error occurred during evaluation";
    }
}

} // namespace agentxx_math_plugin
