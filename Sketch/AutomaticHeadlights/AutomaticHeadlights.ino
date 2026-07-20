#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// -------- Hardware configuration --------
static const uint8_t PIN_RELAY = 8;
static const uint8_t PIN_POT = A0;
static const uint8_t PIN_SWITCH_FORCE_ON = 2;
static const uint8_t PIN_SWITCH_FORCE_OFF = 3;

// Most 5V relay modules are active LOW. Set false if your module is active HIGH.
static const bool RELAY_ACTIVE_LOW = true;

// OLED (128x64, I2C)
static const uint8_t OLED_WIDTH = 128;
static const uint8_t OLED_HEIGHT = 64;
static const int8_t OLED_RESET_PIN = -1;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);

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
	display.fillCircle(cx, cy, 7, SSD1306_WHITE);
	display.fillCircle(cx, cy, 4, SSD1306_BLACK);
	for (uint8_t i = 0; i < 8; i++) {
		float a = i * 0.785398f;  // 2*pi/8
		int16_t x0 = cx + static_cast<int16_t>(9 * cos(a));
		int16_t y0 = cy + static_cast<int16_t>(9 * sin(a));
		int16_t x1 = cx + static_cast<int16_t>(13 * cos(a));
		int16_t y1 = cy + static_cast<int16_t>(13 * sin(a));
		display.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
	}
}

void drawMoonIcon(int16_t cx, int16_t cy) {
	display.fillCircle(cx, cy, 8, SSD1306_WHITE);
	display.fillCircle(cx + 4, cy - 2, 8, SSD1306_BLACK);
}

void drawUi(ControlMode mode, float thresholdLux) {
	display.clearDisplay();

	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);

	display.setCursor(0, 0);
	// Mode heading kept short so the setpoint and status are easy to read at a glance.
	if (mode == MODE_FORCE_ON) {
		display.println(F("FORCE ON"));
	} else if (mode == MODE_FORCE_OFF) {
		display.println(F("FORCE OFF"));
	} else {
		display.println(F("AUTO"));
	}

	display.setCursor(0, 14);
	display.print(F("Lux: "));
	display.println(static_cast<int>(currentLux + 0.5f));

	display.setCursor(0, 26);
	display.print(F("Set: "));
	display.println(static_cast<int>(thresholdLux + 0.5f));

	display.setCursor(0, 38);
	display.print(F("Lights: "));
	display.println(relayClosed ? F("ON") : F("OFF"));

	bool darkNow = currentLux < thresholdLux;
	if (darkNow) {
		drawMoonIcon(108, 20);
	} else {
		drawSunIcon(108, 20);
	}

	display.display();
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

	// Keep running even if display init fails; controller logic still works.
	displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
	if (!displayReady) {
		// No display found; continue headlight logic silently.
	} else {
		display.clearDisplay();
		display.setTextSize(1);
		display.setTextColor(SSD1306_WHITE);
		display.setCursor(0, 0);
		display.println(F("Auto Headlights"));
		display.println(F("Init..."));
		display.display();
	}
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
