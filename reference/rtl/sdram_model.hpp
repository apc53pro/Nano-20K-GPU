#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace nano_gpu::rtl {

// External SDRAM chip, not GPU storage or a device-side memory access path.
// The RTL can change memory only with physical SDRAM commands and DQ/DQM.
class SdramPinsModel {
public:
    struct Pins {
        bool cke = false, cs_n = true, ras_n = true, cas_n = true, we_n = true;
        unsigned bank = 0, address = 0;
        bool dq_oe = false;
        std::uint32_t dq_out = 0;
        unsigned dqm = 15;
    };
    struct Statistics {
        std::uint64_t edges = 0, activates = 0, precharges = 0, refreshes = 0;
        std::uint64_t read_commands = 0, write_commands = 0, read_words = 0, write_words = 0;
        std::uint64_t max_refresh_gap = 0, undefined_read_words = 0;
    };
    SdramPinsModel();
    // Sample OLD pin outputs at a rising SDRAM edge. Returned DQ becomes
    // established after the edge, for capture by the RTL on its following edge.
    std::uint32_t rising_edge(const Pins& pins);
    std::uint32_t inspect_word(std::uint32_t byte_address) const;
    bool driving() const { return driving_; }
    const Statistics& statistics() const { return statistics_; }

private:
    struct Bank {
        bool active = false;
        unsigned row = 0;
        std::uint64_t active_at = 0, column_after = 0, activate_after = 0, precharge_after = 0;
    };
    struct Read {
        std::uint64_t first;
        unsigned bank, row, column, length;
    };
    struct Write {
        bool active = false;
        unsigned bank = 0, row = 0, column = 0, offset = 0, length = 8;
    } write_;
    std::vector<std::uint32_t> words_;
    std::vector<std::uint8_t> defined_;
    std::array<Bank, 4> banks_{};
    std::deque<Read> reads_;
    Statistics statistics_{};
    std::uint64_t blocked_until_ = 0, last_refresh_ = 0;
    unsigned cas_ = 3, burst_ = 8;
    bool mode_set_ = false, driving_ = false;
    std::uint32_t dq_ = 0xcdcdcdcdu;
    unsigned word_address(unsigned bank, unsigned row, unsigned column, unsigned offset) const;
    void write_beat(const Pins& pins);
    void require(bool condition, const char* reason) const;
};

} // namespace nano_gpu::rtl
