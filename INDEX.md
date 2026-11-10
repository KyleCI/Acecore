# Dependency index (paths)
- SoC/battery: lib/pc_core/src/soc_guard.h; src/power_guard.cpp/.h; lib/pc_core/src/local_auth.{h,cpp} (BatteryAlertPolicy)
- Per-socket config/shed: lib/pc_core/src/socket_rules.{h,cpp} (validShed); src/socket_cfg.{h,cpp}
- Auth/login/onboarding: src/web_v2.cpp (/v2/login, /v2/setup/*, route auth); src/onboarding.{h,cpp} (openSession, authorize); lib/pc_core/src/onb_core.{h,cpp} (PairStore, PBKDF2, kMaxPhones); src/web_local.cpp (credOf, Bearer/Basic); src/devauth.cpp
- NVS/settings/identity: src/settings.*; src/onboarding.cpp (savePairs); src/offline_queue.*; src/solar.cpp (solar NS); src/clock.cpp
- Build: platformio.ini; README.md; test/host/
