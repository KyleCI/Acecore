#include "sd_cs_config.h"

// Pulls the profile pin static_asserts into both PlatformIO environments.
namespace powercell {
int profile_check_anchor = POWERCELL_SD_CS;
}
