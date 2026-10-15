#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "radio.h"
#include "power.h"

namespace {
  RTC_DATA_ATTR std::optional<SPISettings> kSpi;
  RTC_DATA_ATTR std::optional<ArduinoHal> kHal;
  RTC_DATA_ATTR std::optional<Module> kModule;
  RTC_DATA_ATTR std::optional<SX1262> kRadio;
}

OOK::OOK(uint32_t bitUsDuration, float freq, int8_t minPower, int8_t maxPower)
: mBitUsDuration(bitUsDuration)
, mMinPower(minPower)
, mMaxPower(maxPower)
{
  if (!kRadio)
    return;
  if (int16_t err = kRadio->beginFSK(freq, 4.8f, 5.f, 156.2f, mMinPower)) {
    ESP_LOGE("Radio", "OOK error: %d", err);
  }
  // kRadio->setFrequency(freq); // Initially set to correct freq already
  kRadio->transmitDirect();
  // kRadio->setOutputPower(mMinPower);  // Initially set to low
}
OOK::~OOK() {
  Radio::sleep();
}

void Signal::BasicOOK::send() const {
  // auto ook = OOK(mBitMicros, mFrequency, -9, 22); // Custom power transmission
  auto ook = OOK(mBitMicros, mFrequency);
  for(auto i=0; i<mRepetitions; i++) {
    ook.transmit(mSequence);
    delayMicroseconds(mDelayRepetitions);
  }
}
void Signal::FixedWidthPWM::send() const {
  // auto ook = OOK(mBitMicros, mFrequency, -9, 22); // Custom power transmission
  auto ook = OOK(mBitMicros, mFrequency);
  for(auto i=0; i<mRepetitions; i++) {
    ook.transmit(mSequence, mPattern0, mPattern1);
    delayMicroseconds(mDelayRepetitions);
  }
}

int16_t OOK::setOutputPowerFast(int8_t power) {
  if (auto state = kRadio->checkOutputPower(power, NULL))
    return state;
  const uint8_t data[] = { static_cast<uint8_t>(power), RADIOLIB_SX126X_PA_RAMP_10U };
  return kModule->SPIwriteStream(RADIOLIB_SX126X_CMD_SET_TX_PARAMS, data, 2);
}

template <typename F>
void OOK::transmitBits(F&& forEachBit) {
  if (!kRadio)
    return;
  RadioLibTime_t start = kHal->micros();
  std::optional<bool> lastPower;
  forEachBit([&](bool bit) {
    if (bit != lastPower) {
      setOutputPowerFast(bit ? mMaxPower : mMinPower);
      lastPower = bit;
    }
    kModule->waitForMicroseconds(start, mBitUsDuration);
    start += mBitUsDuration;
  });
  kRadio->setOutputPower(mMinPower);
}
void OOK::transmit(std::span<const uint8_t> seq) {
  transmitBits([&](auto emit) {
    for (uint8_t byte : seq)
      for (int bitIdx = 7; bitIdx >= 0; --bitIdx)
        emit((byte >> bitIdx) & 0x01);
  });
}
void OOK::transmit(Signal::Bits seq) {
  transmitBits([&](auto emit) {
    for (uint8_t i = 0; i < seq.mSize; i++)
      emit(seq[i]);
  });
}
void OOK::transmit(Signal::Bits seq, Signal::Bits pattern0, Signal::Bits pattern1) {
  transmitBits([&](auto emit) {
    for (uint8_t i = 0; i < seq.mSize; i++) {
      auto pat = seq[i] ? pattern1 : pattern0;
      for (uint8_t j = 0; j < pat.mSize; j++)
        emit(pat[j]);
    }
  });
}


Radio::Radio() {
  if constexpr (!HW::kHasLora) {
    return;
  }
  if (!kRadio) {
    // Construct the RadioLib objects just once in RTC mem
    kSpi.emplace(10'000'000, MSBFIRST, SPI_MODE0);
    kHal.emplace(SPI, *kSpi);
    kModule.emplace(&kHal.value(), HW::Lora::Cs, HW::Lora::Dio1, HW::Lora::Res, HW::Lora::Busy);
    kRadio.emplace(&kModule.value());
    // kRadio->XTAL = false; // It is false in the module used
    kRadio->begin(434.0, 125.0, 9, 7, 0x12, 10, 8, 0, false);
    sleep();
  } else {
    kHal->pinMode(kModule->getIrq(), kHal->GpioModeInput);
    kHal->pinMode(kModule->getGpio(), kHal->GpioModeInput);
    kModule->init();
  }
  Power::hold((gpio_num_t)HW::Lora::Cs, false);
}

void Radio::startReceive()
{
  if (!kRadio)
    return;

  // ESP_LOGE("lora", "listening");

  // If we did a deep sleep, we need to call again begin() to reset the module
  kRadio->begin(434.0, 125.0, 9, 7, 0x12, 10, 8, 0, false);

  kRadio->startReceive(); // 4.65mA @ 3.3V
}

void Radio::sendSignal(const Signal::Sequence& seq) {
  std::visit([&](auto& e) {
    ESP_LOGD("", "Sending signal");
    e.send();
    ESP_LOGD("", "Sending signal done");
  }, seq);
}

void Radio::sleep()
{
  if (!kRadio)
    return;

  kRadio->sleep(false); // ~0.3 uA @ 3.3V
  // kRadio->sleep(); // ~0.7 uA @3.3V
}

void Radio::readLoraPck() {
  if (!kRadio)
    return;

  String data;
  auto status = kRadio->readData(data);
  if (status != RADIOLIB_ERR_NONE) {
    ESP_LOGE("Radio", "receive error: %d", status);
  } else {
    // Fill the rest of parameters
    mPck = Radio::Pck{.mData = data.c_str(),
                     .mSNR = kRadio->getSNR(),
                     .mRSSI = kRadio->getRSSI(),
                     .mFreqError = kRadio->getFrequencyError()};
  }
  sleep();
}