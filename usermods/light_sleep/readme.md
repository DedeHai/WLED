# Light Sleep usermod

This usermod uses wifi-sleep and light-sleep (only works on ESP32 C3, S2 and S3) to save power in off-mode. The sleep modes are enabled independently of the "disable wifi-sleep" setting. This is mainly useful to allow to disable wifi-sleep on the C3 while running (to avoid flickering caused by hardware limitations) but still saving power when the LEDs are off.

# Power Consumption

Current measured with a C3 supermini:
- Wifi-sleep disabled: ~100mA
- Wifi-sleep enabled: ~25mA
- Wifi-sleep + light-sleep: ~15mA

# Limitations

- Local time-keeping will not work correctly when using light-sleep
- ESP32 does not allow wifi to be used while in light-sleep with the method used in this usermod, however it will still enable wifi-sleep when the LEDs are off.
- Due to a hardware limitaion, USB connections cannot be maintained in light-sleep. It only works if your hardware uses a UART to USB chip.


## Usermod installation

Use `#define USERMOD_LIGHT_SLEEP` in wled.h or `-D USERMOD_LIGHT_SLEEP` in your platformio.ini.

## Change log
2025-02
* Initial version by @dedehai
