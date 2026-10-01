#include <Wire.h>
#include <BH1750.h>
#include <U8g2lib.h>

// -------- Hardware configuration --------
static const uint8_t PIN_RELAY = 8;
static const uint8_t PIN_POT = A0;
static const uint8_t PIN_SWITCH_FORCE_ON = 2;
static const uint8_t PIN_SWITCH_FORCE_OFF = 3;
static const uint8_t PIN_HANDBRAKE = 4;

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
bool hasLuxSample = false;

unsigned long lastSensorReadMs = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long darkCandidateStartMs = 0;
unsigned long brightCandidateStartMs = 0;
unsigned long lastSensorReinitAttemptMs = 0;
unsigned long lastI2cRecoveryMs = 0;
uint8_t consecutiveSensorErrors = 0;

static const uint16_t SENSOR_INTERVAL_MS = 200;
static const uint16_t DISPLAY_INTERVAL_MS = 120;
static const uint16_t AUTO_ON_DELAY_MS = 2500;
static const uint16_t AUTO_OFF_DELAY_MS = 4000;
static const uint16_t SENSOR_REINIT_INTERVAL_MS = 1500;
static const uint16_t I2C_RECOVERY_INTERVAL_MS = 10000;
static const uint8_t MAX_SENSOR_ERRORS_BEFORE_FAULT = 5;

// Map potentiometer (0..1023) to an easy-to-tune AUTO trigger range.
// Left 0 lx (very dark), center ~200 lx, right 400 lx.
static const float THRESHOLD_MIN_LUX = 0.0f;
static const float THRESHOLD_MAX_LUX = 400.0f;

// Hysteresis band to avoid relay chatter around threshold.
static const float HYSTERESIS_LUX = 35.0f;

// BH1750 high-res mode tops out well below this; anything above is a bad reading.
static const float MAX_VALID_LUX = 60000.0f;

void resetAutoTimers() {
	darkCandidateStartMs = 0;
	brightCandidateStartMs = 0;
}

void forceRelayOpenAndReset() {
	writeRelay(false);
	resetAutoTimers();
}

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

bool isHandbrakeOn() {
	// Handbrake wire is expected to be earthed when engaged.
	// With INPUT_PULLUP, LOW means handbrake ON.
	return digitalRead(PIN_HANDBRAKE) == LOW;
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
	bool handbrakeOn = isHandbrakeOn();
	display.firstPage();
	do {
		display.setDrawColor(1);
		display.setFont(u8g2_font_7x13B_tf);

		display.setCursor(0, 12);
		// Keep the active mode visible, and show brake status separately below.
		if (mode == MODE_FORCE_ON) {
			display.println(F("FORCE ON"));
		} else if (mode == MODE_FORCE_OFF) {
			display.println(F("FORCE OFF"));
		} else {
			display.println(F("AUTO"));
		}
		display.drawHLine(0, 14, 128);

		display.setFont(u8g2_font_6x12_tf);
		display.setCursor(0, 25);
		display.print(F("Lux: "));
		if (sensorReady) {
			float displayLux = constrain(currentLux, 0.0f, 32767.0f);
			display.print(static_cast<int>(displayLux + 0.5f));
			display.print(F(" lx"));
		} else {
			display.print(F("ERR"));
		}

		display.setCursor(0, 37);
		display.print(F("Set: "));
		display.print(static_cast<int>(thresholdLux + 0.5f));
		display.print(F(" lx"));

		display.setCursor(0, 49);
		display.print(F("Brake: "));
		display.print(handbrakeOn ? F("ON") : F("OFF"));

		display.setCursor(0, 61);
		display.print(F("Light: "));
		display.print(relayClosed ? F("ON") : F("OFF"));

		bool darkNow = currentLux < thresholdLux;
		if (darkNow) {
			drawMoonIcon(108, 34);
		} else {
			drawSunIcon(108, 34);
		}
	} while (display.nextPage());
}

void setup() {
	pinMode(PIN_RELAY, OUTPUT);
	pinMode(PIN_SWITCH_FORCE_ON, INPUT_PULLUP);
	pinMode(PIN_SWITCH_FORCE_OFF, INPUT_PULLUP);
	pinMode(PIN_HANDBRAKE, INPUT_PULLUP);

	// Fail-safe default: relay open at startup.
	forceRelayOpenAndReset();

	Wire.begin();
	// Prevent an I2C glitch from hanging the sketch forever.
	Wire.setWireTimeout(25000, true);
	sensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
	consecutiveSensorErrors = sensorReady ? 0 : MAX_SENSOR_ERRORS_BEFORE_FAULT;

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

void recoverI2cDevices() {
	// A short car supply glitch can leave I2C devices or the AVR bus in a bad state.
	// Reinitialize both devices periodically so the controller can recover without a power cycle.
	Wire.end();
	pinMode(A4, INPUT_PULLUP);
	pinMode(A5, INPUT_PULLUP);
	delay(10);
	Wire.begin();
	Wire.setWireTimeout(25000, true);

	sensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
	consecutiveSensorErrors = sensorReady ? 0 : MAX_SENSOR_ERRORS_BEFORE_FAULT;
	hasLuxSample = false;

	display.begin();
	displayReady = true;
}

void loop() {
	unsigned long now = millis();

	if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
		lastSensorReadMs = now;

		if (sensorReady) {
			float rawLux = lightMeter.readLightLevel();
			if (rawLux < 0.0f || isnan(rawLux) || isinf(rawLux) || rawLux > MAX_VALID_LUX) {
				if (consecutiveSensorErrors < 255) {
					consecutiveSensorErrors++;
				}
				if (consecutiveSensorErrors >= MAX_SENSOR_ERRORS_BEFORE_FAULT) {
					sensorReady = false;
					hasLuxSample = false;
					forceRelayOpenAndReset();
				}
			} else {
				consecutiveSensorErrors = 0;
				if (!hasLuxSample) {
					filteredLux = rawLux;
					hasLuxSample = true;
				} else {
					// Lightweight smoothing for stable display and control.
					filteredLux = (filteredLux * 0.75f) + (rawLux * 0.25f);
				}
				currentLux = filteredLux;
			}
		} else if (now - lastSensorReinitAttemptMs >= SENSOR_REINIT_INTERVAL_MS) {
			lastSensorReinitAttemptMs = now;
			sensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
			if (sensorReady) {
				consecutiveSensorErrors = 0;
				hasLuxSample = false;
			}
		}
	}

	ControlMode mode = readModeFromSwitch();
	float thresholdLux = readThresholdFromPot();
	bool handbrakeOn = isHandbrakeOn();

	if (handbrakeOn) {
		// Safety interlock: handbrake engaged keeps headlights relay open.
		forceRelayOpenAndReset();
	} else if (!sensorReady) {
		forceRelayOpenAndReset();
	} else if (mode == MODE_FORCE_ON) {
		writeRelay(true);
		resetAutoTimers();
	} else if (mode == MODE_FORCE_OFF) {
		forceRelayOpenAndReset();
	} else {
		updateAutoRelayWithDelay(now, thresholdLux);
	}

	if (!sensorReady && (now - lastI2cRecoveryMs >= I2C_RECOVERY_INTERVAL_MS)) {
		lastI2cRecoveryMs = now;
		recoverI2cDevices();
	}

	if (displayReady && (now - lastDisplayRefreshMs >= DISPLAY_INTERVAL_MS)) {
		lastDisplayRefreshMs = now;
		drawUi(mode, thresholdLux);
	}
}
