#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace photonstack {
enum class SerByteOrder { Compatibility, Specification, Little, Big };
struct SerSequenceInfo {
    std::uint32_t width=0,height=0,frames=0,colorID=0,depth=0,endianFlag=0;
    unsigned channels=0;
    bool littleEndian=true,timestamps=false;
    std::string bayer,observer,instrument,telescope;
    std::uint64_t localStartTicks=0,utcStartTicks=0,frameBytes=0;
};
struct SerFrame {ImageBuffer image;std::optional<std::uint64_t> utcTicks;};
class SerSequence {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    explicit SerSequence(const std::filesystem::path&,SerByteOrder order=SerByteOrder::Compatibility);
    ~SerSequence();
    SerSequence(const SerSequence&)=delete;
    const SerSequenceInfo& info()const;
    SerFrame read(std::uint32_t index);
};
std::string serTimestampISO(std::uint64_t ticks);
// New file only; preserves native counts, CFA phase and available UTC frame time.
void writeSerFrameFits(const SerFrame&,const SerSequenceInfo&,std::uint32_t index,const std::filesystem::path&);
}
