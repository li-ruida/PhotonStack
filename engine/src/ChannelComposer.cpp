#include "photonstack/ChannelComposer.hpp"
#include "photonstack/WorkerPool.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace photonstack {
namespace {
bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
}
bool ChannelExpression::validName(const std::string& name) {
    return !name.empty() && name.size() <= 32 && letter(name[0]) &&
        std::all_of(name.begin(), name.end(), [](char c) { return letter(c) || digit(c); }) &&
        name != "abs" && name != "sqrt" && name != "min" && name != "max" && name != "pow";
}
ChannelExpression::ChannelExpression(const std::string& source, const std::vector<std::string>& variables) {
    if (source.empty() || source.size() > 4096 || variables.empty() || variables.size() > 16)
        throw std::invalid_argument("Provide an expression of 1..4096 characters and 1..16 named masters");
    for (std::size_t i = 0; i < variables.size(); ++i)
        if (!validName(variables[i]) || std::find(variables.begin(), variables.begin() + i, variables[i]) != variables.begin() + i)
            throw std::invalid_argument("Invalid or duplicate master symbol");
    std::size_t pos = 0, depth = 0;
    const auto error = [&]() -> void { throw std::invalid_argument("Invalid channel expression at character " + std::to_string(pos + 1) + ": " + source); };
    const auto skip = [&] { while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t')) ++pos; };
    const auto take = [&](char c) { skip(); if (pos < source.size() && source[pos] == c) { ++pos; return true; } return false; };
    const auto emit = [&](Op op, double value = 0, std::size_t index = 0) {
        if (code_.size() >= 256) throw std::invalid_argument("Expression exceeds 256 instructions");
        code_.push_back({op, value, index});
    };
    std::function<void()> sum, product, atom;
    atom = [&] {
        if (++depth > 32) throw std::invalid_argument("Expression nesting exceeds 32 levels");
        skip();
        if (take('+')) atom();
        else if (take('-')) { atom(); emit(Op::Negate); }
        else if (take('(')) { sum(); if (!take(')')) error(); }
        else if (pos < source.size() && (digit(source[pos]) || source[pos] == '.')) {
            char* end = nullptr;
            const double value = std::strtod(source.c_str() + pos, &end);
            if (end == source.c_str() + pos || !std::isfinite(value)) error();
            pos = static_cast<std::size_t>(end - source.c_str()); emit(Op::Literal, value);
        } else if (pos < source.size() && letter(source[pos])) {
            const auto begin = pos++;
            while (pos < source.size() && (letter(source[pos]) || digit(source[pos]))) ++pos;
            const auto name = source.substr(begin, pos - begin);
            if (take('(')) {
                const bool unary = name == "abs" || name == "sqrt";
                if (!unary && name != "min" && name != "max" && name != "pow") error();
                sum();
                if (!unary) { if (!take(',')) error(); sum(); }
                if (!take(')')) error();
                emit(name == "abs" ? Op::Abs : name == "sqrt" ? Op::Sqrt : name == "min" ? Op::Min : name == "max" ? Op::Max : Op::Pow);
            } else {
                const auto found = std::find(variables.begin(), variables.end(), name);
                if (found == variables.end()) error();
                const auto index = static_cast<std::size_t>(found - variables.begin());
                emit(Op::Variable, 0, index); dependencies_.push_back(index);
            }
        } else error();
        --depth;
    };
    product = [&] { atom(); for (;;) { if (take('*')) { atom(); emit(Op::Multiply); } else if (take('/')) { atom(); emit(Op::Divide); } else break; } };
    sum = [&] { product(); for (;;) { if (take('+')) { product(); emit(Op::Add); } else if (take('-')) { product(); emit(Op::Subtract); } else break; } };
    sum(); skip(); if (pos != source.size()) error();
    std::sort(dependencies_.begin(), dependencies_.end());
    dependencies_.erase(std::unique(dependencies_.begin(), dependencies_.end()), dependencies_.end());
}
double ChannelExpression::evaluate(const double* values) const {
    std::array<double, 256> stack;
    std::size_t n = 0;
    for (const auto& instruction : code_) {
        const auto op = instruction.op;
        if (op == Op::Literal) { stack[n++] = instruction.literal; continue; }
        if (op == Op::Variable) { const auto v = values[instruction.index]; if (!std::isfinite(v)) return missing; stack[n++] = v; continue; }
        double result;
        if (op == Op::Negate || op == Op::Abs || op == Op::Sqrt) {
            const double a = stack[--n];
            result = op == Op::Negate ? -a : op == Op::Abs ? std::abs(a) : std::sqrt(a);
        } else {
            const double b = stack[--n], a = stack[--n];
            switch (op) {
            case Op::Add: result = a + b; break;
            case Op::Subtract: result = a - b; break;
            case Op::Multiply: result = a * b; break;
            case Op::Divide: result = b == 0 ? missing : a / b; break;
            case Op::Min: result = std::min(a, b); break;
            case Op::Max: result = std::max(a, b); break;
            case Op::Pow: result = std::pow(a, b); break;
            default: return missing;
            }
        }
        if (!std::isfinite(result)) return missing;
        stack[n++] = result;
    }
    return stack[0];
}
ChannelComposition composeChannels(const std::vector<ImageBuffer>& planes, const std::array<ChannelExpression, 3>& expressions, std::size_t workers) {
    if (planes.empty() || planes.size() > 16) throw std::invalid_argument("Choose 1..16 mono masters");
    for (const auto& p : planes)
        if (p.empty() || p.channels != 1 || p.width != planes[0].width || p.height != planes[0].height ||
            p.pixels.size() != p.pixelCount() || p.colorEncoding != ColorEncoding::Linear)
            throw std::invalid_argument("Masters must be linear mono images on the same grid");
    for (const auto& e : expressions) for (const auto i : e.dependencies())
        if (i >= planes.size()) throw std::invalid_argument("Expression references a missing master");
    ChannelComposition result;
    auto& rgb = result.image; auto& validity = result.validity;
    rgb.width = validity.width = planes[0].width; rgb.height = validity.height = planes[0].height;
    validity.channels = 1; validity.format = PixelFormat::Float32Gray;
    rgb.pixels.resize(rgb.sampleCount()); validity.pixels.resize(validity.pixelCount());
    result.workers = std::min<std::size_t>(rgb.height, workers ? std::clamp<std::size_t>(workers, 1, 64) :
        std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8));
    std::vector<std::size_t> invalid(result.workers, 0);
    detail::runWorkers(result.workers, [&](std::size_t worker) {
        std::array<double, 16> values{};
        const auto start = rgb.pixelCount() * worker / result.workers, end = rgb.pixelCount() * (worker + 1) / result.workers;
        for (std::size_t i = start; i < end; ++i) {
            for (std::size_t j = 0; j < planes.size(); ++j) values[j] = planes[j].pixels[i];
            bool valid = true;
            for (std::size_t c = 0; c < 3; ++c) {
                const auto value = expressions[c].evaluate(values.data());
                valid &= std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
                rgb.pixels[4*i+c] = valid ? static_cast<float>(value) : 0;
            }
            if (!valid) { ++invalid[worker]; rgb.pixels[4*i] = rgb.pixels[4*i+1] = rgb.pixels[4*i+2] = 0; }
            validity.pixels[i] = rgb.pixels[4*i+3] = valid ? 1 : 0;
        }
    });
    for (const auto count : invalid) result.invalidPixels += count;
    return result;
}
} // namespace photonstack
