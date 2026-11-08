#include "serial_spool.h"

#ifdef POWERCELL_HW_V011
#include <Arduino.h>
#include "driver/uart.h"

// Arduino Serial.print/printf/println on UART0 ends in uart_write_bytes.
// The real write still runs, so the live console is unchanged. The copy is
// only queued. Boot-ROM bytes and panic paths that use the ROM putc never
// enter this function.
static portMUX_TYPE tee_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" int __real_uart_write_bytes(uart_port_t uart_num, const char* src, size_t size);
extern "C" int __wrap_uart_write_bytes(uart_port_t uart_num, const char* src, size_t size) {
  if (uart_num == UART_NUM_0 && src && size && !powercell::console_spool().suppressed()) {
    portENTER_CRITICAL(&tee_mux);
    powercell::console_spool().ingest(src, size);
    portEXIT_CRITICAL(&tee_mux);
  }
  return __real_uart_write_bytes(uart_num, src, size);
}
#endif
