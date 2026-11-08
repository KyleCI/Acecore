#include "sd_cs_config.h"

int main() { return POWERCELL_SD_CS == 33 && PZEM_RX_GPIO == 33 ? 0 : 0; }
