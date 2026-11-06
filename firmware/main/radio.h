#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <variant>

#include <Arduino.h>
#include <RadioLib.h>

#include "hardware.h"

namespace Signal {
  // Up to 64 bits packed MSB first, built at compile time with the _bits literal
  struct Bits {
    uint64_t mValue = 0;
    uint8_t mSize = 0;
    constexpr bool operator[](uint8_t i) const { return (mValue >> (mSize - 1 - i)) & 1; }
  };
  namespace literals {
    // Not constexpr: calling it from the consteval literal is a compile error
    void invalid_bits_literal();
    // 0001'1110_bits -> Bits{0b00011110, 8}. Leading zeros are kept, ' separators skipped
    template <char... C>
    consteval Bits operator""_bits() {
      Bits b;
      for (char c : {C...}) {
        if (c == '\'')
          continue;
        if ((c != '0' && c != '1') || b.mSize == 64)
          invalid_bits_literal();
        b.mValue = (b.mValue << 1) | (c == '1');
        b.mSize++;
      }
      return b;
    }
  }

  struct BasicOOK {
    const float mFrequency;
    const uint32_t mBitMicros;
    const std::span<const uint8_t> mSequence;
    const uint8_t mRepetitions = 1;
    const uint32_t mDelayRepetitions = 0;
    void send() const;
  };
  struct FixedWidthPWM {
    const float mFrequency;
    const uint32_t mBitMicros;
    const Bits mSequence, mPattern0, mPattern1;
    const uint8_t mRepetitions = 1;
    const uint32_t mDelayRepetitions = 0;
    void send() const;
  };
  using Sequence = std::variant<BasicOOK, FixedWidthPWM>;
  struct Entry {
    const char* mName;
    Sequence mSequence;
  };
  struct Group {
    const char* mName;
    std::span<const Entry> mSequences;
  };
}
// constinit in secrets.cpp: lives in flash, no construction at boot
extern const std::span<const Signal::Group> kSignals;

/* Helper class to use the Radio HW module 
 */
class Radio {
public:
  struct Pck {
    std::string mData;
    float mSNR;
    float mRSSI;
    float mFreqError;
  };

  std::optional<Pck> mPck;

  Radio();

  static void startReceive();
  static void sleep();
  static void sendSignal(const Signal::Sequence& seq);

  bool sendLoraPck(const std::string& pck);
  void readLoraPck();
};

class OOK {
  uint32_t mBitUsDuration;
  int8_t mMinPower, mMaxPower;
  int16_t setOutputPowerFast(int8_t power);
  // Calls forEachBit(emit) and keys the carrier for every bit it emits
  template <typename F> void transmitBits(F&& forEachBit);
public:
  OOK(uint32_t bitUsDuration, float freq, int8_t minPower = -9, int8_t maxPower = 22);
  ~OOK();
  void transmit(std::span<const uint8_t> seq);
  void transmit(Signal::Bits seq);
  // Each bit of seq is sent as pattern1 (1) or pattern0 (0)
  void transmit(Signal::Bits seq, Signal::Bits pattern0, Signal::Bits pattern1);
};