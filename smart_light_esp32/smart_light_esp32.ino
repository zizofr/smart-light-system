/*
 * Smart streetlight - ESP32
 *
 * Behaviour:
 *   - Day   : lamp off
 *   - Night : lamp at 20 %
 *   - Motion: lamp at 100 % while motion is detected, then back to 20 %
 *             MOTION_HOLD_MS after the last detection
 *   - The potentiometer sets the day/night threshold
 *   - One telemetry line per second on the serial port
 *   - Serial commands (115200 baud, "Newline"): AUTO | MANU n (n = 0..100) | ?
 *
 * Hardware (ESP32 DevKit, 3.3 V logic):
 *   Lamp : LED + 100-220 ohm resistor on GPIO 18 (PWM)
 *   PIR  : HC-SR501 OUT on GPIO 27 (VCC on VIN = 5 V)
 *   LDR  : LDR between 3V3 and GPIO 34, 10 kohm from GPIO 34 to GND
 *          (higher value = more light)
 *   Pot  : 10 kohm, wiper on GPIO 35, outer pins on 3V3 and GND
 *
 * Needs ESP32 board package 3.x (analogWrite).
 * Written with AI assistance; I tested it on my own hardware.
 */

// ---------------- Pins ----------------
const uint8_t PIN_LAMP = 18;
const uint8_t PIN_PIR  = 27;
const uint8_t PIN_LDR  = 34;
const uint8_t PIN_POT  = 35;

// ---------------- Parameters ----------------
const unsigned long SERIAL_BAUD         = 115200;
const unsigned long LIGHT_PERIOD_MS     = 50;     // LDR and threshold sampling
const unsigned long TELEMETRY_PERIOD_MS = 1000;
const unsigned long MOTION_HOLD_MS      = 5000;   // time at 100 % after the last motion
const unsigned long PIR_WARMUP_MS       = 30000;  // PIR gives random output after power-on (0 = disable)
const int           HYSTERESIS          = 150;    // ADC counts (0..4095 scale)
const uint8_t       LEVEL_NIGHT         = 20;     // %
const uint8_t       LEVEL_MOTION        = 100;    // %

// ---------------- State ----------------
enum class State { Day, Night, Motion, Manual };

State state = State::Day;
uint8_t lampLevel = 255;        // 255 = "not written yet", forces the first write
uint8_t manualLevel = 0;

bool isNight = false;
int ldrAvg = 0;
int threshold = 0;

bool pirRaw = false;
bool pirPrev = false;
bool hasMotion = false;
unsigned long lastMotionMs = 0;
unsigned long motionCount = 0;

// ---------------- Moving average (8 samples) ----------------
const uint8_t AVG_SIZE = 8;
int samples[AVG_SIZE];
long sampleSum = 0;
uint8_t sampleIdx = 0;

int movingAverage(int x) {
  sampleSum += x - samples[sampleIdx];
  samples[sampleIdx] = x;
  sampleIdx = (sampleIdx + 1) % AVG_SIZE;
  return (int)(sampleSum / AVG_SIZE);
}

// ---------------- Tasks ----------------
void lightTask() {
  ldrAvg = movingAverage(analogRead(PIN_LDR));
  threshold = analogRead(PIN_POT);

  // Hysteresis: night below the threshold, day only above threshold + margin
  if (ldrAvg < threshold) isNight = true;
  else if (ldrAvg > threshold + HYSTERESIS) isNight = false;
}

void motionTask(unsigned long now) {
  pirRaw = (digitalRead(PIN_PIR) == HIGH);
  if (now < PIR_WARMUP_MS) return;               // ignore the PIR while it warms up

  if (pirRaw && !pirPrev) motionCount++;         // rising edge = one passer-by
  if (pirRaw) {                                  // remember the last detection
    hasMotion = true;
    lastMotionMs = now;
  }
  pirPrev = pirRaw;
}

bool motionActive(unsigned long now) {
  return hasMotion && (now - lastMotionMs < MOTION_HOLD_MS);
}

void stateTask(unsigned long now) {
  if (state == State::Manual) return;            // only AUTO leaves manual mode
  if (!isNight)                state = State::Day;
  else if (motionActive(now))  state = State::Motion;
  else                         state = State::Night;
}

// Lamp level depends only on the state
uint8_t levelOf(State s) {
  switch (s) {
    case State::Day:    return 0;
    case State::Night:  return LEVEL_NIGHT;
    case State::Motion: return LEVEL_MOTION;
    default:            return manualLevel;
  }
}

void applyLamp(uint8_t percent) {
  if (percent == lampLevel) return;
  lampLevel = percent;
  analogWrite(PIN_LAMP, percent * 255 / 100);
}

const char* stateName() {
  switch (state) {
    case State::Day:    return "DAY";
    case State::Night:  return "NIGHT";
    case State::Motion: return "MOTION";
    default:            return "MANUAL";
  }
}

void sendTelemetry(unsigned long now) {
  Serial.printf("t=%lu state=%s ldr=%d thr=%d lamp=%d pir=%d count=%lu\n",
                now / 1000, stateName(), ldrAvg, threshold,
                (int)lampLevel, (int)pirRaw, motionCount);
}

// ---------------- Serial commands ----------------
char cmdBuf[16];                // 15 characters + terminator
uint8_t cmdLen = 0;
bool cmdOverflow = false;

void runCommand(const char* cmd) {
  if (cmd[0] == '\0') return;

  if (strcmp(cmd, "?") == 0) {
    sendTelemetry(millis());
  } else if (strcmp(cmd, "AUTO") == 0) {
    state = State::Day;                          // recomputed by stateTask()
    Serial.println("OK AUTO");
  } else if (strncmp(cmd, "MANU ", 5) == 0) {
    char* end;
    long n = strtol(cmd + 5, &end, 10);
    if (end == cmd + 5 || *end != '\0') {
      Serial.print("ERR ");
      Serial.println(cmd);
    } else {
      manualLevel = (uint8_t)constrain(n, 0L, 100L);
      state = State::Manual;
      Serial.print("OK MANU ");
      Serial.println((int)manualLevel);
    }
  } else {
    Serial.print("ERR ");
    Serial.println(cmd);
  }
}

void commandTask() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (cmdOverflow) {
        Serial.println("ERR too long");          // never execute a truncated command
      } else {
        cmdBuf[cmdLen] = '\0';
        runCommand(cmdBuf);
      }
      cmdLen = 0;
      cmdOverflow = false;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = toupper(c);
    } else {
      cmdOverflow = true;
    }
  }
}

// ---------------- Setup and main loop ----------------
unsigned long lastLight = 0;
unsigned long lastTelemetry = 0;

void setup() {
  Serial.begin(SERIAL_BAUD);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LAMP, OUTPUT);

  int first = analogRead(PIN_LDR);               // fill the filter before starting
  for (uint8_t i = 0; i < AVG_SIZE; i++) samples[i] = first;
  sampleSum = (long)first * AVG_SIZE;
  lightTask();

  Serial.println("Smart streetlight ready. Commands: AUTO | MANU n | ?");
}

void loop() {
  unsigned long now = millis();

  if (now - lastLight >= LIGHT_PERIOD_MS) {
    lastLight += LIGHT_PERIOD_MS;
    lightTask();
  }
  if (now - lastTelemetry >= TELEMETRY_PERIOD_MS) {
    lastTelemetry += TELEMETRY_PERIOD_MS;
    sendTelemetry(now);
  }

  motionTask(now);
  commandTask();
  stateTask(now);
  applyLamp(levelOf(state));
}
