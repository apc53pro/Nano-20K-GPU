#include "sdram_model.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace nano_gpu::rtl {

namespace {
// Controller timing in core edges; the board and default profiles share them.
constexpr unsigned kTrp = 3, kTrcd = 3, kTrfc = 9, kTmrd = 3;
constexpr unsigned kTwr = 3, kTras = 6, kTrc = 9;
}

SdramPinsModel::SdramPinsModel() : words_(8u * 1024u * 1024u / 4, 0xcdcdcdcdu), defined_(words_.size(), 0) {}

void SdramPinsModel::require(bool condition, const char* reason) const {
    if (!condition) throw std::runtime_error(std::string("SDRAM pins at edge ") +
        std::to_string(statistics_.edges) + ": " + reason);
}

unsigned SdramPinsModel::word_address(unsigned bank, unsigned row, unsigned column, unsigned offset) const {
    const auto wrapped = (column & ~(burst_ - 1u)) | ((column + offset) & (burst_ - 1u));
    return (row << 10) | (bank << 8) | wrapped;
}

void SdramPinsModel::write_beat(const Pins& pins) {
    require(pins.dq_oe, "physical write burst has no DQ driver");
    require(!driving_, "FPGA and SDRAM drive DQ simultaneously");
    const auto address = word_address(write_.bank, write_.row, write_.column, write_.offset);
    for (unsigned lane = 0; lane < 4; ++lane) if ((pins.dqm & (1u << lane)) == 0) {
        const auto mask = 0xffu << (8 * lane);
        words_[address] = (words_[address] & ~mask) | (pins.dq_out & mask);
        defined_[address] |= static_cast<std::uint8_t>(1u << lane);
    }
    ++statistics_.write_words;
    banks_[write_.bank].precharge_after = std::max(banks_[write_.bank].precharge_after, statistics_.edges + kTwr);
    if (++write_.offset == write_.length) write_.active = false;
}

std::uint32_t SdramPinsModel::rising_edge(const Pins& pins) {
    ++statistics_.edges;
    if (!pins.cke) { driving_ = false; return dq_; }
    driving_ = false;
    if (!reads_.empty()) {
        const auto& read = reads_.front();
        if (statistics_.edges >= read.first && statistics_.edges < read.first + read.length) {
            const auto offset = static_cast<unsigned>(statistics_.edges - read.first);
            const auto address = word_address(read.bank, read.row, read.column, offset);
            dq_ = words_[address]; driving_ = true; ++statistics_.read_words;
            if (defined_[address] != 15) ++statistics_.undefined_read_words;
        }
        if (statistics_.edges + 1 >= read.first + read.length) reads_.pop_front();
    }
    require(!(driving_ && pins.dq_oe), "FPGA and SDRAM drive DQ simultaneously");
    const bool continuing_write = write_.active;
    if (continuing_write) write_beat(pins);
    if (pins.cs_n) return dq_;
    const unsigned command = (pins.ras_n ? 4u : 0u) | (pins.cas_n ? 2u : 0u) | (pins.we_n ? 1u : 0u);
    if (command == 7) return dq_; // NOP
    require(statistics_.edges >= blocked_until_, "command violates tRFC/tRP/tMRD");
    require(pins.bank < 4 && pins.address < 2048, "bank or address exceeds physical chip");
    auto& bank = banks_[pins.bank];
    switch (command) {
    case 3: // ACTIVE
        require(mode_set_, "ACTIVE before mode-register initialization");
        require(!bank.active && statistics_.edges >= bank.activate_after, "ACTIVE violates bank ownership/tRC/tRP");
        bank.active = true; bank.row = pins.address; bank.active_at = statistics_.edges;
        bank.column_after = statistics_.edges + kTrcd; bank.precharge_after = statistics_.edges + kTras;
        ++statistics_.activates;
        break;
    case 2: // PRECHARGE, address10 selects all banks.
        for (unsigned index = 0; index < 4; ++index) if ((pins.address & 1024u) || index == pins.bank) {
            auto& selected = banks_[index];
            require(!selected.active || statistics_.edges >= selected.precharge_after, "PRECHARGE violates tRAS/tWR");
            require(!(write_.active && write_.bank == index), "PRECHARGE during a write burst");
            for (const auto& read : reads_) require(read.bank != index, "PRECHARGE during a read burst");
            selected.active = false;
            selected.activate_after = std::max(statistics_.edges + kTrp, selected.active_at + kTrc);
        }
        blocked_until_ = statistics_.edges + kTrp; ++statistics_.precharges;
        break;
    case 1: // AUTO REFRESH
        require(!write_.active && reads_.empty(), "REFRESH during a data burst");
        for (const auto& selected : banks_) require(!selected.active, "REFRESH requires all banks precharged");
        if (last_refresh_) statistics_.max_refresh_gap = std::max(statistics_.max_refresh_gap, statistics_.edges - last_refresh_);
        last_refresh_ = statistics_.edges; blocked_until_ = statistics_.edges + kTrfc;
        ++statistics_.refreshes;
        break;
    case 0: // MODE REGISTER SET
        require(!write_.active && reads_.empty(), "mode change during a burst");
        for (const auto& selected : banks_) require(!selected.active, "mode change requires precharged banks");
        require((pins.address & 7u) == 3 && (pins.address & 8u) == 0, "expected sequential BL8 mode");
        cas_ = (pins.address >> 4) & 7u;
        require(cas_ == 2 || cas_ == 3, "unsupported CAS latency");
        require((pins.address & 512u) == 0, "single-location write mode is unsupported");
        burst_ = 8; mode_set_ = true; blocked_until_ = statistics_.edges + kTmrd;
        break;
    case 5: // READ
        require(mode_set_ && bank.active && statistics_.edges >= bank.column_after, "READ before ACTIVE/tRCD");
        require(!write_.active && !continuing_write, "READ overlaps a write burst");
        require((pins.address & 1024u) == 0, "auto-precharge is not selected by this controller profile");
        reads_.push_back({statistics_.edges + cas_, pins.bank, bank.row, pins.address & 255u, burst_});
        ++statistics_.read_commands;
        break;
    case 4: // WRITE: first DQ beat accompanies the command.
        require(mode_set_ && bank.active && statistics_.edges >= bank.column_after, "WRITE before ACTIVE/tRCD");
        require(!write_.active && !continuing_write && reads_.empty() && !driving_, "WRITE overlaps another burst");
        require((pins.address & 1024u) == 0, "auto-precharge is not selected by this controller profile");
        write_ = {true, pins.bank, bank.row, pins.address & 255u, 0, burst_};
        write_beat(pins); ++statistics_.write_commands;
        break;
    default:
        require(false, "unsupported SDRAM command");
    }
    return dq_;
}

std::uint32_t SdramPinsModel::inspect_word(std::uint32_t byte_address) const {
    require((byte_address & 3u) == 0 && byte_address < 8u * 1024u * 1024u, "diagnostic word address is invalid");
    return words_[byte_address / 4];
}

} // namespace nano_gpu::rtl
