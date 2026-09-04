# M5Paper Monitor

Embedded project for the M5Stack M5Paper (original, ESP32-D0WDQ6, 
16MB flash, 8MB PSRAM, 4.7" e-ink 960x540, IT8951 controller).

- Toolchain: PlatformIO (pio CLI), framework = arduino, board = m5stack-fire 
  or a custom m5paper env
- Display library: M5EPD
- The device is connected over USB-C; find the serial port before flashing
- Always run `pio run` to verify the build compiles before uploading
- Use `pio device monitor -b 115200` to check serial debug output
- E-ink rule: prefer UPDATE_MODE_GC16 for full refresh, UPDATE_MODE_DU4 
  for fast partial updates; avoid refreshing more than ~1x/sec