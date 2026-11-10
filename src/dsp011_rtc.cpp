#include "dsp011_rtc.h"
#if POWERCELL_DSP011
#include "ds3231_time.h"
#include "i2c_bus.h"
#include "log.h"
#include "pins.h"
#include <Arduino.h>
#include <Wire.h>

namespace {
enum class RtcIo { Ok, Nack, Short, Busy };

RtcIo readFrame(uint8_t frame[16]) {
  if (!i2c_bus_lock(20)) return RtcIo::Busy;
  Wire.setClock(100000);
  Wire.beginTransmission(powercell::kDs3231Address);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    i2c_bus_unlock();
    return RtcIo::Nack;
  }
  const uint8_t got = Wire.requestFrom(static_cast<uint8_t>(powercell::kDs3231Address),
                                       static_cast<uint8_t>(powercell::kDs3231FrameBytes));
  if (got != powercell::kDs3231FrameBytes) {
    i2c_bus_unlock();
    return RtcIo::Short;
  }
  for (size_t i = 0; i < powercell::kDs3231FrameBytes; ++i) frame[i] = static_cast<uint8_t>(Wire.read());
  i2c_bus_unlock();
  return RtcIo::Ok;
}

bool writeRegs(uint8_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(powercell::kDs3231Address);
  Wire.write(reg);
  Wire.write(data, len);
  return Wire.endTransmission(true) == 0;
}

powercell::RtcReading readPair() {
  uint8_t first[16], second[16];
  const RtcIo a = readFrame(first);
  const RtcIo b = a == RtcIo::Ok ? readFrame(second) : a;
  powercell::RtcReading rtc;
  if (a == RtcIo::Ok && b == RtcIo::Ok) return powercell::ds3231_decode_pair(first, second);
  rtc.status = (a == RtcIo::Short || b == RtcIo::Short) ? powercell::RtcStatus::ShortRead
                                                        : powercell::RtcStatus::Incoherent;
  return rtc;
}
}  // namespace

bool hwRtcBegin(int64_t* unix_utc) {
  i2c_bus_init();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  const powercell::RtcReading rtc = readPair();
  if (rtc.status != powercell::RtcStatus::Unanchored) {
    const char* why = "rtc_invalid";
    if (rtc.status == powercell::RtcStatus::OscillatorStopped) why = "rtc_oscillator_stopped";
    else if (rtc.status == powercell::RtcStatus::ShortRead) why = "rtc_short";
    else if (rtc.status == powercell::RtcStatus::BadCalendar) why = "rtc_bad_calendar";
    else if (rtc.status == powercell::RtcStatus::BadBcd || rtc.status == powercell::RtcStatus::BadRegister) why = "rtc_bad_register";
    LOGW("RTC", "DS3231", "%s", why);
    return false;
  }
  if (unix_utc) *unix_utc = rtc.unix_utc;
  LOGI("RTC", "DS3231", "coherent unanchored epoch=%lld", (long long)rtc.unix_utc);
  return true;
}

bool hwRtcWrite(int64_t unix_utc) {
  uint8_t time_bytes[7], control = 0, status = 0;
  if (!powercell::ds3231_encode(unix_utc, time_bytes, &control, &status)) return false;
  if (!i2c_bus_lock(20)) return false;
  Wire.setClock(100000);
  const bool wrote = writeRegs(0x00, time_bytes, 7) && writeRegs(0x0E, &control, 1) &&
                     writeRegs(0x0F, &status, 1);
  i2c_bus_unlock();
  if (!wrote) return false;
  return powercell::ds3231_readback_matches(readPair(), unix_utc);
}
#endif
