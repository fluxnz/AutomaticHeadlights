#include <Wire.h>
#include <BH1750.h>
#include <U8g2lib.h>

// -------- Hardware configuration --------
static const uint8_t PIN_RELAY = 8;
static const uint8_t PIN_POT = A0;
static const uint8_t PIN_SWITCH_FORCE_ON = 2;
static const uint8_t PIN_SWITCH_FORCE_OFF = 3;

// Most 5V relay modules are active LOW. Set false if your module is active HIGH.
static const bool RELAY_ACTIVE_LOW = true;

// OLED (128x64, I2C)
U8G2_SSD1306_128X64_NONAME_1_HW_I2C display(U8G2_R0);

BH1750 lightMeter;

enum ControlMode {
	MODE_AUTO,
	MODE_FORCE_ON,
	MODE_FORCE_OFF
};

float currentLux = 0.0f;
float filteredLux = 0.0f;
bool relayClosed = false;  // Closed = headlights ON.
bool displayReady = false;
bool sensorReady = false;

unsigned long lastSensorReadMs = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long darkCandidateStartMs = 0;
unsigned long brightCandidateStartMs = 0;

static const uint16_t SENSOR_INTERVAL_MS = 200;
static const uint16_t DISPLAY_INTERVAL_MS = 120;
static const uint16_t AUTO_ON_DELAY_MS = 2500;
static const uint16_t AUTO_OFF_DELAY_MS = 4000;

// Map potentiometer (0..1023) to an easy-to-tune AUTO trigger range.
// Left ~10 lx (very dark), center ~50-60 lx, right ~200 lx.
static const float THRESHOLD_MIN_LUX = 10.0f;
static const float THRESHOLD_MAX_LUX = 200.0f;

// Hysteresis band to avoid relay chatter around threshold.
static const float HYSTERESIS_LUX = 35.0f;

void writeRelay(bool closed) {
	relayClosed = closed;
	uint8_t pinState;
	if (RELAY_ACTIVE_LOW) {
		pinState = closed ? LOW : HIGH;
	} else {
		pinState = closed ? HIGH : LOW;
	}
	digitalWrite(PIN_RELAY, pinState);
}

ControlMode readModeFromSwitch() {
	// Switch expected wiring:
	// - Common to GND
	// - One throw to D2 (force ON), other throw to D3 (force OFF)
	// - Center OFF leaves both pins HIGH via INPUT_PULLUP
	bool forceOnActive = digitalRead(PIN_SWITCH_FORCE_ON) == LOW;
	bool forceOffActive = digitalRead(PIN_SWITCH_FORCE_OFF) == LOW;

	if (forceOnActive && !forceOffActive) {
		return MODE_FORCE_ON;
	}
	if (forceOffActive && !forceOnActive) {
		return MODE_FORCE_OFF;
	}
	return MODE_AUTO;
}

float readThresholdFromPot() {
	int pot = analogRead(PIN_POT);
	return THRESHOLD_MIN_LUX +
				 (THRESHOLD_MAX_LUX - THRESHOLD_MIN_LUX) * (static_cast<float>(pot) / 1023.0f);
}

void updateAutoRelayWithDelay(unsigned long now, float thresholdLux) {
	float onTriggerLux = thresholdLux - HYSTERESIS_LUX;
	float offTriggerLux = thresholdLux + HYSTERESIS_LUX;
	if (onTriggerLux < 0.0f) {
		onTriggerLux = 0.0f;
	}

	if (!relayClosed) {
		// Open relay (lights OFF): require sustained dark before turning ON.
		if (currentLux < onTriggerLux) {
			if (darkCandidateStartMs == 0) {
				darkCandidateStartMs = now;
			}
			if (now - darkCandidateStartMs >= AUTO_ON_DELAY_MS) {
				writeRelay(true);
				darkCandidateStartMs = 0;
			}
		} else {
			darkCandidateStartMs = 0;
		}
		brightCandidateStartMs = 0;
	} else {
		// Closed relay (lights ON): require sustained bright before turning OFF.
		if (currentLux > offTriggerLux) {
			if (brightCandidateStartMs == 0) {
				brightCandidateStartMs = now;
			}
			if (now - brightCandidateStartMs >= AUTO_OFF_DELAY_MS) {
				writeRelay(false);
				brightCandidateStartMs = 0;
			}
		} else {
			brightCandidateStartMs = 0;
		}
		darkCandidateStartMs = 0;
	}
}

void drawSunIcon(int16_t cx, int16_t cy) {
	display.setDrawColor(1);
	display.drawDisc(cx, cy, 7);
	display.setDrawColor(0);
	display.drawDisc(cx, cy, 4);
	display.setDrawColor(1);
	for (uint8_t i = 0; i < 8; i++) {
		float a = i * 0.785398f;  // 2*pi/8
		int16_t x0 = cx + static_cast<int16_t>(9 * cos(a));
		int16_t y0 = cy + static_cast<int16_t>(9 * sin(a));
		int16_t x1 = cx + static_cast<int16_t>(13 * cos(a));
		int16_t y1 = cy + static_cast<int16_t>(13 * sin(a));
		display.drawLine(x0, y0, x1, y1);
	}
}

void drawMoonIcon(int16_t cx, int16_t cy) {
	display.setDrawColor(1);
	display.drawDisc(cx, cy, 8);
	display.setDrawColor(0);
	display.drawDisc(cx + 4, cy - 2, 8);
	display.setDrawColor(1);
}

void drawUi(ControlMode mode, float thresholdLux) {
	display.firstPage();
	do {
		display.setDrawColor(1);
		display.setFont(u8g2_font_7x13B_tf);

		display.setCursor(0, 12);
		// Mode heading kept short so the setpoint and status are easy to read at a glance.
		if (mode == MODE_FORCE_ON) {
			display.println(F("FORCE ON"));
		} else if (mode == MODE_FORCE_OFF) {
			display.println(F("FORCE OFF"));
		} else {
			display.println(F("AUTO"));
		}
		display.drawHLine(0, 14, 128);

		display.setFont(u8g2_font_5x8_tf);
		display.setCursor(0, 24);
		display.print(F("LUX"));
		display.setCursor(0, 39);
		display.print(F("SET"));
		display.setCursor(0, 54);
		display.print(F("LIGHT"));

		display.setFont(u8g2_font_8x13B_tf);
		display.setCursor(26, 28);
		display.print(static_cast<int>(currentLux + 0.5f));
		display.setFont(u8g2_font_5x8_tf);
		display.print(F(" lx"));

		display.setFont(u8g2_font_8x13B_tf);
		display.setCursor(26, 43);
		display.print(static_cast<int>(thresholdLux + 0.5f));
		display.setFont(u8g2_font_5x8_tf);
		display.print(F(" lx"));

		display.setFont(u8g2_font_6x12_tf);
		if (relayClosed) {
			display.drawBox(26, 45, 28, 14);
			display.setDrawColor(0);
			display.setCursor(32, 56);
			display.print(F("ON"));
			display.setDrawColor(1);
		} else {
			display.drawFrame(26, 45, 32, 14);
			display.setCursor(30, 56);
			display.print(F("OFF"));
		}

		bool darkNow = currentLux < thresholdLux;
		if (darkNow) {
			drawMoonIcon(108, 24);
		} else {
			drawSunIcon(108, 24);
		}
	} while (display.nextPage());
}

void setup() {
	pinMode(PIN_RELAY, OUTPUT);
	pinMode(PIN_SWITCH_FORCE_ON, INPUT_PULLUP);
	pinMode(PIN_SWITCH_FORCE_OFF, INPUT_PULLUP);

	// Fail-safe default: relay open at startup.
	writeRelay(false);

	Wire.begin();
	sensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
	if (!sensorReady) {
		writeRelay(false);
	}

	// U8g2 does not provide a simple init-fail status; begin and continue.
	display.begin();
	displayReady = true;
	display.firstPage();
	do {
		display.setDrawColor(1);
		display.setFont(u8g2_font_7x13B_tf);
		int16_t titleX = (128 - display.getStrWidth("Auto Headlights")) / 2;
		display.setCursor(titleX, 24);
		display.println(F("Auto Headlights"));
		display.setFont(u8g2_font_6x12_tf);
		int16_t initX = (128 - display.getStrWidth("Init...")) / 2;
		display.setCursor(initX, 42);
		display.println(F("Init..."));
	} while (display.nextPage());
}

void loop() {
	unsigned long now = millis();

	if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
		lastSensorReadMs = now;

		if (sensorReady) {
			currentLux = lightMeter.readLightLevel();
			if (currentLux < 0.0f || isnan(currentLux)) {
				sensorReady = false;
				writeRelay(false);
				darkCandidateStartMs = 0;
				brightCandidateStartMs = 0;
				currentLux = 0.0f;
				filteredLux = 0.0f;
			} else {
				// Lightweight smoothing for stable display and control.
				filteredLux = (filteredLux * 0.75f) + (currentLux * 0.25f);
				currentLux = filteredLux;
			}
		} else {
			writeRelay(false);
			darkCandidateStartMs = 0;
			brightCandidateStartMs = 0;
		}
	}

	ControlMode mode = readModeFromSwitch();
	float thresholdLux = readThresholdFromPot();

	if (!sensorReady) {
		writeRelay(false);
		darkCandidateStartMs = 0;
		brightCandidateStartMs = 0;
	} else if (mode == MODE_FORCE_ON) {
		writeRelay(true);
		darkCandidateStartMs = 0;
		brightCandidateStartMs = 0;
	} else if (mode == MODE_FORCE_OFF) {
		writeRelay(false);
		darkCandidateStartMs = 0;
		brightCandidateStartMs = 0;
	} else {
		updateAutoRelayWithDelay(now, thresholdLux);
	}

	if (displayReady && (now - lastDisplayRefreshMs >= DISPLAY_INTERVAL_MS)) {
		lastDisplayRefreshMs = now;
		drawUi(mode, thresholdLux);
	}
}
