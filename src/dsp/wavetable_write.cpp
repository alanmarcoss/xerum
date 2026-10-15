#include "wavetable_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace xerum::dsp
{
namespace
{

// ---------------------------------------------------------------------------------- the tokeniser

enum class Var : std::uint8_t { X, W, I, Frame, Y, Z, Size, Frames };

enum class Op : std::uint8_t
{
    Add, Sub, Mul, Div, Mod, Pow,
    Neg, Not,
    Less, Greater, LessEqual, GreaterEqual, Equal, NotEqual,
    LogicalAnd, LogicalOr
};

enum class Builtin : std::uint8_t
{
    Sin, Cos, Tan, Asin, Acos, Atan, Atan2,
    Sinh, Cosh, Tanh, Asinh, Acosh, Atanh,
    Log, Ln, Log2, Log10, Exp, Sqrt, Cbrt,
    Abs, Sign, Rint, Floor, Ceil, Round, Trunc, Frac,
    Step, Min, Max, Sum, Avg, Pow, Clamp
};

struct FunctionInfo
{
    std::string_view name;
    Builtin builtin;
    int minArgs;
    int maxArgs;   ///< -1 means variadic
};

// Order matters only for readability; lookup is by name.
constexpr FunctionInfo kFunctions[] =
{
    { "sin",   Builtin::Sin,   1,  1 }, { "cos",   Builtin::Cos,   1,  1 },
    { "tan",   Builtin::Tan,   1,  1 }, { "asin",  Builtin::Asin,  1,  1 },
    { "acos",  Builtin::Acos,  1,  1 }, { "atan",  Builtin::Atan,  1,  1 },
    { "atan2", Builtin::Atan2, 2,  2 }, { "sinh",  Builtin::Sinh,  1,  1 },
    { "cosh",  Builtin::Cosh,  1,  1 }, { "tanh",  Builtin::Tanh,  1,  1 },
    { "asinh", Builtin::Asinh, 1,  1 }, { "acosh", Builtin::Acosh, 1,  1 },
    { "atanh", Builtin::Atanh, 1,  1 }, { "log",   Builtin::Log,   1,  1 },
    { "ln",    Builtin::Ln,    1,  1 }, { "log2",  Builtin::Log2,  1,  1 },
    { "log10", Builtin::Log10, 1,  1 }, { "exp",   Builtin::Exp,   1,  1 },
    { "sqrt",  Builtin::Sqrt,  1,  1 }, { "cbrt",  Builtin::Cbrt,  1,  1 },
    { "abs",   Builtin::Abs,   1,  1 }, { "sign",  Builtin::Sign,  1,  1 },
    { "rint",  Builtin::Rint,  1,  1 }, { "floor", Builtin::Floor, 1,  1 },
    { "ceil",  Builtin::Ceil,  1,  1 }, { "round", Builtin::Round, 1,  1 },
    { "trunc", Builtin::Trunc, 1,  1 }, { "frac",  Builtin::Frac,  1,  1 },
    { "step",  Builtin::Step,  2,  2 }, { "min",   Builtin::Min,   1, -1 },
    { "max",   Builtin::Max,   1, -1 }, { "sum",   Builtin::Sum,   1, -1 },
    { "avg",   Builtin::Avg,   1, -1 }, { "pow",   Builtin::Pow,   2,  2 },
    { "clamp", Builtin::Clamp, 3,  3 },
};

const FunctionInfo* findFunction (std::string_view name)
{
    for (const auto& function : kFunctions)
        if (function.name == name)
            return &function;

    return nullptr;
}

bool findVariable (std::string_view name, Var& out)
{
    if (name == "x") { out = Var::X; return true; }
    if (name == "w") { out = Var::W; return true; }
    if (name == "i") { out = Var::I; return true; }
    if (name == "n") { out = Var::Frame; return true; }
    if (name == "y") { out = Var::Y; return true; }
    if (name == "z") { out = Var::Z; return true; }
    if (name == "N") { out = Var::Size; return true; }
    if (name == "M") { out = Var::Frames; return true; }

    return false;
}

enum class TokenKind : std::uint8_t { End, Number, Identifier, Operator, Open, Close, Comma };

struct Token
{
    TokenKind kind { TokenKind::End };
    double number { 0.0 };
    std::string text;
};

/// Operators longest-first, so `<=` is never lexed as `<` then `=`.
constexpr std::string_view kOperators[] =
{
    "&&", "||", "==", "!=", "<=", ">=", "+", "-", "*", "/", "%", "^", "<", ">", "!", "?", ":"
};

std::vector<Token> tokenise (std::string_view text, std::string& error)
{
    std::vector<Token> tokens;
    std::size_t i = 0;

    while (i < text.size())
    {
        const char c = text[i];
        const auto byte = static_cast<unsigned char> (c);

        if (std::isspace (byte) != 0)
        {
            ++i;
            continue;
        }

        if (std::isdigit (byte) != 0 || (c == '.' && i + 1 < text.size()))
        {
            const std::size_t start = i;

            while (i < text.size())
            {
                const char d = text[i];

                if (std::isdigit (static_cast<unsigned char> (d)) != 0 || d == '.')
                    ++i;
                else if ((d == 'e' || d == 'E') && i + 1 < text.size())
                    ++i;   // exponent; the sign, if any, is consumed below
                else if ((d == '+' || d == '-') && i > start && (text[i - 1] == 'e' || text[i - 1] == 'E'))
                    ++i;
                else
                    break;
            }

            const std::string literal (text.substr (start, i - start));
            char* end = nullptr;
            const double value = std::strtod (literal.c_str(), &end);

            if (end == literal.c_str() || *end != '\0')
            {
                error = "malformed number \"" + literal + "\"";
                return {};
            }

            tokens.push_back ({ TokenKind::Number, value, {} });
            continue;
        }

        if (std::isalpha (byte) != 0 || c == '_')
        {
            const std::size_t start = i;

            while (i < text.size() && (std::isalnum (static_cast<unsigned char> (text[i])) != 0 || text[i] == '_'))
                ++i;

            tokens.push_back ({ TokenKind::Identifier, 0.0, std::string (text.substr (start, i - start)) });
            continue;
        }

        if (c == '(') { tokens.push_back ({ TokenKind::Open, 0.0, {} }); ++i; continue; }
        if (c == ')') { tokens.push_back ({ TokenKind::Close, 0.0, {} }); ++i; continue; }
        if (c == ',') { tokens.push_back ({ TokenKind::Comma, 0.0, {} }); ++i; continue; }

        bool matched = false;

        for (const auto op : kOperators)
            if (text.compare (i, op.size(), op) == 0)
            {
                tokens.push_back ({ TokenKind::Operator, 0.0, std::string (op) });
                i += op.size();
                matched = true;
                break;
            }

        if (! matched)
        {
            error = "unexpected character '" + std::string (1, c) + "' at offset " + std::to_string (i);
            return {};
        }
    }

    tokens.push_back ({ TokenKind::End, 0.0, {} });
    return tokens;
}

// ---------------------------------------------------------------------------------- the parser

struct Node
{
    enum class Kind : std::uint8_t { Number, Variable, Unary, Binary, Call, Ternary } kind { Kind::Number };

    double number { 0.0 };
    Var variable { Var::X };
    Op op { Op::Add };
    Builtin builtin { Builtin::Sin };
    std::vector<std::uint32_t> children;
};

std::uint32_t addNode (std::vector<Node>& nodes, Node node)
{
    nodes.push_back (std::move (node));
    return static_cast<std::uint32_t> (nodes.size() - 1);
}

class Parser
{
public:
    Parser (std::vector<Token> tokens, std::vector<Node>& nodes)
        : tokens_ (std::move (tokens)), nodes_ (nodes)
    {}

    bool parse (std::uint32_t& root, std::string& error)
    {
        if (tokens_.size() == 1)   // just End
        {
            error = "empty formula";
            return false;
        }

        if (! parseTernary (root, error))
            return false;

        if (peek().kind != TokenKind::End)
        {
            error = "unexpected trailing input";
            return false;
        }

        return true;
    }

private:
    const Token& peek() const { return tokens_[pos_]; }

    bool acceptOperator (std::string_view op)
    {
        if (peek().kind == TokenKind::Operator && peek().text == op)
        {
            ++pos_;
            return true;
        }

        return false;
    }

    bool parseTernary (std::uint32_t& out, std::string& error)
    {
        std::uint32_t condition = 0;

        if (! parseOr (condition, error))
            return false;

        if (! acceptOperator ("?"))
        {
            out = condition;
            return true;
        }

        std::uint32_t whenTrue = 0, whenFalse = 0;

        if (! parseTernary (whenTrue, error))
            return false;

        if (! acceptOperator (":"))
        {
            error = "expected ':' in the ternary expression";
            return false;
        }

        if (! parseTernary (whenFalse, error))
            return false;

        Node node;
        node.kind = Node::Kind::Ternary;
        node.children = { condition, whenTrue, whenFalse };
        out = addNode (nodes_, std::move (node));
        return true;
    }

    /// Left-associative binary levels, one per precedence tier.
    bool parseBinaryLevel (std::uint32_t& out, std::string& error, const std::pair<std::string_view, Op>* ops,
                           std::size_t opCount, bool (Parser::*next) (std::uint32_t&, std::string&))
    {
        std::uint32_t left = 0;

        if (! (this->*next) (left, error))
            return false;

        for (;;)
        {
            bool matched = false;

            for (std::size_t i = 0; i < opCount; ++i)
                if (acceptOperator (ops[i].first))
                {
                    std::uint32_t right = 0;

                    if (! (this->*next) (right, error))
                        return false;

                    Node node;
                    node.kind = Node::Kind::Binary;
                    node.op = ops[i].second;
                    node.children = { left, right };
                    left = addNode (nodes_, std::move (node));
                    matched = true;
                    break;
                }

            if (! matched)
                break;
        }

        out = left;
        return true;
    }

    bool parseOr (std::uint32_t& out, std::string& error)
    {
        static constexpr std::pair<std::string_view, Op> ops[] = { { "||", Op::LogicalOr } };
        return parseBinaryLevel (out, error, ops, std::size (ops), &Parser::parseAnd);
    }

    bool parseAnd (std::uint32_t& out, std::string& error)
    {
        static constexpr std::pair<std::string_view, Op> ops[] = { { "&&", Op::LogicalAnd } };
        return parseBinaryLevel (out, error, ops, std::size (ops), &Parser::parseComparison);
    }

    bool parseComparison (std::uint32_t& out, std::string& error)
    {
        static constexpr std::pair<std::string_view, Op> ops[] =
        {
            { "==", Op::Equal }, { "!=", Op::NotEqual }, { "<=", Op::LessEqual },
            { ">=", Op::GreaterEqual }, { "<", Op::Less }, { ">", Op::Greater },
        };
        return parseBinaryLevel (out, error, ops, std::size (ops), &Parser::parseAdditive);
    }

    bool parseAdditive (std::uint32_t& out, std::string& error)
    {
        static constexpr std::pair<std::string_view, Op> ops[] = { { "+", Op::Add }, { "-", Op::Sub } };
        return parseBinaryLevel (out, error, ops, std::size (ops), &Parser::parseMultiplicative);
    }

    bool parseMultiplicative (std::uint32_t& out, std::string& error)
    {
        static constexpr std::pair<std::string_view, Op> ops[] =
        {
            { "*", Op::Mul }, { "/", Op::Div }, { "%", Op::Mod },
        };
        return parseBinaryLevel (out, error, ops, std::size (ops), &Parser::parsePower);
    }

    /// `^` binds tighter than unary minus and is right-associative: `2^3^2` is `2^(3^2)`.
    bool parsePower (std::uint32_t& out, std::string& error)
    {
        std::uint32_t base = 0;

        if (! parseUnary (base, error))
            return false;

        if (! acceptOperator ("^"))
        {
            out = base;
            return true;
        }

        std::uint32_t exponent = 0;

        if (! parsePower (exponent, error))
            return false;

        Node node;
        node.kind = Node::Kind::Binary;
        node.op = Op::Pow;
        node.children = { base, exponent };
        out = addNode (nodes_, std::move (node));
        return true;
    }

    bool parseUnary (std::uint32_t& out, std::string& error)
    {
        Op op {};

        if (acceptOperator ("-"))      op = Op::Neg;
        else if (acceptOperator ("+")) return parseUnary (out, error);
        else if (acceptOperator ("!")) op = Op::Not;
        else                           return parsePrimary (out, error);

        std::uint32_t operand = 0;

        if (! parseUnary (operand, error))
            return false;

        Node node;
        node.kind = Node::Kind::Unary;
        node.op = op;
        node.children = { operand };
        out = addNode (nodes_, std::move (node));
        return true;
    }

    bool parsePrimary (std::uint32_t& out, std::string& error)
    {
        const Token& token = peek();

        if (token.kind == TokenKind::Number)
        {
            ++pos_;
            Node node;
            node.kind = Node::Kind::Number;
            node.number = token.number;
            out = addNode (nodes_, std::move (node));
            return true;
        }

        if (token.kind == TokenKind::Open)
        {
            ++pos_;

            if (! parseTernary (out, error))
                return false;

            if (peek().kind != TokenKind::Close)
            {
                error = "expected ')'";
                return false;
            }

            ++pos_;
            return true;
        }

        if (token.kind == TokenKind::Identifier)
        {
            const std::string name = token.text;
            ++pos_;

            if (peek().kind == TokenKind::Open)
            {
                const auto* function = findFunction (name);

                if (function == nullptr)
                {
                    error = "unknown function \"" + name + "\"";
                    return false;
                }

                ++pos_;
                std::vector<std::uint32_t> args;

                if (peek().kind != TokenKind::Close)
                {
                    for (;;)
                    {
                        std::uint32_t argument = 0;

                        if (! parseTernary (argument, error))
                            return false;

                        args.push_back (argument);

                        if (peek().kind != TokenKind::Comma)
                            break;

                        ++pos_;
                    }
                }

                if (peek().kind != TokenKind::Close)
                {
                    error = "expected ')' after the arguments of \"" + name + "\"";
                    return false;
                }

                ++pos_;

                const int count = static_cast<int> (args.size());

                if (count < function->minArgs || (function->maxArgs >= 0 && count > function->maxArgs))
                {
                    error = "\"" + name + "\" takes "
                          + (function->maxArgs < 0 ? std::to_string (function->minArgs) + " or more"
                                                   : std::to_string (function->minArgs) + " to "
                                                         + std::to_string (function->maxArgs))
                          + " arguments, got " + std::to_string (count);
                    return false;
                }

                Node node;
                node.kind = Node::Kind::Call;
                node.builtin = function->builtin;
                node.children = std::move (args);
                out = addNode (nodes_, std::move (node));
                return true;
            }

            if (name == "pi" || name == "e")
            {
                Node node;
                node.kind = Node::Kind::Number;
                node.number = name == "pi" ? 3.14159265358979323846 : 2.71828182845904523536;
                out = addNode (nodes_, std::move (node));
                return true;
            }

            Var variable {};

            if (! findVariable (name, variable))
            {
                error = "unknown name \"" + name + "\"";
                return false;
            }

            Node node;
            node.kind = Node::Kind::Variable;
            node.variable = variable;
            out = addNode (nodes_, std::move (node));
            return true;
        }

        error = "expected a value";
        return false;
    }

    std::vector<Token> tokens_;
    std::vector<Node>& nodes_;
    std::size_t pos_ { 0 };
};

bool compileProgram (std::string_view text, std::vector<Node>& nodes, std::uint32_t& root, std::string& error)
{
    auto tokens = tokenise (text, error);

    if (! error.empty())
        return false;

    Parser parser (std::move (tokens), nodes);
    return parser.parse (root, error);
}

// ---------------------------------------------------------------------------------- the evaluator

double evalNode (const std::vector<Node>& nodes, std::uint32_t index, const FormulaContext& context)
{
    const Node& node = nodes[index];

    switch (node.kind)
    {
        case Node::Kind::Number:
            return node.number;

        case Node::Kind::Variable:
            switch (node.variable)
            {
                case Var::X:      return context.x;
                case Var::W:      return context.w;
                case Var::I:      return context.i;
                case Var::Frame:  return context.n;
                case Var::Y:      return context.y;
                case Var::Z:      return context.z;
                case Var::Size:   return context.N;
                case Var::Frames: return context.M;
            }
            return 0.0;

        case Node::Kind::Unary:
        {
            const double a = evalNode (nodes, node.children[0], context);

            switch (node.op)
            {
                case Op::Neg: return -a;
                case Op::Not: return a == 0.0 ? 1.0 : 0.0;
                default:      return a;
            }
        }

        case Node::Kind::Binary:
        {
            const double a = evalNode (nodes, node.children[0], context);

            // `&&` and `||` short-circuit, as they do in the reference's grammar.
            if (node.op == Op::LogicalAnd || node.op == Op::LogicalOr)
            {
                const bool left = a != 0.0;
                const bool right = evalNode (nodes, node.children[1], context) != 0.0;

                if (node.op == Op::LogicalAnd)
                    return left && right ? 1.0 : 0.0;

                return left || right ? 1.0 : 0.0;
            }

            const double b = evalNode (nodes, node.children[1], context);

            switch (node.op)
            {
                case Op::Add:          return a + b;
                case Op::Sub:          return a - b;
                case Op::Mul:          return a * b;
                case Op::Div:          return a / b;
                case Op::Mod:          return std::fmod (a, b);
                case Op::Pow:          return std::pow (a, b);
                case Op::Less:         return a < b ? 1.0 : 0.0;
                case Op::Greater:      return a > b ? 1.0 : 0.0;
                case Op::LessEqual:    return a <= b ? 1.0 : 0.0;
                case Op::GreaterEqual: return a >= b ? 1.0 : 0.0;
                case Op::Equal:        return a == b ? 1.0 : 0.0;
                case Op::NotEqual:     return a != b ? 1.0 : 0.0;
                default:               return 0.0;
            }
        }

        case Node::Kind::Ternary:
            return evalNode (nodes, node.children[0], context) != 0.0
                 ? evalNode (nodes, node.children[1], context)
                 : evalNode (nodes, node.children[2], context);

        case Node::Kind::Call:
        {
            const std::size_t count = node.children.size();

            switch (node.builtin)
            {
                case Builtin::Min:
                {
                    double value = evalNode (nodes, node.children[0], context);
                    for (std::size_t i = 1; i < count; ++i)
                        value = std::min (value, evalNode (nodes, node.children[i], context));
                    return value;
                }
                case Builtin::Max:
                {
                    double value = evalNode (nodes, node.children[0], context);
                    for (std::size_t i = 1; i < count; ++i)
                        value = std::max (value, evalNode (nodes, node.children[i], context));
                    return value;
                }
                case Builtin::Sum:
                {
                    double value = 0.0;
                    for (const auto child : node.children)
                        value += evalNode (nodes, child, context);
                    return value;
                }
                case Builtin::Avg:
                {
                    double value = 0.0;
                    for (const auto child : node.children)
                        value += evalNode (nodes, child, context);
                    return value / static_cast<double> (count);
                }
                default:
                    break;
            }

            const double a = evalNode (nodes, node.children[0], context);
            const double b = count > 1 ? evalNode (nodes, node.children[1], context) : 0.0;
            const double c = count > 2 ? evalNode (nodes, node.children[2], context) : 0.0;

            switch (node.builtin)
            {
                case Builtin::Sin:   return std::sin (a);
                case Builtin::Cos:   return std::cos (a);
                case Builtin::Tan:   return std::tan (a);
                case Builtin::Asin:  return std::asin (a);
                case Builtin::Acos:  return std::acos (a);
                case Builtin::Atan:  return std::atan (a);
                case Builtin::Atan2: return std::atan2 (a, b);
                case Builtin::Sinh:  return std::sinh (a);
                case Builtin::Cosh:  return std::cosh (a);
                case Builtin::Tanh:  return std::tanh (a);
                case Builtin::Asinh: return std::asinh (a);
                case Builtin::Acosh: return std::acosh (a);
                case Builtin::Atanh: return std::atanh (a);
                case Builtin::Log:   return std::log (a);
                case Builtin::Ln:    return std::log (a);
                case Builtin::Log2:  return std::log2 (a);
                case Builtin::Log10: return std::log10 (a);
                case Builtin::Exp:   return std::exp (a);
                case Builtin::Sqrt:  return std::sqrt (a);
                case Builtin::Cbrt:  return std::cbrt (a);
                case Builtin::Abs:   return std::fabs (a);
                case Builtin::Sign:  return a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0);
                case Builtin::Rint:  return std::nearbyint (a);
                case Builtin::Floor: return std::floor (a);
                case Builtin::Ceil:  return std::ceil (a);
                case Builtin::Round: return std::round (a);
                case Builtin::Trunc: return std::trunc (a);
                case Builtin::Frac:  return a - std::floor (a);
                case Builtin::Step:  return b >= a ? 1.0 : 0.0;   // step(edge, x)
                case Builtin::Pow:   return std::pow (a, b);
                case Builtin::Clamp: return std::min (std::max (a, b), c);
                default:             return 0.0;
            }
        }
    }

    return 0.0;
}

// ---------------------------------------------------------------------------------- the encoder

void putU16 (std::vector<std::byte>& out, std::uint16_t value)
{
    out.push_back (std::byte (value & 0xFFu));
    out.push_back (std::byte ((value >> 8) & 0xFFu));
}

void putU32 (std::vector<std::byte>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back (std::byte ((value >> shift) & 0xFFu));
}

void putTag (std::vector<std::byte>& out, std::string_view tag)
{
    for (const char c : tag)
        out.push_back (std::byte (c));
}

void putFloat (std::vector<std::byte>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    putU32 (out, bits);   // little-endian, matching what ticket 24's reader memcpy's back
}

/// The `clm ` payload: `<!><frameSize> <B><C>000000 wavetable (xerum)`, where B is the interpolation
/// mode and C the factory flag. The vendor string is ours; nothing xerum writes may read as a reference
/// file, and the release check verifies no shipped byte sequence is one.
std::string clmPayload (std::size_t frameSize, std::uint8_t mode, bool factory)
{
    char nibbles[9];
    std::snprintf (nibbles, sizeof (nibbles), "%u%u000000", static_cast<unsigned> (mode), factory ? 1u : 0u);

    return "<!>" + std::to_string (frameSize) + " " + nibbles + " wavetable (xerum)";
}

} // namespace

// ---------------------------------------------------------------------------------- Formula

struct Formula::Program
{
    std::vector<Node> nodes;
    std::uint32_t root { 0 };
};

Formula Formula::fromText (std::string_view text)
{
    Formula formula;
    auto program = std::make_shared<Program>();
    std::string error;

    if (! compileProgram (text, program->nodes, program->root, error))
    {
        formula.error_ = error;
        return formula;
    }

    formula.program_ = std::move (program);
    return formula;
}

double Formula::evaluate (const FormulaContext& context) const noexcept
{
    if (program_ == nullptr)
        return 0.0;

    return evalNode (program_->nodes, program_->root, context);
}

FormulaResult evaluateFormula (std::string_view text, const FormulaContext& context)
{
    FormulaResult result;
    const auto formula = Formula::fromText (text);

    if (! formula.valid())
    {
        result.error = formula.error();
        return result;
    }

    result.ok = true;
    result.value = formula.evaluate (context);
    return result;
}

// ---------------------------------------------------------------------------------- the starter library

const std::vector<TableSpec>& starterLibrary()
{
    // Every entry is authored here: basic shapes, which are mathematics, and tables generated from
    // formulas we wrote. The reference's factory formula list is content and is not reproduced
    // (docs/content-policy.md). Frame counts and interpolation modes are ours, not the reference's.
    static const std::vector<TableSpec> library =
    {
        { .name = "Sine",
          .relativePath = "xerum/Sine.wav",
          .formula = "sin(pi*x)",
          .description = "Basic shape: sine. Exactly one DFT bin of a 2048-sample frame.",
          .frameCount = 1,
          .interpolation = FrameInterpolation::None,
          .sixteenBit = false },

        { .name = "Saw",
          .relativePath = "xerum/Saw.wav",
          .formula = "x",
          .description = "Basic shape: saw. The ramp -1..1, harmonics falling as 1/k.",
          .frameCount = 1,
          .interpolation = FrameInterpolation::None,
          .sixteenBit = false },

        { .name = "Square",
          .relativePath = "xerum/Square.wav",
          .formula = "sign(x)",
          .description = "Basic shape: square. Odd harmonics only, falling as 1/k.",
          .frameCount = 1,
          .interpolation = FrameInterpolation::None,
          .sixteenBit = false },

        { .name = "Triangle",
          .relativePath = "xerum/Triangle.wav",
          .formula = "1 - 2*abs(x)",
          .description = "Basic shape: triangle. Odd harmonics falling as 1/k^2.",
          .frameCount = 1,
          .interpolation = FrameInterpolation::None,
          .sixteenBit = false },

        { .name = "Rectified",
          .relativePath = "xerum/Rectified.wav",
          .formula = "abs(sin(pi*x))",
          .description = "Full-wave rectified sine: an octave-up companion to Sine. Shipped as 16-bit "
                         "PCM, the format's other supported encoding, so the reader's 16-bit path is "
                         "exercised by our own content and not only by the factory corpus.",
          .frameCount = 1,
          .interpolation = FrameInterpolation::None,
          .sixteenBit = true },

        { .name = "FM Morph",
          .relativePath = "xerum/FM Morph.wav",
          .formula = "sin(pi*x + 8*y*sin(pi*x))",
          .description = "Phase-modulated sine; the modulation index rises from 0 to 8 across the frames.",
          .frameCount = 32,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Sync Sweep",
          .relativePath = "xerum/Sync Sweep.wav",
          .formula = "sin(2*pi*frac(w*(1 + 7*y)))",
          .description = "A sine read through a rising sync ratio, 1x to 8x across the frames.",
          .frameCount = 32,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "PWM Morph",
          .relativePath = "xerum/PWM Morph.wav",
          .formula = "(w < 0.05 + 0.9*y) ? 1 : -1",
          .description = "Pulse width sweeping 5% to 95% across the frames.",
          .frameCount = 32,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Fold Drive",
          .relativePath = "xerum/Fold Drive.wav",
          .formula = "tanh((1 + 12*y)*sin(pi*x))",
          .description = "Symmetric tanh folder; drive rises from 1x to 13x across the frames.",
          .frameCount = 32,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        // ------------------------------------------------------------------------------------------
        // The character tables. The five above are the shapes every synth has; these are the ones the
        // factory bank is built out of, and each is a *morph*: a single axis of change the preset's
        // table position reads. Written as formulas here rather than captured from anywhere, which is
        // what makes the bank ours (docs/content-policy.md).
        //
        // Two conventions, both so that a preset can be authored against them without measuring:
        //
        //   * frames are `FrameInterpolation::Linear`, so a fractional table position crossfades and a
        //     whole-numbered one picks a frame;
        //   * the last frame is the extreme of the sweep, and `Wavetable::interpolated` clamps a position
        //     at or beyond `frameCount - 1`, so a preset's position runs 1..frameCount-1.
        // ------------------------------------------------------------------------------------------

        { .name = "Harmonic Sweep",
          .relativePath = "xerum/Harmonic Sweep.wav",
          .formula = "sum(sin(pi*x), sin(2*pi*x)*min(1,2*y), sin(3*pi*x)*max(0,3*y-1), "
                     "sin(5*pi*x)*max(0,2*y-1), sin(8*pi*x)*max(0,2*y-1.5))*0.25",
          .description = "Additive: a pure sine whose second, third, fifth and eighth harmonics fade in "
                         "across the frames, so the position is a brightness control.",
          .frameCount = 24,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Formant Sweep",
          .relativePath = "xerum/Formant Sweep.wav",
          .formula = "(sin(pi*x)*exp(-pow((1-(1+13*y))/1.5,2)) + sin(2*pi*x)*exp(-pow((2-(1+13*y))/1.5,2)) "
                     "+ sin(3*pi*x)*exp(-pow((3-(1+13*y))/1.5,2)) + sin(4*pi*x)*exp(-pow((4-(1+13*y))/1.5,2)) "
                     "+ sin(5*pi*x)*exp(-pow((5-(1+13*y))/1.5,2)) + sin(6*pi*x)*exp(-pow((6-(1+13*y))/1.5,2)) "
                     "+ sin(7*pi*x)*exp(-pow((7-(1+13*y))/1.5,2)) + sin(8*pi*x)*exp(-pow((8-(1+13*y))/1.5,2)) "
                     "+ sin(9*pi*x)*exp(-pow((9-(1+13*y))/1.5,2)) + sin(10*pi*x)*exp(-pow((10-(1+13*y))/1.5,2)) "
                     "+ sin(11*pi*x)*exp(-pow((11-(1+13*y))/1.5,2)) + sin(12*pi*x)*exp(-pow((12-(1+13*y))/1.5,2)) "
                     "+ sin(13*pi*x)*exp(-pow((13-(1+13*y))/1.5,2)) + sin(14*pi*x)*exp(-pow((14-(1+13*y))/1.5,2)))"
                     "*0.5",
          .description = "A single resonant peak walked up the harmonic series from the first to the "
                         "fourteenth, which is what a vowel sweep is when it is done additively.",
          .frameCount = 24,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Formant High",
          .relativePath = "xerum/Formant High.wav",
          .formula = "(sin(pi*x)*exp(-pow((1-(5+6*y))/1.2,2)) + sin(2*pi*x)*exp(-pow((2-(5+6*y))/1.2,2)) "
                     "+ sin(3*pi*x)*exp(-pow((3-(5+6*y))/1.2,2)) + sin(4*pi*x)*exp(-pow((4-(5+6*y))/1.2,2)) "
                     "+ sin(5*pi*x)*exp(-pow((5-(5+6*y))/1.2,2)) + sin(6*pi*x)*exp(-pow((6-(5+6*y))/1.2,2)) "
                     "+ sin(7*pi*x)*exp(-pow((7-(5+6*y))/1.2,2)) + sin(8*pi*x)*exp(-pow((8-(5+6*y))/1.2,2)) "
                     "+ sin(9*pi*x)*exp(-pow((9-(5+6*y))/1.2,2)) + sin(10*pi*x)*exp(-pow((10-(5+6*y))/1.2,2)) "
                     "+ sin(11*pi*x)*exp(-pow((11-(5+6*y))/1.2,2)))*0.55",
          .description = "The same additive peak, but starting at the fifth harmonic and moving to the "
                         "eleventh: the bright half of the vowel sweep, on its own.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Vocal Stack",
          .relativePath = "xerum/Vocal Stack.wav",
          .formula = "(sin(pi*x)*(exp(-pow((1-2)/1,2)) + exp(-pow((1-(6+6*y))/1.3,2))) "
                     "+ sin(2*pi*x)*(exp(-pow((2-2)/1,2)) + exp(-pow((2-(6+6*y))/1.3,2))) "
                     "+ sin(3*pi*x)*(exp(-pow((3-2)/1,2)) + exp(-pow((3-(6+6*y))/1.3,2))) "
                     "+ sin(4*pi*x)*(exp(-pow((4-2)/1,2)) + exp(-pow((4-(6+6*y))/1.3,2))) "
                     "+ sin(5*pi*x)*(exp(-pow((5-2)/1,2)) + exp(-pow((5-(6+6*y))/1.3,2))) "
                     "+ sin(6*pi*x)*(exp(-pow((6-2)/1,2)) + exp(-pow((6-(6+6*y))/1.3,2))) "
                     "+ sin(7*pi*x)*(exp(-pow((7-2)/1,2)) + exp(-pow((7-(6+6*y))/1.3,2))) "
                     "+ sin(8*pi*x)*(exp(-pow((8-2)/1,2)) + exp(-pow((8-(6+6*y))/1.3,2))) "
                     "+ sin(9*pi*x)*(exp(-pow((9-2)/1,2)) + exp(-pow((9-(6+6*y))/1.3,2))) "
                     "+ sin(10*pi*x)*(exp(-pow((10-2)/1,2)) + exp(-pow((10-(6+6*y))/1.3,2))) "
                     "+ sin(11*pi*x)*(exp(-pow((11-2)/1,2)) + exp(-pow((11-(6+6*y))/1.3,2))) "
                     "+ sin(12*pi*x)*(exp(-pow((12-2)/1,2)) + exp(-pow((12-(6+6*y))/1.3,2))))*0.55",
          .description = "Two fixed formants: one held at the second harmonic, one walked from the sixth "
                         "to the twelfth. Choirs, not vowels.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Odd Stack",
          .relativePath = "xerum/Odd Stack.wav",
          .formula = "sum(sin(pi*x), sin(3*pi*x)*y*1.2, sin(5*pi*x)*y*0.8, sin(7*pi*x)*y*0.5, "
                     "sin(9*pi*x)*y*0.3)*0.6",
          .description = "Odd harmonics only, appearing with the frame: the hollow, clarinet-like end of "
                         "the additive family.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Saw Fade",
          .relativePath = "xerum/Saw Fade.wav",
          .formula = "(sin(pi*x) + sin(2*pi*x)*0.5 + sin(3*pi*x)*0.33 + sin(4*pi*x)*0.25 "
                     "+ sin(5*pi*x)*0.2 + sin(6*pi*x)*0.17 + sin(7*pi*x)*0.14 + sin(8*pi*x)*0.12)"
                     "*0.35*(1-y) + sin(pi*x)*y",
          .description = "An eight-harmonic saw fading into its own fundamental: the plainest morph in "
                         "the library, and the one a bass or a lead wants.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Square Fade",
          .relativePath = "xerum/Square Fade.wav",
          .formula = "sign(x)*(1-y) + sin(pi*x)*y",
          .description = "Square crossfaded into sine across the frames. Signed, so its middle frame is "
                         "quieter than either end — a morph that thins as it opens.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Bit Crush",
          .relativePath = "xerum/Bit Crush.wav",
          .formula = "round(x*(2+30*(1-y)))/(2+30*(1-y))",
          .description = "A ramp quantised onto fewer and fewer levels as the frame rises: 32 steps "
                         "down to 2, which is arithmetic rather than a bit-crusher effect.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Bend Sweep",
          .relativePath = "xerum/Bend Sweep.wav",
          .formula = "sin(pi*x*(1 + 5*y*(1 - x*x)))",
          .description = "A sine whose phase is warped about the centre, so the zero crossings crowd "
                         "together as the frame opens while the cycle stays seamless at the edges.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Ring Swell",
          .relativePath = "xerum/Ring Swell.wav",
          .formula = "sin(pi*x)*cos(pi*7*x*(0.3+y))",
          .description = "A sine ring-modulated by a quotient of itself and a rising multiplier: the "
                         "fundamental stays, and a clash of inharmonic partials grows around it.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Glass Stack",
          .relativePath = "xerum/Glass Stack.wav",
          .formula = "sum(sin(pi*x), sin(5*pi*x)*0.5*y, sin(11*pi*x)*0.35*y, sin(13*pi*x)*0.28*y, "
                     "sin(17*pi*x)*0.2*y)*0.5",
          .description = "The fundamental with 5th, 11th, 13th and 17th harmonics stacked on top: high, "
                         "thin and glassy at the top of its sweep.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Noise Blend",
          .relativePath = "xerum/Noise Blend.wav",
          .formula = "sin(pi*x)*(1-y) + (frac(sin(i*12.9898 + n*7.233)*43758.5453)*2 - 1)*y*0.8",
          .description = "A sine dissolving into deterministic noise — a hash of the sample index, not a "
                         "random number, so the table regenerates byte for byte.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Dust",
          .relativePath = "xerum/Dust.wav",
          .formula = "frac(sin(i*12.9898 + n*78.233)*43758.5453)*2 - 1",
          .description = "The same hash with nothing under it: eight frames of flat noise, for the presets "
                         "that want a texture rather than a tone.",
          .frameCount = 8,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Growl Fold",
          .relativePath = "xerum/Growl Fold.wav",
          .formula = "tanh((1 + 10*y)*x)*0.9",
          .description = "A ramp through a tanh saturator, driven from 1x to 11x: the saw-side companion "
                         "to Fold Drive, which does the same to a sine.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Pulse Grit",
          .relativePath = "xerum/Pulse Grit.wav",
          .formula = "((w < 0.5 - 0.45*y) ? 1 : -1)*0.9",
          .description = "A pulse narrowing from 50% to 5% across the frames — the other half of the "
                         "pulse-width family from PWM Morph, which runs 5% to 95%.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Sync Saw",
          .relativePath = "xerum/Sync Saw.wav",
          .formula = "2*frac(w*(1 + 13*y)) - 1",
          .description = "A saw read through a rising sync ratio, 1x to 14x. Sync Sweep does the same to "
                         "a sine, and the two are different sounds.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Metal Sync",
          .relativePath = "xerum/Metal Sync.wav",
          .formula = "sin(2*pi*frac(w*(2.5 + 9*y)))",
          .description = "The same sync read at **half-integer** ratios from 2.5 to 11.5, which puts the "
                         "cycle's partials between the harmonics instead of on them.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Sparkle",
          .relativePath = "xerum/Sparkle.wav",
          .formula = "sin(pi*x)*0.8*(1-y) + sin(19*pi*x)*y*0.5 + sin(23*pi*x)*y*0.3",
          .description = "A sine replaced by the 19th and 23rd harmonics as the frame rises: air, and "
                         "nothing else, at the top of its sweep.",
          .frameCount = 12,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false },

        { .name = "Fallback Shapes",
          .relativePath = "xerum/Fallback Shapes.wav",
          .formula = "(n < 1) ? sin(pi*x) : ((n < 2) ? (1 - 2*abs(x)) : ((n < 3) ? x : sign(x)))",
          .description = "The fallback: sine, triangle, saw and square, for a reference that cannot be resolved.",
          .frameCount = 4,
          .interpolation = FrameInterpolation::Linear,
          .sixteenBit = false,
          .fallback = true },
    };

    return library;
}

std::string_view fallbackTableReference()
{
    for (const auto& spec : starterLibrary())
        if (spec.fallback)
        {
            static const std::string reference = spec.relativePath;
            return reference;
        }

    return {};
}

bool buildTable (const TableSpec& spec, Wavetable& table, std::string& error)
{
    if (spec.frameCount == 0)
    {
        error = "frame count must be at least 1";
        return false;
    }

    const auto formula = Formula::fromText (spec.formula);

    if (! formula.valid())
    {
        error = "formula \"" + spec.formula + "\": " + formula.error();
        return false;
    }

    table = Wavetable {};
    table.frameSize = kDefaultFrameSize;
    table.frameCount = spec.frameCount;
    table.interpolation = spec.interpolation;
    table.factory = false;   // nothing xerum ships claims to be a factory table
    table.sixteenBit = spec.sixteenBit;
    table.sampleRate = spec.sampleRate;
    table.samples.resize (spec.frameCount * kDefaultFrameSize);

    const double size = static_cast<double> (kDefaultFrameSize);
    const double frames = static_cast<double> (spec.frameCount);

    for (std::size_t n = 0; n < spec.frameCount; ++n)
    {
        const double y = spec.frameCount > 1 ? static_cast<double> (n) / static_cast<double> (spec.frameCount - 1) : 0.0;

        FormulaContext context;
        context.n = static_cast<double> (n);
        context.y = y;
        context.z = 2.0 * y - 1.0;
        context.N = size;
        context.M = frames;

        for (std::size_t i = 0; i < kDefaultFrameSize; ++i)
        {
            context.i = static_cast<double> (i);
            context.w = static_cast<double> (i) / size;
            context.x = 2.0 * context.w - 1.0;

            table.samples[n * kDefaultFrameSize + i] = static_cast<float> (formula.evaluate (context));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------- encoding

EncodeOptions encodeOptionsFor (const TableSpec& spec)
{
    EncodeOptions options;
    options.sixteenBit = spec.sixteenBit;
    options.interpolation = spec.interpolation;
    options.spectralSubMode = spec.spectralSubMode;
    options.factory = false;
    options.sampleRate = spec.sampleRate;
    return options;
}

EncodeResult encodeWavetable (const Wavetable& table, const EncodeOptions& options)
{
    EncodeResult result;

    if (table.frameSize == 0)
    {
        result.error = "frame size is 0";
        return result;
    }

    if (table.frameCount == 0)
    {
        result.error = "frame count is 0";
        return result;
    }

    const std::size_t expected = table.frameSize * table.frameCount;

    if (table.samples.size() != expected)
    {
        result.error = "sample count " + std::to_string (table.samples.size())
                     + " does not match " + std::to_string (table.frameCount) + " frames of "
                     + std::to_string (table.frameSize);
        return result;
    }

    std::uint8_t mode = 0;

    switch (options.interpolation)
    {
        case FrameInterpolation::None:   mode = 0; break;
        case FrameInterpolation::Linear: mode = 1; break;
        case FrameInterpolation::SpectralMorph:
            mode = options.spectralSubMode;

            if (mode < 2 || mode > 4)
            {
                result.error = "spectral morph sub-mode must be 2, 3 or 4, not " + std::to_string (mode);
                return result;
            }
            break;

        case FrameInterpolation::Unknown:
            result.error = "cannot encode an unknown interpolation mode";
            return result;
    }

    const bool sixteenBit = options.sixteenBit;
    const std::uint16_t bitsPerSample = sixteenBit ? 16 : 32;
    const std::uint16_t formatTag = sixteenBit ? 1 : 3;
    const std::uint16_t blockAlign = static_cast<std::uint16_t> (bitsPerSample / 8);   // mono
    const std::uint32_t sampleRate = static_cast<std::uint32_t> (std::llround (options.sampleRate));
    const std::uint32_t byteRate = sampleRate * blockAlign;

    const std::size_t bytesPerSample = bitsPerSample / 8;
    const std::size_t dataSize = expected * bytesPerSample;

    const std::string clm = clmPayload (table.frameSize, mode, options.factory);
    const std::size_t clmSize = ((clm.size() + 1) / 2) * 2;   // chunks are word-aligned

    constexpr std::size_t fmtSize = 16;
    const std::size_t riffSize = 4 + (8 + fmtSize) + (8 + clmSize) + (8 + dataSize);

    auto& out = result.bytes;
    out.reserve (8 + riffSize);

    putTag (out, "RIFF");
    putU32 (out, static_cast<std::uint32_t> (riffSize));
    putTag (out, "WAVE");

    putTag (out, "fmt ");
    putU32 (out, fmtSize);
    putU16 (out, formatTag);
    putU16 (out, 1);            // mono
    putU32 (out, sampleRate);
    putU32 (out, byteRate);
    putU16 (out, blockAlign);
    putU16 (out, bitsPerSample);

    putTag (out, "clm ");
    putU32 (out, static_cast<std::uint32_t> (clmSize));
    for (const char c : clm)
        out.push_back (std::byte (c));
    for (std::size_t i = clm.size(); i < clmSize; ++i)
        out.push_back (std::byte (0));

    putTag (out, "data");
    // The true byte count — never the reference's inflated 32-bit-count-for-16-bit-data arithmetic.
    putU32 (out, static_cast<std::uint32_t> (dataSize));

    if (sixteenBit)
    {
        for (const float sample : table.samples)
        {
            const double clamped = std::min (1.0, std::max (-1.0, static_cast<double> (sample)));
            const auto value = static_cast<std::int16_t> (std::lround (clamped * 32767.0));
            putU16 (out, static_cast<std::uint16_t> (value));
        }
    }
    else
    {
        for (const float sample : table.samples)
            putFloat (out, sample);
    }

    result.ok = true;
    return result;
}

} // namespace xerum::dsp
