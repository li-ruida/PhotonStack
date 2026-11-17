#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <string>
#include <vector>

namespace photonstack {
// Small, bounded arithmetic language. No scripts, files, globals or side effects.
class ChannelExpression {
  public:
    ChannelExpression(const std::string& source, const std::vector<std::string>& variables);
    double evaluate(const double* values) const;
    const std::vector<std::size_t>& dependencies() const { return dependencies_; }
    static bool validName(const std::string& name);
  private:
    enum class Op { Literal, Variable, Negate, Add, Subtract, Multiply, Divide, Abs, Sqrt, Min, Max, Pow };
    struct Instruction { Op op; double literal = 0; std::size_t index = 0; };
    std::vector<Instruction> code_;
    std::vector<std::size_t> dependencies_;
};
struct ChannelComposition {
    ImageBuffer image, validity;
    std::size_t invalidPixels = 0, workers = 0;
};
// Mono planes must already share a pixel grid and photometric units.
ChannelComposition composeChannels(const std::vector<ImageBuffer>& planes,
                                   const std::array<ChannelExpression, 3>& expressions,
                                   std::size_t workers = 0);
} // namespace photonstack
