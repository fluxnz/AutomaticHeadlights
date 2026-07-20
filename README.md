# Automatic Headlights (Arduino Nano)

Automatic headlight controller for an Arduino Nano using a BH1750 ambient light sensor, OLED status display, manual override switch, and relay output.

## Features

- Automatic relay control from ambient light (lux)
- Potentiometer-adjustable AUTO threshold (`10` to `200` lux)
- Hysteresis plus ON/OFF delay to prevent relay chatter
- Fail-safe behavior:
  - Relay stays OFF if the BH1750 sensor fails to initialize or returns an invalid reading
  - OLED display is skipped automatically if initialization fails
- Manual override with SPDT center-off switch:
  - Force ON
  - AUTO (center)
  - Force OFF
- OLED UI with day/night icon and live values:
  - Mode (`AUTO` / `FORCE ON` / `FORCE OFF`)
  - `Lux` (live measured light)
  - `Set` (potentiometer threshold)
  - `Lights` (`ON` / `OFF`)

## Hardware

- Arduino Nano V3 USB-C (ATmega328PB)
- BH1750 GY-302 I2C light sensor
- SSD1306 0.96" OLED 128x64 I2C display
- 10k linear potentiometer (B10K)
- SPDT center-off toggle switch (ON-OFF-ON)
- 5V 1-channel relay module (SRD-05VDC-SL-C)
- 470uF electrolytic capacitor
- 100nF ceramic capacitor

## Wiring

### I2C bus

- Nano `A4` -> BH1750 `SDA` and OLED `SDA`
- Nano `A5` -> BH1750 `SCL` and OLED `SCL`
- Nano `5V` -> sensor/display `VCC`
- Nano `GND` -> sensor/display `GND`

### Inputs and output

- Potentiometer wiper -> Nano `A0`
- Pot ends -> `5V` and `GND`
- Relay input -> Nano `D8`
- Toggle switch common -> `GND`
- Toggle throw 1 -> Nano `D2` (force ON)
- Toggle throw 2 -> Nano `D3` (force OFF)

The sketch uses `INPUT_PULLUP` for the switch pins, so active switch position pulls pin `LOW`.

## Capacitor placement (recommended)

- `100nF` ceramic close to Nano `5V/GND`
- `470uF` electrolytic across `5V/GND` rail to absorb relay/supply transients

## Arduino sketch location

- `Sketch/AutomaticHeadlights/AutomaticHeadlights.ino`

## Required libraries

Install these libraries in Arduino IDE / Arduino CLI:

- `BH1750`
- `Adafruit GFX Library`
- `Adafruit SSD1306`

## Build and upload

1. Open `Sketch/AutomaticHeadlights/AutomaticHeadlights.ino` in Arduino IDE.
2. Select board: **Arduino Nano**.
3. Select the correct processor/bootloader settings for your Nano clone/original.
4. Select the correct COM port.
5. Compile and upload.

## Runtime behavior

- AUTO mode maps potentiometer value (`0-1023`) to `10-200` lux setpoint.
- Relay switches ON only after sustained darkness (`AUTO_ON_DELAY_MS`).
- Relay switches OFF only after sustained brightness (`AUTO_OFF_DELAY_MS`).
- Hysteresis (`HYSTERESIS_LUX`) prevents flicker near threshold.
- If the BH1750 sensor is unavailable or returns a bad reading, the sketch forces the relay OFF and disables AUTO control until reset.
- If the OLED fails to initialize, the controller still runs without the display.

### Display example

AUTO  
Lux: 42  
Set: 60  
Lights: OFF

## Safety note

This project is intended to switch a relay control signal. If you are switching vehicle lighting power, use proper automotive wiring practices (fusing, suitable wire gauge, and protected enclosures).
