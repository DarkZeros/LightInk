#pragma once

#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <Arduino.h>
#include <RadioLib.h>

#include "hardware.h"

namespace Signal {
  struct BasicOOK {
    const float mFrequency;
    const uint32_t mBitMicros;
    const std::vector<uint8_t> mSequence;
    const uint8_t mRepetitions = 1;
    const uint32_t mDelayRepetitions = 0;
    void send() const;
  };
  struct FixedWidthPWM {
    const float mFrequency;
    const uint32_t mBitMicros;
    const std::vector<bool> mSequence, mPattern0, mPattern1;
    const uint8_t mRepetitions = 1;
    const uint32_t mDelayRepetitions = 0;
    void send() const;
  };
  using Sequence = std::variant<BasicOOK, FixedWidthPWM>;
  struct Group {
    std::string mName;
    std::vector<std::pair<std::string, Sequence>> mSequences;
  };
}
extern const std::vector<Signal::Group> kSignals;

std::vector<bool> split_bits(std::string_view bits);
#define SPLIT_BIT(x) split_bits(#x)

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
public:
  OOK(uint32_t bitUsDuration, float freq, int8_t minPower = -9, int8_t maxPower = 22);
  ~OOK();
  void transmit(std::vector<uint8_t> seq);
  void transmit(std::vector<bool> seq);
};

class PWM : public OOK {
  std::vector<bool> mPattern0, mPattern1;
public:
  PWM(std::vector<bool> pattern0, std::vector<bool> pattern1, float freq, int8_t minPower = -9, int8_t maxPower = 22);
  ~PWM();
  void transmit(std::vector<bool> seq);
};