#include <inverter.h>
#include <pinDefines.h>

#define MAX_MISSES 3

namespace {
  SemaphoreHandle_t inverterMutex;

  struct FieldF {
    float value = 0;
    uint8_t misses = 0;
    bool seenThisRecord = false;
  };

  struct FieldS {
    String value = "";
    uint8_t misses = 0;
    bool seenThisRecord = false;
  };

  FieldS sys, sw, chr;
  FieldF bat, grid, temp, vout, iout;

  float parseFloatValue(String value) {
    value.trim();
    value.replace("V", ""); value.replace("v", "");
    value.replace("A", ""); value.replace("a", "");
    value.replace("C", ""); value.replace("c", "");
    value.trim();
    return value.toFloat();
  }

  void finalizeRecord() {
    FieldF* floatFields[] = { &bat, &grid, &temp, &vout, &iout };
    for (FieldF* f : floatFields) {
      if (!f->seenThisRecord) {
        f->misses++;
        if (f->misses >= MAX_MISSES) f->value = 0;
      } else {
        f->misses = 0;
      }
      f->seenThisRecord = false;
    }

    FieldS* strFields[] = { &sw, &chr };
    for (FieldS* f : strFields) {
      if (!f->seenThisRecord) {
        f->misses++;
        if (f->misses >= MAX_MISSES) f->value = "";
      } else {
        f->misses = 0;
      }
      f->seenThisRecord = false;
    }
  }

  void parseInverterLine(String line) {
    line.trim();
    if (line.length() == 0) return;

    int sep = line.indexOf(':');
    if (sep == -1) return;

    String key = line.substring(0, sep);
    String value = line.substring(sep + 1);
    key.trim();
    value.trim();

    if (key.equalsIgnoreCase("SYS")) {
      finalizeRecord();
      sys.value = value;
    } else if (key.equalsIgnoreCase("SW")) {
      sw.value = value; sw.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("BAT")) {
      bat.value = parseFloatValue(value); bat.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("GRID")) {
      grid.value = parseFloatValue(value); grid.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("TEMP")) {
      temp.value = parseFloatValue(value); temp.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("CHR")) {
      chr.value = value; chr.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("VOUT")) {
      vout.value = parseFloatValue(value); vout.seenThisRecord = true;
    } else if (key.equalsIgnoreCase("IOUT")) {
      iout.value = parseFloatValue(value); iout.seenThisRecord = true;
    }
  }

  void parseInverterData(void* pvParameters) {
    String lineBuffer = "";

    for (;;) {
      while (INVERTER_UART.available()) {
        char c = INVERTER_UART.read();

        if (c == '\n') {
          if (xSemaphoreTake(inverterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            parseInverterLine(lineBuffer);
            xSemaphoreGive(inverterMutex);
          }
          lineBuffer = "";
        } else if (c != '\r') {
          lineBuffer += c;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
} // namespace

void initInverterParser(int rxPin, int txPin, long baud) {
  INVERTER_UART.begin(INVERTER_BAUD, SERIAL_8N1, INVERTER_RX, INVERTER_TX);
  inverterMutex = xSemaphoreCreateMutex();

  xTaskCreate(
    parseInverterData,
    "ParseInverterData",
    4096,
    NULL,
    1,
    NULL
  );
}

InverterSnapshot getInverterStatus() {
  InverterSnapshot snap;

  if (xSemaphoreTake(inverterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap.sys         = sys.value;
    snap.sw          = sw.value;
    snap.batVoltage  = bat.value;
    snap.gridVoltage = grid.value;
    snap.tempC       = temp.value;
    snap.chargeMode  = chr.value;
    snap.vout        = vout.value;
    snap.iout        = iout.value;
    xSemaphoreGive(inverterMutex);
  }

  return snap;
}