// ald_manual_control.ino
// Manual control of the ALD system.
//
// While running manual_gui.py on the control computer, users can:
// - set temperature setpoints for heating elements
// - actuate valves one at a time
//
// Useful for testing individual components of the ALD system.
//
// CMU Hacker Fab 2025
// Joel Gonzalez, Haewon Uhm, Atharva Raut
// Updated 2025-Dec-05
//
// Serial command protocol:
//   t<delivery_C>;<prec1_C>;<prec2_C>;<substrate_C>
//     Update heater setpoints and enable temperature control.
//   v<valve_id>;<num_pulses>;<pulse_ms>;<purge_ms>
//     Pulse one valve for the requested number of pulse/purge cycles.
//   r
//     Reset valve command state.
//   k
//     Keepalive (sent by the GUI while a recipe runs). Resets the idle timer only.
//   s
//     Emergency stop: turn heaters off, close valves, then halt.
//
// Safety shutoffs (heaters off, valves untouched; send a new 't' to re-enable):
//   - no command received for IDLE_TIMEOUT_MS
//   - a thermocouple reads NaN (open/shorted) TC_FAULT_LIMIT times in a row
//   - an active heater's reading exceeds its setpoint by OVERTEMP_MARGIN_C
// Each prints a line containing "HEATERS OFF" so the GUI can react.
//
// Relay convention used throughout this file:
//   Heater relays are active LOW: LOW = ON, HIGH = OFF.
//   Valve relays are driven HIGH during pulse/open and LOW during purge/closed.

// ============================================================
// PIN ALLOCATION
// 0-1    Reserved (RX,TX)
// 2      RELAY1 (Substrate heater)
// 3-10   Thermocouples
// 11     RELAY2 (Delivery line tape)
// 12     RELAY3 (Precursor 1 heater)
// A2     RELAY4 (Precursor 2 heater)
// A0     Pressure Gauge
// A1     Flow Sensor
// A3-A5  Valves
// ============================================================

// toggle this if you want the system to do nothing
#define DO_NOTHING 0

// -------------------- Heater relay pins --------------------
#define RELAY_SUBSTRATE_PIN 2
#define RELAY_DELIVERY_PIN A2
#define RELAY_PREC1_PIN 9
#define RELAY_PREC2_PIN 7

// -------------------- Valve relay pins ---------------------
#define RELAY_VALVE1_PIN A3
#define RELAY_VALVE2_PIN A4
#define RELAY_VALVE3_PIN A5

// -------------------- Analog inputs ------------------------
#define PGAUGE_PIN A0
#define FLOW_SENSE_PIN A1

// -------------------- Analog scaling -----------------------
#define ADC_REF_V 5.0
#define CVM211_DIVIDER_RATIO 1.622
#define D6FW_DIVIDER_RATIO 2

const double D6FW04A1_LUT[5] = {1.00, 1.58, 2.88, 4.11, 5.00};

#include <Adafruit_MAX31855.h>

// ============================================================
// Thermocouple physical mapping
//
// K-type thermocouples on pins 3,4,5,6 correspond to:
// index 0 (pin 3) -> substrate heater
// index 1 (pin 4) -> delivery line heater
// index 2 (pin 5) -> precursor 1 heater
// index 3 (pin 6) -> precursor 2 heater
//
// J-type thermocouples on pins 7,8,9,10 are currently unused here
// ============================================================

Adafruit_MAX31855 thermocouples[4] = {
  Adafruit_MAX31855(3),   // substrate
  Adafruit_MAX31855(4),   // delivery
  Adafruit_MAX31855(5),   // precursor 1
  Adafruit_MAX31855(6),   // precursor 2
};

int32_t rawData = 0;

// -------------------- Sampling / timing --------------------
const int num_samples_pgauge = 40;
int active_pgauge_samples = 40;  // matches num_samples_pgauge default
const int num_samples_flow_sense = 10;
const int num_temp_samples = 8;

unsigned long lastTempSend = 0;
unsigned long lastPressureSend = 0;
unsigned long lastFlowSend = 0;

const unsigned long TEMP_INTERVAL_MS = 500;
// 10 Hz so the GUI can resolve the rise/fall of each individual pulse.
const unsigned long PRESSURE_INTERVAL_MS = 100;
const unsigned long FLOW_INTERVAL_MS = 1000;

// -------------------- Thermocouple buffers -----------------
double substrate_readings[num_temp_samples];
double delivery_readings[num_temp_samples];
double prec1_readings[num_temp_samples];
double prec2_readings[num_temp_samples];

int temp_index = 0;
int temp_count = 0;

// Current moving averages
double temp_substrate_avg = 0.0;
double temp_delivery_avg  = 0.0;
double temp_prec1_avg     = 0.0;
double temp_prec2_avg     = 0.0;

// Raw latest values from MAX31855 objects
double current_reading[4] = {0};

// -------------------- Temperature setpoints ----------------
int temp_set_delivery  = 0;
int temp_set_prec1     = 0;
int temp_set_prec2     = 0;
int temp_set_substrate = 0;

// Used to detect setpoint changes and reset precursor startup/pulse state.
// Without this, a small setpoint increase such as 83 C -> 88 C can leave
// the precursor controller stuck in its old steady/startup state.
int last_temp_set_prec1 = -999;
int last_temp_set_prec2 = -999;

int tc_active = 0;

// -------------------- Safety shutoff tuning ----------------
const unsigned long IDLE_TIMEOUT_MS = 60UL * 60UL * 1000UL;  // 60 min with no command -> heaters off
const int TC_FAULT_LIMIT = 6;          // consecutive NaN reads (~3 s at 500 ms) -> heaters off
const int OVERTEMP_MARGIN_C = 50;      // reading > setpoint + this -> heaters off (matches GUI rule)

unsigned long last_activity_ms = 0;
int tc_fail_count[4] = {0};
uint8_t tc_last_err[4] = {0};
// Thermocouple-fault bypass ('i1' on, 'i0' off). Off at boot. The GUI must
// resend 'i1' at least every TC_BYPASS_TTL_MS or it switches itself off,
// so it can't outlive the GUI session. Overtemp and idle checks still apply.
bool tc_fault_bypass = false;
unsigned long tc_bypass_seen_ms = 0;
const unsigned long TC_BYPASS_TTL_MS = 90000;
// In bypass, a faulted TC holds only ITS heater off until it reads normally again.
const int TC_RECOVER_READS = 4;           // consecutive good reads (~2 s) before a held heater may heat again
const double TC_MIN_VALID_C = 1.0;        // 0 C in a lab = bad read, not a real temperature
const double TC_MAX_VALID_C = 600.0;
int tc_good_count[4] = {0};
bool heater_held[4] = {false};            // index matches TC_NAMES   // MAX31855 fault bits: 1=open, 2=short to GND, 4=short to VCC
const char *TC_NAMES[4] = {"substrate", "delivery", "prec1", "prec2"};

// -------------------- Standard hysteresis values ----------
const int HYSTERESIS_PREC1 = 0;
const int HYSTERESIS_PREC2 = 0;
const int HYSTERESIS_SUBSTRATE = 0;

// -------------------- Heater relay states ------------------
bool relay_delivery_on  = false;
bool relay_prec1_on     = false;
bool relay_prec2_on     = false;
bool relay_substrate_on = false;

// -------------------- Valve state --------------------------
unsigned int which_valve = 0;
unsigned int num_pulse = 0;
unsigned int pulse_time = 0;
unsigned int purge_time = 0;

unsigned long valve_timer_start = 0;
int valve_state = 0;  // 0=idle, 1=pulsing, 2=purging

bool busy = false;
bool busy_prev = false;

// ============================================================
// Precursor 1 custom cool-biased slope-aware control state
// ============================================================
bool prec1_startup_mode = true;
bool prec1_peak_seen = false;
bool prec1_temp_rising = false;
bool prec1_pulse_mode = false;
bool prec1_force_recovery_mode = false;
bool prec1_startup_pulse_stage = false;
bool prec2_startup_pulse_stage = false;

float prec1_peak_temp = -999.0;
float prev_temp_prec1_avg = 0.0;

unsigned long prec1_last_switch_ms = 0;
unsigned long prec1_pulse_start_ms = 0;
unsigned long prec1_last_pulse_end_ms = 0;
unsigned long prec1_current_pulse_on_ms = 700;

const unsigned long prec1_startup_min_off_ms = 6000;
const unsigned long prec1_pulse_cooldown_ms = 4000;

// ============================================================
// Precursor 2 custom cool-biased slope-aware control state
// ============================================================
bool prec2_startup_mode = true;
bool prec2_peak_seen = false;
bool prec2_temp_rising = false;
bool prec2_pulse_mode = false;
bool prec2_force_recovery_mode = false;

float prec2_peak_temp = -999.0;
float prev_temp_prec2_avg = 0.0;

unsigned long prec2_last_switch_ms = 0;
unsigned long prec2_pulse_start_ms = 0;
unsigned long prec2_last_pulse_end_ms = 0;
unsigned long prec2_current_pulse_on_ms = 700;

const unsigned long prec2_startup_min_off_ms = 6000;
const unsigned long prec2_pulse_cooldown_ms = 1500;

// ============================================================
// Delivery line custom trend-based control state
// ============================================================
float prev_temp_delivery_avg = 0.0;
float delivery_temp_peak = 0.0;
bool delivery_temp_rising = false;
bool delivery_restart_armed = false;
unsigned long delivery_last_switch_ms = 0;
const unsigned long delivery_lockout_ms = 1500;
int delivery_falling_count = 0;
const int DELIVERY_TREND_CONFIRM = 1;
const float delivery_trend_epsilon = 0.15;

// ============================================================
// Substrate custom trend-based control state
// ============================================================
float prev_temp_substrate_avg = 0.0;
bool substrate_temp_rising = false;
bool substrate_restart_armed = false;
unsigned long substrate_last_switch_ms = 0;
const unsigned long substrate_lockout_ms = 2000;

// ============================================================
// Pressure smoothing buffer
// ============================================================
// 3 samples at 10 Hz = 0.3 s smoothing. Larger flattens individual pulse spikes.
const int PRESSURE_AVG_WINDOW = 3;
double pressure_history[PRESSURE_AVG_WINDOW] = {0};
int pressure_idx = 0;
int pressure_count = 0;

// ============================================================
// Setup
// ============================================================
void setup()
{
  if (DO_NOTHING)
    while (1);

  Serial.begin(115200);

  digitalWrite(RELAY_PREC1_PIN, HIGH);   // preload HIGH
  pinMode(RELAY_PREC1_PIN, OUTPUT);      // now becomes OUTPUT already HIGH
  relay_prec1_on = false;
  pinMode(RELAY_SUBSTRATE_PIN, OUTPUT);
  pinMode(RELAY_DELIVERY_PIN, OUTPUT);
  pinMode(RELAY_PREC2_PIN, OUTPUT);
  pinMode(RELAY_VALVE1_PIN, OUTPUT);
  pinMode(RELAY_VALVE2_PIN, OUTPUT);
  pinMode(RELAY_VALVE3_PIN, OUTPUT);

  // heaters OFF initially (active LOW relays)
  digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);
  digitalWrite(RELAY_DELIVERY_PIN, HIGH);
  digitalWrite(RELAY_PREC1_PIN, HIGH);
  digitalWrite(RELAY_PREC2_PIN, HIGH);

  // valves OFF initially
  digitalWrite(RELAY_VALVE1_PIN, LOW);
  digitalWrite(RELAY_VALVE2_PIN, LOW);
  digitalWrite(RELAY_VALVE3_PIN, LOW);

  pinMode(PGAUGE_PIN, INPUT);
  pinMode(FLOW_SENSE_PIN, INPUT);

  for (int i = 0; i < 4; i++)
    thermocouples[i].begin();

  while (!Serial)
    Serial.println(F("Waiting for serial..."));

  Serial.println(F("Setup complete!"));
}

// ============================================================
// Thermocouple reading and moving average
// ============================================================
void readThermocouples()
{
  for (int i = 0; i < 4; ++i)
  {
    double curr_val = thermocouples[i].readCelsius();

    // Keep the last good value for display, but count faults so a dead
    // sensor can't leave its heater running on a frozen reading.
    if (isnan(curr_val) || curr_val <= TC_MIN_VALID_C || curr_val > TC_MAX_VALID_C) {
      if (tc_fail_count[i] < 1000) tc_fail_count[i]++;
      tc_good_count[i] = 0;
      tc_last_err[i] = isnan(curr_val) ? thermocouples[i].readError() : 0;
    } else {
      tc_fail_count[i] = 0;
      if (tc_good_count[i] < 1000) tc_good_count[i]++;
      current_reading[i] = curr_val;
    }
  }

  // Consistent physical mapping:
  // pin 3 -> substrate
  // pin 4 -> delivery
  // pin 5 -> precursor 1
  // pin 6 -> precursor 2
  substrate_readings[temp_index] = current_reading[0];
  delivery_readings[temp_index]  = current_reading[1];
  prec1_readings[temp_index]     = current_reading[2];
  prec2_readings[temp_index]     = current_reading[3];

  temp_index = (temp_index + 1) % num_temp_samples;
  if (temp_count < num_temp_samples) temp_count++;

  double sum_substrate = 0.0;
  double sum_delivery  = 0.0;
  double sum_prec1     = 0.0;
  double sum_prec2     = 0.0;

  for (int i = 0; i < temp_count; i++)
  {
    sum_substrate += substrate_readings[i];
    sum_delivery  += delivery_readings[i];
    sum_prec1     += prec1_readings[i];
    sum_prec2     += prec2_readings[i];
  }

  temp_substrate_avg = sum_substrate / temp_count;
  temp_delivery_avg  = sum_delivery  / temp_count;
  temp_prec1_avg     = sum_prec1     / temp_count;
  temp_prec2_avg     = sum_prec2     / temp_count;

  Serial.println(
    "T: " +
    String(temp_delivery_avg)  + "; " +
    String(temp_prec1_avg)     + "; " +
    String(temp_prec2_avg)     + "; " +
    String(temp_substrate_avg)
  );
}

// ============================================================
// Reset all per-precursor controller state after a setpoint change
// ============================================================
void resetPrecursorControlState(
    bool &relay_on,
    bool &startup_mode,
    bool &startup_pulse_stage,
    bool &peak_seen,
    bool &temp_rising,
    bool &pulse_mode,
    bool &force_recovery_mode,
    float &peak_temp,
    unsigned long &last_switch_ms,
    unsigned long &pulse_start_ms,
    unsigned long &last_pulse_end_ms,
    unsigned long &current_pulse_on_ms,
    const int relay_pin)
{
  // Active LOW relay: HIGH = OFF
  digitalWrite(relay_pin, HIGH);
  relay_on = false;

  startup_mode = true;
  startup_pulse_stage = false;
  peak_seen = false;
  temp_rising = false;
  pulse_mode = false;
  force_recovery_mode = false;

  peak_temp = -999.0;
  last_switch_ms = millis();
  pulse_start_ms = 0;
  last_pulse_end_ms = millis();
  current_pulse_on_ms = 700;
}

// ============================================================
// Safety shutoff: all heaters OFF, controller disabled until next 't'.
// Precursor state is reset too, otherwise a heater can be left with
// pulse_mode=true and relay off, and never turn back on.
// ============================================================
const __FlashStringHelper *faultName(uint8_t e)
{
  return e & 1 ? F(" (open circuit)") : e & 2 ? F(" (short to GND)") : e & 4 ? F(" (short to VCC)") : F(" (bad reading)");
}

void heatersOff(const String &reason)
{
  digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);
  digitalWrite(RELAY_DELIVERY_PIN, HIGH);
  relay_substrate_on = false;
  relay_delivery_on = false;

  resetPrecursorControlState(relay_prec1_on, prec1_startup_mode, prec1_startup_pulse_stage,
      prec1_peak_seen, prec1_temp_rising, prec1_pulse_mode, prec1_force_recovery_mode,
      prec1_peak_temp, prec1_last_switch_ms, prec1_pulse_start_ms, prec1_last_pulse_end_ms,
      prec1_current_pulse_on_ms, RELAY_PREC1_PIN);
  resetPrecursorControlState(relay_prec2_on, prec2_startup_mode, prec2_startup_pulse_stage,
      prec2_peak_seen, prec2_temp_rising, prec2_pulse_mode, prec2_force_recovery_mode,
      prec2_peak_temp, prec2_last_switch_ms, prec2_pulse_start_ms, prec2_last_pulse_end_ms,
      prec2_current_pulse_on_ms, RELAY_PREC2_PIN);

  tc_active = 0;
  for (int i = 0; i < 4; i++) heater_held[i] = false;
  Serial.println("SAFETY: " + reason + " - HEATERS OFF");
}

// ============================================================
// Shared precursor heater controller
//
// Both precursor heaters use this same startup, pulse, and recovery
// logic. Each precursor passes in its own state variables by reference,
// so the two channels remain independent while sharing identical logic.
// ============================================================
void controlPrecursorHeater(
    double temp_avg,
    double prev_temp_avg,
    int temp_set,
    bool &relay_on,
    bool &startup_mode,
    bool &startup_pulse_stage,
    bool &peak_seen,
    bool &temp_rising,
    bool &pulse_mode,
    bool &force_recovery_mode,
    float &peak_temp,
    unsigned long &last_switch_ms,
    unsigned long &pulse_start_ms,
    unsigned long &last_pulse_end_ms,
    unsigned long &current_pulse_on_ms,
    const unsigned long startup_min_off_ms,
    const unsigned long pulse_cooldown_ms,
    const int relay_pin)
{
  // Same control logic/tuning is used for BOTH precursor heaters.
  // Relay convention: LOW = ON, HIGH = OFF.
  bool can_switch = (millis() - last_switch_ms) >= 2000;

  bool prev_temp_rising = temp_rising;
  const float trend_eps = 0.04;

  if (temp_avg > prev_temp_avg + trend_eps) temp_rising = true;
  else if (temp_avg < prev_temp_avg - trend_eps) temp_rising = false;

  if (temp_avg > peak_temp) peak_temp = temp_avg;

  // -------------------- Shared precursor tuning --------------------
  // Last-resort recovery: only used after startup if the temp drops too low.
  float recovery_on_threshold   = temp_set - 2.5;
  float recovery_off_threshold  = temp_set - 1.0;

  // Startup bulk stage: full ON only when far below setpoint.
  float bulk_on_threshold       = temp_set - 8.0;
  float bulk_to_pulse_threshold = temp_set - 3.0;

  // Startup pulse stage: small pulses near the target to avoid overshoot.
  float startup_pulse_on_threshold = temp_set - 2.0;
  float startup_sufficiently_low   = temp_set - 2.5;
  float startup_cutoff_threshold   = temp_set - 0.4;
  float startup_exit_threshold     = temp_set - 0.3;

  // Normal steady-state pulse control.
  float steady_on_threshold        = temp_set - 0.8;
  float steady_sufficiently_low    = temp_set - 1.2;
  float steady_cutoff_threshold    = temp_set - 0.2;

  unsigned long startup_cooldown   = 1200;
  unsigned long steady_cooldown    = 1200;

  // If setpoint is zero or invalid, keep heater off and reset state.
  if (temp_set <= 0) {
    digitalWrite(relay_pin, HIGH);
    relay_on = false;
    pulse_mode = false;
    force_recovery_mode = false;
    return;
  }

  // ==================================================
  // LAST-RESORT RECOVERY MODE
  // ==================================================
  if (!startup_mode && !pulse_mode && temp_avg <= recovery_on_threshold) {
    force_recovery_mode = true;
  }

  if (force_recovery_mode) {
    if (!relay_on) {
      digitalWrite(relay_pin, LOW);   // ON
      relay_on = true;
      pulse_mode = false;
      last_switch_ms = millis();
    }

    if (temp_avg >= recovery_off_threshold) {
      digitalWrite(relay_pin, HIGH);  // OFF
      relay_on = false;
      pulse_mode = false;
      force_recovery_mode = false;
      last_switch_ms = millis();
      last_pulse_end_ms = millis();
    }

    return;
  }

  // ==================================================
  // STARTUP MODE
  // ==================================================
  if (startup_mode) {

    // -------------------------
    // STAGE 1: bulk heating
    // -------------------------
    if (!startup_pulse_stage) {

      // Important fix for small setpoint increases, e.g. 83 C -> 88 C:
      // If the current temp is already close enough that full bulk heating
      // is not appropriate, skip directly into startup pulse mode.
      if (!relay_on && temp_avg > bulk_on_threshold) {
        startup_pulse_stage = true;
        last_pulse_end_ms = millis();
        return;
      }

      if (!relay_on) {
        if (can_switch && temp_avg <= bulk_on_threshold) {
          digitalWrite(relay_pin, LOW);   // ON
          relay_on = true;
          last_switch_ms = millis();
        }
      }

      if (relay_on && can_switch && temp_avg >= bulk_to_pulse_threshold) {
        digitalWrite(relay_pin, HIGH);    // OFF
        relay_on = false;
        last_switch_ms = millis();
        last_pulse_end_ms = millis();
        startup_pulse_stage = true;
      }

      return;
    }

    // -------------------------
    // STAGE 2: startup pulsing
    // -------------------------
    if (pulse_mode && relay_on) {
      if ((millis() - pulse_start_ms) >= current_pulse_on_ms) {
        digitalWrite(relay_pin, HIGH);   // OFF
        relay_on = false;
        pulse_mode = false;
        last_switch_ms = millis();
        last_pulse_end_ms = millis();
      }
    }

    if (!relay_on && !pulse_mode) {
      bool cooldown_done =
          (millis() - last_pulse_end_ms) >= startup_cooldown;

      bool sufficiently_low = temp_avg <= startup_sufficiently_low;

      if (cooldown_done &&
          temp_avg <= startup_pulse_on_threshold &&
          (!temp_rising || sufficiently_low)) {

        float drop_deg = temp_set - temp_avg;

        current_pulse_on_ms =
            850 + (unsigned long)(250.0 * drop_deg);
        if (current_pulse_on_ms < 850) current_pulse_on_ms = 850;
        if (current_pulse_on_ms > 1700) current_pulse_on_ms = 1700;

        digitalWrite(relay_pin, LOW);   // ON
        relay_on = true;
        pulse_mode = true;
        pulse_start_ms = millis();
        last_switch_ms = millis();
      }
    }

    if (relay_on &&
        pulse_mode &&
        temp_rising &&
        temp_avg >= startup_cutoff_threshold) {
      digitalWrite(relay_pin, HIGH);   // OFF
      relay_on = false;
      pulse_mode = false;
      last_switch_ms = millis();
      last_pulse_end_ms = millis();
    }

    if (!relay_on &&
        prev_temp_rising &&
        !temp_rising &&
        temp_avg >= startup_exit_threshold) {
      startup_mode = false;
      startup_pulse_stage = false;
      peak_seen = true;
      pulse_mode = false;
      force_recovery_mode = false;
      last_pulse_end_ms = millis();
    }

    return;
  }

  // ==================================================
  // NORMAL STEADY PULSE CONTROL
  // ==================================================
  if (pulse_mode && relay_on) {
    if ((millis() - pulse_start_ms) >= current_pulse_on_ms) {
      digitalWrite(relay_pin, HIGH);   // OFF
      relay_on = false;
      pulse_mode = false;
      last_switch_ms = millis();
      last_pulse_end_ms = millis();
    }
  }

  if (!relay_on && !pulse_mode) {
    bool cooldown_done =
        (millis() - last_pulse_end_ms) >= steady_cooldown;

    bool sufficiently_low = temp_avg <= steady_sufficiently_low;

    if (cooldown_done &&
        temp_avg <= steady_on_threshold &&
        (!temp_rising || sufficiently_low)) {

      float drop_deg = temp_set - temp_avg;

      current_pulse_on_ms =
          700 + (unsigned long)(350.0 * drop_deg);
      if (current_pulse_on_ms < 700) current_pulse_on_ms = 700;
      if (current_pulse_on_ms > 1800) current_pulse_on_ms = 1800;

      digitalWrite(relay_pin, LOW);   // ON
      relay_on = true;
      pulse_mode = true;
      pulse_start_ms = millis();
      last_switch_ms = millis();
    }
  }

  if (relay_on &&
      pulse_mode &&
      temp_rising &&
      temp_avg >= steady_cutoff_threshold) {
    digitalWrite(relay_pin, HIGH);   // OFF
    relay_on = false;
    pulse_mode = false;
    last_switch_ms = millis();
    last_pulse_end_ms = millis();
  }
}
// ============================================================
// Heater control
// ============================================================
void actuateHeatingElements()
{
  // Only actuate after temperature command has been received
  if (!tc_active) return;

  // ---------------- Safety checks (independent of the GUI) ----------------
  if (tc_fault_bypass && millis() - tc_bypass_seen_ms >= TC_BYPASS_TTL_MS) {
    tc_fault_bypass = false;
    Serial.println(F("TC FAULT BYPASS EXPIRED (no GUI heartbeat) - fault checks back on"));
  }
  double avgs[4] = {temp_substrate_avg, temp_delivery_avg, temp_prec1_avg, temp_prec2_avg};
  int sets[4]    = {temp_set_substrate, temp_set_delivery, temp_set_prec1, temp_set_prec2};
  for (int i = 0; i < 4; i++)
  {
    // A faulted sensor only matters if its heater is in use.
    if (sets[i] > 0 && tc_fail_count[i] >= TC_FAULT_LIMIT && !tc_fault_bypass) {
      heatersOff(String(F("thermocouple fault on ")) + TC_NAMES[i] + faultName(tc_last_err[i]));
      return;
    }
    if (sets[i] > 0 && avgs[i] > sets[i] + OVERTEMP_MARGIN_C) {
      heatersOff(String(F("overtemp on ")) + TC_NAMES[i] + F(" (") + String(avgs[i]) + F(" C)"));
      return;
    }
  }
  if (millis() - last_activity_ms >= IDLE_TIMEOUT_MS) {
    heatersOff("idle timeout, no command for " + String(IDLE_TIMEOUT_MS / 60000UL) + " min");
    return;
  }

  // ---------------- Bypass: per-heater hold on bad readings ----------------
  for (int i = 0; i < 4; i++)
  {
    bool want_hold = tc_fault_bypass && sets[i] > 0 &&
                     (heater_held[i] ? tc_good_count[i] < TC_RECOVER_READS : tc_fail_count[i] > 0);
    if (want_hold == heater_held[i]) continue;
    heater_held[i] = want_hold;
    if (want_hold) {
      Serial.print(F("HEATER HOLD: ")); Serial.print(TC_NAMES[i]);
      Serial.print(faultName(tc_last_err[i])); Serial.println(F(" - off until readings recover"));
      // Precursor controllers restart from startup mode after a hold.
      if (i == 2) resetPrecursorControlState(relay_prec1_on, prec1_startup_mode, prec1_startup_pulse_stage,
          prec1_peak_seen, prec1_temp_rising, prec1_pulse_mode, prec1_force_recovery_mode,
          prec1_peak_temp, prec1_last_switch_ms, prec1_pulse_start_ms, prec1_last_pulse_end_ms,
          prec1_current_pulse_on_ms, RELAY_PREC1_PIN);
      if (i == 3) resetPrecursorControlState(relay_prec2_on, prec2_startup_mode, prec2_startup_pulse_stage,
          prec2_peak_seen, prec2_temp_rising, prec2_pulse_mode, prec2_force_recovery_mode,
          prec2_peak_temp, prec2_last_switch_ms, prec2_pulse_start_ms, prec2_last_pulse_end_ms,
          prec2_current_pulse_on_ms, RELAY_PREC2_PIN);
    } else {
      Serial.print(F("HEATER RESUME: ")); Serial.print(TC_NAMES[i]);
      Serial.println(F(" readings OK - heating to setpoint"));
    }
  }

  // ----------------------------------------------------------
  // Delivery line heater: peak-tracking control
  // ----------------------------------------------------------
  float delivery_off_threshold = temp_set_delivery - 0.3;

  bool delivery_can_switch =
      (millis() - delivery_last_switch_ms) >= delivery_lockout_ms;
  // Serial.print(F("DELIV set="));
  // Serial.print(temp_set_delivery);
  // Serial.print(F(" avg="));
  // Serial.print(temp_delivery_avg);
  // Serial.print(F(" rising="));
  // Serial.print(delivery_temp_rising ? "Y" : "N");
  // Serial.print(F(" armed="));
  // Serial.print(delivery_restart_armed ? "Y" : "N");
  // Serial.print(F(" fallCt="));
  // Serial.print(delivery_falling_count);
  // Serial.print(F(" relay="));
  // Serial.println(relay_delivery_on ? "ON" : "OFF");
  if (temp_set_delivery <= 0 || heater_held[1])
  {
      // Setpoint 0 = heater off. Without this, the peak-drop restart below
      // keeps firing short heat pulses whenever the line cools slightly.
      if (relay_delivery_on) {
          digitalWrite(RELAY_DELIVERY_PIN, HIGH);
          relay_delivery_on = false;
      }
  }
  else if (relay_delivery_on)
  {
      // Turn off early on the way up
      if (delivery_can_switch &&
          temp_delivery_avg >= delivery_off_threshold)
      {
          digitalWrite(RELAY_DELIVERY_PIN, HIGH);  // OFF
          relay_delivery_on = false;
          delivery_last_switch_ms = millis();
          delivery_temp_peak = 0.0;
      }
  }
  else
  {
      // Track peak temperature while relay is off
      if (temp_delivery_avg > delivery_temp_peak)
          delivery_temp_peak = temp_delivery_avg;

      float drop_from_peak = delivery_temp_peak - temp_delivery_avg;

      // Turn on as soon as temp drops 0.3°C below peak
      if (delivery_can_switch && drop_from_peak >= 0.3)
      {
          digitalWrite(RELAY_DELIVERY_PIN, LOW);   // ON
          relay_delivery_on = true;
          delivery_last_switch_ms = millis();
          delivery_temp_peak = 0.0;
      }
      // Cold start - well below setpoint
      else if (delivery_can_switch &&
              temp_delivery_avg < (temp_set_delivery - 3.0))
      {
          digitalWrite(RELAY_DELIVERY_PIN, LOW);   // ON
          relay_delivery_on = true;
          delivery_last_switch_ms = millis();
          delivery_temp_peak = 0.0;
      }
  }

  // Update trend tracking
  bool prev_delivery_temp_rising = delivery_temp_rising;
  if (temp_delivery_avg > prev_temp_delivery_avg + delivery_trend_epsilon)
      delivery_temp_rising = true;
  else if (temp_delivery_avg < prev_temp_delivery_avg - delivery_trend_epsilon)
      delivery_temp_rising = false;

  prev_temp_delivery_avg = temp_delivery_avg;

  if (!heater_held[2]) controlPrecursorHeater(
      temp_prec1_avg,
      prev_temp_prec1_avg,
      temp_set_prec1,
      relay_prec1_on,
      prec1_startup_mode,
      prec1_startup_pulse_stage,
      prec1_peak_seen,
      prec1_temp_rising,
      prec1_pulse_mode,
      prec1_force_recovery_mode,
      prec1_peak_temp,
      prec1_last_switch_ms,
      prec1_pulse_start_ms,
      prec1_last_pulse_end_ms,
      prec1_current_pulse_on_ms,
      prec1_startup_min_off_ms,
      prec1_pulse_cooldown_ms,
      RELAY_PREC1_PIN
  );
  prev_temp_prec1_avg = temp_prec1_avg;

  if (!heater_held[3]) controlPrecursorHeater(
      temp_prec2_avg,
      prev_temp_prec2_avg,
      temp_set_prec2,
      relay_prec2_on,
      prec2_startup_mode,
      prec2_startup_pulse_stage,
      prec2_peak_seen,
      prec2_temp_rising,
      prec2_pulse_mode,
      prec2_force_recovery_mode,
      prec2_peak_temp,
      prec2_last_switch_ms,
      prec2_pulse_start_ms,
      prec2_last_pulse_end_ms,
      prec2_current_pulse_on_ms,
      prec2_startup_min_off_ms,
      prec2_pulse_cooldown_ms,
      RELAY_PREC2_PIN
  );
  prev_temp_prec2_avg = temp_prec2_avg;

  // ----------------------------------------------------------
  // Substrate heater: simple on/off at setpoint
  // Turns ON below setpoint, turns OFF at or above setpoint
  // ----------------------------------------------------------
  bool substrate_can_switch =
    (millis() - substrate_last_switch_ms) >= substrate_lockout_ms;

  if (heater_held[0])
  {
    if (relay_substrate_on) {
      digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);   // OFF (held, bad readings)
      relay_substrate_on = false;
      substrate_last_switch_ms = millis();       // lockout also limits relay chatter
    }
  }
  else if (relay_substrate_on)
  {
    if (substrate_can_switch &&
        temp_substrate_avg >= temp_set_substrate)
    {
      Serial.println(F("SUBSTRATE -> OFF"));
      digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);   // OFF
      relay_substrate_on = false;
      substrate_last_switch_ms = millis();
    }
  }
  else
  {
    if (substrate_can_switch &&
        temp_substrate_avg < temp_set_substrate)
    {
      Serial.println(F("SUBSTRATE -> ON"));
      digitalWrite(RELAY_SUBSTRATE_PIN, LOW);    // ON
      relay_substrate_on = true;
      substrate_last_switch_ms = millis();
    }
  }

  prev_temp_substrate_avg = temp_substrate_avg;
}
// ============================================================
// Valve actuation
// ============================================================
void precursorValveActuation()
{
    unsigned long now = millis();

    if (num_pulse > 0)
    {
        // Reduce pressure sampling during valve operation to minimize timing jitter
        active_pgauge_samples = 10;  // fast sampling during valve

        if (valve_state == 0)
        {
            digitalWrite(which_valve == 1 ? RELAY_VALVE1_PIN :
                        which_valve == 2 ? RELAY_VALVE2_PIN : RELAY_VALVE3_PIN, HIGH);
            // GUI parses this line to count lifetime pulses/open time per precursor.
            Serial.println("V: Pulsing valve " + String(which_valve) + " " + String(pulse_time) + "ms");
            valve_timer_start = now;
            valve_state = 1;
        }
        else if (valve_state == 1 && (now - valve_timer_start >= pulse_time))
        {
            digitalWrite(which_valve == 1 ? RELAY_VALVE1_PIN :
                        which_valve == 2 ? RELAY_VALVE2_PIN : RELAY_VALVE3_PIN, LOW);
            Serial.println(F("V: Purging line"));
            valve_timer_start = now;
            valve_state = 2;
        }
        else if (valve_state == 2 && (now - valve_timer_start >= purge_time))
        {
            num_pulse--;
            valve_state = 0;
        }
    }
    else
    {
        active_pgauge_samples = num_samples_pgauge;  // restore full sampling
        digitalWrite(RELAY_VALVE1_PIN, LOW);
        digitalWrite(RELAY_VALVE2_PIN, LOW);
        digitalWrite(RELAY_VALVE3_PIN, LOW);
        valve_state = 0;
    }
}

// ============================================================
// Pressure gauge helpers
// ============================================================
static double cvm211_readGaugeVolts() {
    uint32_t acc = 0;
    for (int i = 0; i < active_pgauge_samples; ++i) acc += analogRead(PGAUGE_PIN);
    double adcCounts = acc / (double)active_pgauge_samples;
    double vadc = (adcCounts * ADC_REF_V) / 1023.0;
    double vgauge = vadc * CVM211_DIVIDER_RATIO;
    return vgauge;
}

static double cvm211_logLinear_toTorr(double v)
{
  if (v < 1.0) v = 1.0;
  if (v > 8.0) v = 8.0;
  return pow(10.0, v - 5.0);
}

void readCVM211PressureTorr()
{
  double v = cvm211_readGaugeVolts();
  double P_Torr = cvm211_logLinear_toTorr(v);
  double P_mTorr = 1000 * P_Torr;

  pressure_history[pressure_idx] = P_mTorr;
  pressure_idx = (pressure_idx + 1) % PRESSURE_AVG_WINDOW;
  if (pressure_count < PRESSURE_AVG_WINDOW) pressure_count++;

  double sum = 0.0;
  for (int i = 0; i < pressure_count; i++) sum += pressure_history[i];
  double avg = sum / pressure_count;

  if (avg > 100000.0)   // pick the unit from the value actually printed
    Serial.println("P: " + String(avg / 1000.0) + " Torr");
  else
    Serial.println("P: " + String(avg) + " mTorr");
}

// ============================================================
// Flow sensor helpers
// ============================================================
static double d6fw_readGaugeVolts()
{
  uint32_t acc = 0;
  for (int i = 0; i < num_samples_flow_sense; ++i) acc += analogRead(FLOW_SENSE_PIN);
  double adcCounts = acc / (double)num_samples_flow_sense;
  double vadc = (adcCounts * ADC_REF_V) / 1023.0;
  double vgauge = vadc * D6FW_DIVIDER_RATIO;
  return vgauge;
}

static double d6fw_nonLinear_to_mps(double v)
{
  double flow_rate_extrapolated;
  int low = 0;
  int high = 4;

  for (int i = 0; i <= 4; ++i)
  {
    if (D6FW04A1_LUT[i] <= v) low = i;
    if (D6FW04A1_LUT[4 - i] > v) high = (4 - i);
  }

  if (D6FW04A1_LUT[high] == D6FW04A1_LUT[low])
    flow_rate_extrapolated = 0;
  else
    flow_rate_extrapolated =
      ((v - D6FW04A1_LUT[low]) * high +
       (D6FW04A1_LUT[high] - v) * low) /
      (D6FW04A1_LUT[high] - D6FW04A1_LUT[low]);

  return flow_rate_extrapolated;
}

void readD6FWFlow()
{
  double v = d6fw_readGaugeVolts();
  double flow_rate = d6fw_nonLinear_to_mps(v);
  Serial.println("F: " + String(flow_rate) + " m/s");
}

// ============================================================
// Main loop
//
// The loop remains non-blocking for normal sensing/control. Valve timing
// uses millis() so temperature, pressure, and flow telemetry can continue
// updating while a valve command is active.
// ============================================================
void loop()
{
  busy_prev = busy;
  unsigned long now = millis();

  if (Serial.available() > 0)
  {
    char s[100] = {0};
    String inputString = Serial.readStringUntil('\n');
    inputString.toCharArray(s, sizeof(s));   // bounded copy; strcpy overflowed on long lines

    // Bypass heartbeat: handled before the idle-timer reset on purpose, so an
    // open-but-unattended GUI can't keep heaters on forever.
    if (s[0] == 'i') {
      bool on = (s[1] == '1');
      if (on) tc_bypass_seen_ms = millis();
      if (on != tc_fault_bypass)
        Serial.println(on ? F("WARNING: TC FAULT BYPASS ON") : F("TC FAULT BYPASS OFF"));
      tc_fault_bypass = on;
      return;
    }

    // Any command counts as operator/recipe activity for the idle timeout.
    last_activity_ms = millis();

    if (s[0] == 'k') return;   // keepalive: silent, nothing else to do

    Serial.println(F("Got command!"));
    Serial.println(s);
    int result = 0;

    if (s[0] == 's')
    {
      Serial.println(F("EMERGENCY STOP command received! Closing all valves, stopping heating."));

      digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);
      digitalWrite(RELAY_DELIVERY_PIN, HIGH);
      digitalWrite(RELAY_PREC1_PIN, HIGH);
      digitalWrite(RELAY_PREC2_PIN, HIGH);

      digitalWrite(RELAY_VALVE1_PIN, LOW);
      digitalWrite(RELAY_VALVE2_PIN, LOW);
      digitalWrite(RELAY_VALVE3_PIN, LOW);

      while (1) { }
    }
    else if (s[0] == 'h')
    {
      heatersOff(F("heaters off command"));   // normal stop; next 't' turns control back on
    }
    else if (s[0] == 'r')
    {
      num_pulse = 0;
      which_valve = 0;
      Serial.println(F("RESET command received! Resetting state!"));
    }
    else
    {
      if (s[0] == 't')
      {
        result = sscanf(
          s,
          "t%d;%d;%d;%d",
          &temp_set_delivery,
          &temp_set_prec1,
          &temp_set_prec2,
          &temp_set_substrate
        );
        // A partial parse may have overwritten some setpoints; don't heat on those.
        if (result != 4) heatersOff(F("misformatted temperature command"));
        else tc_active = 1;
      Serial.print(F("SETPOINTS:"));
      Serial.print(temp_set_delivery); Serial.print(F(","));
      Serial.print(temp_set_prec1); Serial.print(F(","));
      Serial.print(temp_set_prec2); Serial.print(F(","));
      Serial.println(temp_set_substrate);

      // Reset precursor controller state whenever either precursor setpoint changes.
      // This makes small changes like 83 C -> 88 C retrigger startup/pulse logic.
      if (last_temp_set_prec1 != -999 && temp_set_prec1 != last_temp_set_prec1) {
        Serial.println(F("PREC1 setpoint changed: resetting precursor 1 control state"));
        resetPrecursorControlState(
          relay_prec1_on,
          prec1_startup_mode,
          prec1_startup_pulse_stage,
          prec1_peak_seen,
          prec1_temp_rising,
          prec1_pulse_mode,
          prec1_force_recovery_mode,
          prec1_peak_temp,
          prec1_last_switch_ms,
          prec1_pulse_start_ms,
          prec1_last_pulse_end_ms,
          prec1_current_pulse_on_ms,
          RELAY_PREC1_PIN
        );
      }

      if (last_temp_set_prec2 != -999 && temp_set_prec2 != last_temp_set_prec2) {
        Serial.println(F("PREC2 setpoint changed: resetting precursor 2 control state"));
        resetPrecursorControlState(
          relay_prec2_on,
          prec2_startup_mode,
          prec2_startup_pulse_stage,
          prec2_peak_seen,
          prec2_temp_rising,
          prec2_pulse_mode,
          prec2_force_recovery_mode,
          prec2_peak_temp,
          prec2_last_switch_ms,
          prec2_pulse_start_ms,
          prec2_last_pulse_end_ms,
          prec2_current_pulse_on_ms,
          RELAY_PREC2_PIN
        );
      }

      last_temp_set_prec1 = temp_set_prec1;
      last_temp_set_prec2 = temp_set_prec2;
      }

      else if (s[0] == 'v')
      {
        if (busy)
        {
          Serial.println(F("COMMAND IGNORED. Wait for previous command to finish, or issue RESET."));
          return;
        }
        busy = true;
        result = sscanf(
          s,
          "v%u;%u;%u;%u",
          &which_valve,
          &num_pulse,
          &pulse_time,
          &purge_time
        );
        // A partial parse used to leave num_pulse set, so the valve fired with
        // stale timing. Reject anything not fully valid before it can actuate.
        if (result != 4 || which_valve < 1 || which_valve > 3)
        {
          num_pulse = 0;
          busy = false;
          if (result == 4) {
            Serial.println(F("INVALID VALVE ID!"));
            return;
          }
        }
      }
      else
      {
        Serial.println(F("INVALID COMMAND!"));
        return;
      }

      if (result != 4)
      {
        Serial.println(F("MISFORMATTED COMMAND! sscanf result: "));
        Serial.println(result);
        return;
      }
      else
      {
        Serial.println(F("Starting command!"));
      }
    }
  }

  precursorValveActuation();

  if (num_pulse == 0)
  {
    busy = false;
    if (busy_prev) Serial.println(F("Previous command has completed. Ready for new command."));
  }

  if (now - lastTempSend >= TEMP_INTERVAL_MS)
  {
    lastTempSend = now;
    readThermocouples();
    actuateHeatingElements();
  }

  if (now - lastPressureSend >= PRESSURE_INTERVAL_MS)
  {
    lastPressureSend = now;

    // Previously pressure was only read while idle or during the open pulse,
    // so the purge window (where each pulse's spike shows up) was never sampled
    // and only the final spike after the command was visible. Now we skip only
    // the open pulse, to keep pulse timing tight.
    if (valve_state != 1) {
      readCVM211PressureTorr();
    }
  }

  if (now - lastFlowSend >= FLOW_INTERVAL_MS)
  {
    lastFlowSend = now;
    readD6FWFlow();
  }
}
