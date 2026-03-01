#include <Arduino.h>
#include <HardwareSerial.h>
#include <string>

#include "gps.h"
#include "power.h"

#include "driver/rtc_io.h"
#include "esp32-hal-log.h"

RTC_DATA_ATTR Gps::Data Gps::mData {};

void Gps::set(bool high) const {
  if constexpr (!HW::kHasGps) {
    high = false;
  }
  if (high) {
    Power::lock(Power::Flag::Gps);
  } else {
    Power::unlock(Power::Flag::Gps);
    mData.mTimeOn.reset();
  }
  if (HW::Gps::Vcc == (uint8_t)-1)
    return;
  rtc_gpio_hold_dis((gpio_num_t)HW::Gps::Vcc);
  pinMode(HW::Gps::Vcc, OUTPUT);
  digitalWrite(HW::Gps::Vcc, !high);
  rtc_gpio_hold_en((gpio_num_t)HW::Gps::Vcc);
}

bool Gps::isOn() const {
  return Power::status() & Power::Flag::Gps;
}

bool Gps::read(uint32_t timeout) const {
  if constexpr (!HW::kHasGps) {
    return false;
  }
  if (!isOn()) {
    return false;
  }

  // Initialize HW serial to receive data
  HardwareSerial mSerial{2};
  mSerial.begin(HW::Gps::BaudRate, SERIAL_8N1, HW::Gps::Rx, -1/*, HW::Gps::Tx*/, false, timeout);
  uint32_t maxWait = millis() + timeout;

  // If there is data in the receive buffer of the hardware serial
  // Discard all data until the $ sign
  while (!mSerial.readStringUntil('$').isEmpty()) {
    if (millis() > maxWait)
      return false;
    delay(1); // Feed the wdt
    // Read a packet
    auto packet = mSerial.readStringUntil('*');
    // ESP_LOGE("gps", "%s", packet.c_str());
    // Early exit
    if (packet.substring(2,5) != "RMC") 
      continue;

    // ESP_LOGE("gps", "Processing: %s", packet.c_str());
    // Read CRC
    char crcBytes[2];
    mSerial.readBytes(crcBytes, 2);
    uint8_t crc = 16 * (crcBytes[0] - (crcBytes[0] >= 'A' ? 'A'-10 : '0'))
                      +(crcBytes[1] - (crcBytes[1] >= 'A' ? 'A'-10 : '0'));
    // Calculate CRC
    auto crc2 = 0;
    for(auto i=0; i<packet.length(); i++)
      crc2 ^= packet[i];

    if (crc != crc2){
      ESP_LOGE("GPS", "CRC error");
      // ESP_LOGE("GPS", "Packet: %s, CRC:%d, CRC2:%d", packet.c_str(), crc, crc2);
      continue;
    }

    // CHECK: https://www.qso.com.ar/datasheets/Receptores%20GNSS-GPS/NMEA_Format_v0.1.pdf
      
    // Parse fields of packet
    std::string_view sv = std::string_view{packet.begin(), packet.end()};
    std::vector<std::string> fields;
    for(size_t end; (end = sv.find_first_of(',')) != std::string::npos;) {
      fields.emplace_back(sv.substr(0, end));
      sv.remove_prefix(end + 1);
    }
    fields.emplace_back(sv);

    if (fields.size() != 14) {
      ESP_LOGE("GPS", "RMC wrong fields %d %s", fields.size(), std::string(sv).c_str());
      continue;
    }

    // Time / Date
    mData.mDateTime.reset();
    if (fields[1].size() == 9 && fields[9].size() == 6) {
      tmElements_t dt;
      dt.Hour = std::stoi(fields[1].substr(0, 2));
      dt.Minute = std::stoi(fields[1].substr(2, 2));
      dt.Second = std::stoi(fields[1].substr(4, 2));
      auto centiSeconds = std::stoi(fields[1].substr(7, 2));
      dt.Day = std::stoi(fields[9].substr(0, 2));
      dt.Month = std::stoi(fields[9].substr(2, 2));
      dt.Year = 30 + std::stoi(fields[9].substr(4, 2));
      mData.mDateTime.emplace(dt, centiSeconds);
    }

    // Speed
    mData.mSpeed.reset();
    if (!fields[7].empty()) {
      mData.mSpeed.emplace(std::stof(fields[7]) * 1.852f);
    }

    // Direction
    mData.mDirection.reset();
    if (!fields[8].empty()) {
      mData.mDirection.emplace(std::stof(fields[8]));
    }

    // Location
    mData.mLocation.reset();
    if (fields[13][0] != 'N' && fields[3].size() == 10 && fields[4].size() == 1 &&
        fields[5].size() == 11 && fields[6].size() == 1) {
      Data::Location loc;
      loc.mLat = std::stoul(fields[3].substr(0,2)) 
               + std::stod(fields[3].substr(2)) / 60;
      loc.mLat *= fields[4][0] == 'N' ? 1 : -1;
      loc.mLon = std::stoul(fields[5].substr(0,3)) 
               + std::stod(fields[5].substr(3)) / 60;
      loc.mLon *= fields[6][0] == 'E' ? 1 : -1;
      mData.mLocation.emplace(loc);
    }
    return true;
  }
  return false;
}
