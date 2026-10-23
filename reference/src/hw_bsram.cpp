#include "nano_gpu/hw/bsram.hpp"

#include <stdexcept>

namespace nano_gpu::hw {
namespace {

void require_word_address(std::uint16_t address) {
    if (address >= Sdp1kx16::words) {
        throw std::out_of_range("SDPB 1024x16 word address exceeds 10 bits");
    }
}

} // namespace

void Sdp1kx16::tick(const BsramCycle& cycle) {
    if (cycle.write_enable) require_word_address(cycle.write_address);
    if (cycle.read_enable) require_word_address(cycle.read_address);
    if (cycle.write_enable && cycle.reset) {
        throw std::invalid_argument("SDPB write during reset is outside the selected wrapper contract");
    }
    if (cycle.write_enable && cycle.read_enable &&
        cycle.write_address == cycle.read_address) {
        throw std::invalid_argument("SDPB simultaneous same-address read/write is forbidden");
    }

    if (cycle.reset) {
        output_ = 0;
        output_known_ = true;
    } else if (cycle.read_enable) {
        output_ = contents_[cycle.read_address];
        output_known_ = true;
    }
    if (cycle.write_enable) contents_[cycle.write_address] = cycle.write_data;
}

std::uint16_t Sdp1kx16::output() const {
    if (!output_known_) {
        throw std::logic_error("SDPB output must be clocked by a read or reset before observation");
    }
    return output_;
}

std::uint16_t Sdp1kx16::write_bus_address(std::uint16_t word) {
    require_word_address(word);
    return static_cast<std::uint16_t>((word << 4) | 3u);
}

std::uint16_t Sdp1kx16::read_bus_address(std::uint16_t word) {
    require_word_address(word);
    return static_cast<std::uint16_t>(word << 4);
}

} // namespace nano_gpu::hw
