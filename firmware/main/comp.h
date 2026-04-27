#include <array>
#include <cstddef>
#include <cstdint>

// Example "compression": trivial 1B RLE-like
template <auto& input, std::size_t MaxSize = 1024>
struct RLE1B {
    static constexpr auto compute() {
        std::array<uint8_t, MaxSize> out{};
        std::size_t outSize = 0;

        std::size_t i = 0;
        while (i < input.size()) {
            uint8_t value = input[i];
            std::size_t count = 1;

            while (i + count < input.size() &&
                   input[i + count] == value &&
                   count < 255) {
                ++count;
            }

            out[outSize++] = value;
            out[outSize++] = static_cast<uint8_t>(count);

            i += count;
        }

        return std::pair{out, outSize};
    }

    static constexpr auto result = compute();

    // Static storage (safe!)
    static constexpr auto& array = result.first;
    static constexpr std::size_t size = result.second;

    static constexpr std::span<const uint8_t> view{
        array.data(), size
    };
};

struct BitWriter {
    std::array<uint8_t, 1024> out{};
    std::size_t bit_pos = 0;

    constexpr void write_bit(bool b) {
        std::size_t byte = bit_pos / 8;
        std::size_t off  = 7 - (bit_pos % 8);

        if (b) out[byte] |= (1u << off);
        ++bit_pos;
    }

    constexpr void write_bits(uint32_t v, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) {
            write_bit((v >> (n - 1 - i)) & 1);
        }
    }

    constexpr std::size_t size_bytes() const {
        return (bit_pos + 7) / 8;
    }
};

// -----------------------------------------------------------------------------

struct BitReader {
    std::span<const uint8_t> in;
    std::size_t bit_pos = 0;

    constexpr bool read_bit() {
        std::size_t byte = bit_pos / 8;
        std::size_t off  = 7 - (bit_pos % 8);

        bool b = (in[byte] >> off) & 1;
        ++bit_pos;
        return b;
    }

    constexpr uint32_t read_bits(std::size_t n) {
        uint32_t v = 0;
        for (std::size_t i = 0; i < n; i++) {
            v = (v << 1) | read_bit();
        }
        return v;
    }
};

template <auto& input, std::size_t MaxSize = 1024>
struct RLETestA {
    // There are 4 difrent packets:
    // 00xx xxxx : The 6 bits are written to output as is
    // 01xx xyyy : X and Y are how many of those consecutive bits to write X=0, Y=1
    // 10yy yxxx : Same as aove but X and Y are swapped
    // 11ab bbbb : Long write a single bit a b times

    static constexpr auto compute() {
        BitWriter w{};

        std::size_t i = 0;
        while (i < input.size()) {

            uint8_t v = input[i];

            std::size_t count = 1;
            while (i + count < input.size() && input[i + count] == v && count < 31) {
                ++count;
            }

            // ---------------------------------------------------------
            // LONG RUN: 11abbbbb
            // ---------------------------------------------------------
            if (count > 7) {
                w.write_bits(0b11, 2);
                w.write_bit(v);
                w.write_bits(count, 5);
            }

            // ---------------------------------------------------------
            // SHORT RUN: 01xxxyyy or 10yyyxxx
            // ---------------------------------------------------------
            else if (count > 1) {

                if (v == 1) {
                    // 01xxxyyy
                    w.write_bits(0b01, 2);
                    w.write_bits(count, 3); // X = ones
                    w.write_bits(0, 3);     // Y = zeros
                } else {
                    // 10yyyxxx
                    w.write_bits(0b10, 2);
                    w.write_bits(count, 3); // Y = zeros
                    w.write_bits(0, 3);     // X = ones
                }
            }

            // ---------------------------------------------------------
            // LITERAL: 00xxxxxx
            // ---------------------------------------------------------
            else {
                w.write_bits(0b00, 2);
                w.write_bits(v, 6);
            }

            i += count;
        }

        return std::pair{w.out, w.size_bytes()};
    }

    static constexpr auto result = compute();

    // Static storage (safe!)
    static constexpr auto& array = result.first;
    static constexpr std::size_t size = result.second;

    static constexpr std::span<const uint8_t> view{
        array.data(), size
    };
};