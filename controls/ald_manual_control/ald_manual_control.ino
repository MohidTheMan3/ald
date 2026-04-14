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
const int num_samples_pgauge = 200;
int active_pgauge_samples = 200;  // matches num_samples_pgauge default
const int num_samples_flow_sense = 10;
const int num_temp_samples = 10;

unsigned long lastTempSend = 0;
unsigned long lastPressureSend = 0;
unsigned long lastFlowSend = 0;

const unsigned long TEMP_INTERVAL_MS = 500;
const unsigned long PRESSURE_INTERVAL_MS = 50;
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

int tc_active = 0;

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

float prec2_peak_temp = -999.0;
float prev_temp_prec2_avg = 0.0;

unsigned long prec2_last_switch_ms = 0;
unsigned long prec2_pulse_start_ms = 0;
unsigned long prec2_last_pulse_end_ms = 0;
unsigned long prec2_current_pulse_on_ms = 700;

const unsigned long prec2_startup_min_off_ms = 6000;
const unsigned long prec2_pulse_cooldown_ms = 4000;

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
const int PRESSURE_AVG_WINDOW = 5;
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
    Serial.println("Waiting for serial...");

  Serial.println("Setup complete!");
}

// ============================================================
// Thermocouple reading and moving average
// ============================================================
void readThermocouples()
{
  for (int i = 0; i < 4; ++i)
  {
    double curr_val = thermocouples[i].readCelsius();

    if (!isnan(curr_val))
      current_reading[i] = curr_val;
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
// Heater control
// ============================================================
void actuateHeatingElements()
{
  // Only actuate after temperature command has been received
  if (!tc_active) return;

  // ----------------------------------------------------------
  // Delivery line heater: peak-tracking control
  // ----------------------------------------------------------
  float delivery_off_threshold = temp_set_delivery - 0.3;

  bool delivery_can_switch =
      (millis() - delivery_last_switch_ms) >= delivery_lockout_ms;
  Serial.print("DELIV set=");
  Serial.print(temp_set_delivery);
  Serial.print(" avg=");
  Serial.print(temp_delivery_avg);
  Serial.print(" rising=");
  Serial.print(delivery_temp_rising ? "Y" : "N");
  Serial.print(" armed=");
  Serial.print(delivery_restart_armed ? "Y" : "N");
  Serial.print(" fallCt=");
  Serial.print(delivery_falling_count);
  Serial.print(" relay=");
  Serial.println(relay_delivery_on ? "ON" : "OFF");
  if (relay_delivery_on)
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

 // ----------------------------------------------------------
  // Precursor 1 heater: two-phase variable-pulse control
  // tuned for higher setpoints such as 75C
  // ----------------------------------------------------------
  float prec1_error = temp_set_prec1 - temp_prec1_avg;

  bool prec1_can_switch =
      (millis() - prec1_last_switch_ms) >= 2500;

  // Detect trend
  bool prev_prec1_temp_rising = prec1_temp_rising;

  if (temp_prec1_avg > prev_temp_prec1_avg + 0.12) {
    prec1_temp_rising = true;
  }
  else if (temp_prec1_avg < prev_temp_prec1_avg - 0.12) {
    prec1_temp_rising = false;
  }

  // Track peak
  if (temp_prec1_avg > prec1_peak_temp) {
    prec1_peak_temp = temp_prec1_avg;
  }

  // -------------------------
  // STARTUP MODE
  // -------------------------
  if (prec1_startup_mode) {

    // Startup shutoff threshold:
    // shut off early enough to avoid the initial large overshoot
    float startup_off_threshold = temp_set_prec1 - 6.0;

    // If already rising and getting closer to setpoint, shut off even earlier
    if (prec1_error < 5.0 && prec1_temp_rising) {
      startup_off_threshold = temp_set_prec1 - 7.0;
    }

    // Turn ON if well below target
    if (!relay_prec1_on) {
      if (prec1_can_switch && temp_prec1_avg <= temp_set_prec1 - 8.0) {
        digitalWrite(RELAY_PREC1_PIN, LOW);   // ON
        relay_prec1_on = true;
        prec1_last_switch_ms = millis();
      }
    }
    // Turn OFF once startup threshold is reached
    else {
      if (prec1_can_switch && temp_prec1_avg >= startup_off_threshold) {
        digitalWrite(RELAY_PREC1_PIN, HIGH);  // OFF
        relay_prec1_on = false;
        prec1_last_switch_ms = millis();
      }
    }

    // Exit startup mode after first peak passes and enough off-time has elapsed
    if (!relay_prec1_on &&
        prev_prec1_temp_rising &&
        !prec1_temp_rising &&
        (millis() - prec1_last_switch_ms) >= prec1_startup_min_off_ms) {
      prec1_startup_mode = false;
      prec1_peak_seen = true;
      prec1_pulse_mode = false;
      prec1_last_pulse_end_ms = millis();
    }
  }

  // -------------------------
  // STEADY MODE
  // variable pulse-width reheating
  // -------------------------
  else {
    // Start reheating before dropping too low
    float prec1_on_threshold = temp_set_prec1 - 0.8;   // e.g. 73.5C for 75C setpoint

    // -------------------------
    // End active pulse after assigned pulse width
    // -------------------------
    if (prec1_pulse_mode && relay_prec1_on) {
      if ((millis() - prec1_pulse_start_ms) >= prec1_current_pulse_on_ms) {
        digitalWrite(RELAY_PREC1_PIN, HIGH);   // OFF
        relay_prec1_on = false;
        prec1_pulse_mode = false;
        prec1_last_switch_ms = millis();
        prec1_last_pulse_end_ms = millis();
      }
    }

    // -------------------------
    // Start a new pulse if needed
    // -------------------------
    if (!relay_prec1_on && !prec1_pulse_mode) {
      bool cooldown_done =
          (millis() - prec1_last_pulse_end_ms) >= prec1_pulse_cooldown_ms;

      if (cooldown_done &&
          temp_prec1_avg <= prec1_on_threshold &&
          !prec1_temp_rising) {

        float drop_deg = temp_set_prec1 - temp_prec1_avg;   // degrees below setpoint

        // Variable pulse law for higher-temp operation
        prec1_current_pulse_on_ms =
            1100 + (unsigned long)(600.0 * (drop_deg - 1.5));

        // Clamp pulse width
        if (prec1_current_pulse_on_ms < 1000) {
          prec1_current_pulse_on_ms = 1000;
        }
        if (prec1_current_pulse_on_ms > 3500) {
          prec1_current_pulse_on_ms = 3500;
        }

        digitalWrite(RELAY_PREC1_PIN, LOW);   // ON
        relay_prec1_on = true;
        prec1_pulse_mode = true;
        prec1_pulse_start_ms = millis();
        prec1_last_switch_ms = millis();
      }
    }

    // -------------------------
    // Safety shutoff if heating and already rising near target
    // -------------------------
    if (relay_prec1_on &&
        prec1_temp_rising &&
        temp_prec1_avg >= temp_set_prec1 - 2.2) {
      digitalWrite(RELAY_PREC1_PIN, HIGH);   // OFF
      relay_prec1_on = false;
      prec1_pulse_mode = false;
      prec1_last_switch_ms = millis();
      prec1_last_pulse_end_ms = millis();
    }
  }

  prev_temp_prec1_avg = temp_prec1_avg;

   // ----------------------------------------------------------
  // Precursor 2 heater: two-phase variable-pulse control
  // tuned for higher setpoints such as 75C
  // ----------------------------------------------------------
  float prec2_error = temp_set_prec2 - temp_prec2_avg;

  bool prec2_can_switch =
      (millis() - prec2_last_switch_ms) >= 2500;

  // Detect trend
  bool prev_prec2_temp_rising = prec2_temp_rising;

  if (temp_prec2_avg > prev_temp_prec2_avg + 0.12) {
    prec2_temp_rising = true;
  }
  else if (temp_prec2_avg < prev_temp_prec2_avg - 0.12) {
    prec2_temp_rising = false;
  }

  // Track peak
  if (temp_prec2_avg > prec2_peak_temp) {
    prec2_peak_temp = temp_prec2_avg;
  }

  // -------------------------
  // STARTUP MODE
  // -------------------------
  if (prec2_startup_mode) {

    // Startup shutoff threshold:
    // shut off early enough to avoid the initial large overshoot
    float startup_off_threshold = temp_set_prec2 - 6.0;

    // If already rising and getting closer to setpoint, shut off even earlier
    if (prec2_error < 5.0 && prec2_temp_rising) {
      startup_off_threshold = temp_set_prec2 - 7.0;
    }

    // Turn ON if well below target
    if (!relay_prec2_on) {
      if (prec2_can_switch && temp_prec2_avg <= temp_set_prec2 - 8.0) {
        digitalWrite(RELAY_PREC2_PIN, LOW);   // ON
        relay_prec2_on = true;
        prec2_last_switch_ms = millis();
      }
    }
    // Turn OFF once startup threshold is reached
    else {
      if (prec2_can_switch && temp_prec2_avg >= startup_off_threshold) {
        digitalWrite(RELAY_PREC2_PIN, HIGH);  // OFF
        relay_prec2_on = false;
        prec2_last_switch_ms = millis();
      }
    }

    // Exit startup mode after first peak passes and enough off-time has elapsed
    if (!relay_prec2_on &&
        prev_prec2_temp_rising &&
        !prec2_temp_rising &&
        (millis() - prec2_last_switch_ms) >= prec2_startup_min_off_ms) {
      prec2_startup_mode = false;
      prec2_peak_seen = true;
      prec2_pulse_mode = false;
      prec2_last_pulse_end_ms = millis();
    }
  }

  // -------------------------
  // STEADY MODE
  // variable pulse-width reheating
  // -------------------------
  else {
    // Start reheating before dropping too low
    float prec2_on_threshold = temp_set_prec2 - 1.5;

    // -------------------------
    // End active pulse after assigned pulse width
    // -------------------------
    if (prec2_pulse_mode && relay_prec2_on) {
      if ((millis() - prec2_pulse_start_ms) >= prec2_current_pulse_on_ms) {
        digitalWrite(RELAY_PREC2_PIN, HIGH);   // OFF
        relay_prec2_on = false;
        prec2_pulse_mode = false;
        prec2_last_switch_ms = millis();
        prec2_last_pulse_end_ms = millis();
      }
    }

    // -------------------------
    // Start a new pulse if needed
    // -------------------------
    if (!relay_prec2_on && !prec2_pulse_mode) {
      bool cooldown_done =
          (millis() - prec2_last_pulse_end_ms) >= prec2_pulse_cooldown_ms;

      if (cooldown_done &&
          temp_prec2_avg <= prec2_on_threshold &&
          !prec2_temp_rising) {

        float drop_deg = temp_set_prec2 - temp_prec2_avg;

        // Variable pulse law for higher-temp operation
        prec2_current_pulse_on_ms =
            1000 + (unsigned long)(600.0 * (drop_deg - 1.5));

        // Clamp pulse width
        if (prec2_current_pulse_on_ms < 1000) {
          prec2_current_pulse_on_ms = 1000;
        }
        if (prec2_current_pulse_on_ms > 3200) {
          prec2_current_pulse_on_ms = 3200;
        }

        digitalWrite(RELAY_PREC2_PIN, LOW);   // ON
        relay_prec2_on = true;
        prec2_pulse_mode = true;
        prec2_pulse_start_ms = millis();
        prec2_last_switch_ms = millis();
      }
    }

    // -------------------------
    // Safety shutoff if heating and already rising near target
    // -------------------------
    if (relay_prec2_on &&
        prec2_temp_rising &&
        temp_prec2_avg >= temp_set_prec2 - 2.2) {
      digitalWrite(RELAY_PREC2_PIN, HIGH);   // OFF
      relay_prec2_on = false;
      prec2_pulse_mode = false;
      prec2_last_switch_ms = millis();
      prec2_last_pulse_end_ms = millis();
    }
  }

  prev_temp_prec2_avg = temp_prec2_avg;
  // ----------------------------------------------------------
  // Substrate heater: trend-based control
  // ----------------------------------------------------------
  float substrate_off_threshold = temp_set_substrate - 0.8;
  float substrate_on_threshold  = temp_set_substrate + 1.0;
  float substrate_trend_epsilon = 0.25;

  bool substrate_can_switch =
    (millis() - substrate_last_switch_ms) >= substrate_lockout_ms;

  bool prev_substrate_temp_rising = substrate_temp_rising;

  if (temp_substrate_avg > prev_temp_substrate_avg + substrate_trend_epsilon)
  {
    substrate_temp_rising = true;
  }
  else if (temp_substrate_avg < prev_temp_substrate_avg - substrate_trend_epsilon)
  {
    substrate_temp_rising = false;
  }

  if (relay_substrate_on)
  {
    if (substrate_can_switch &&
        substrate_temp_rising &&
        temp_substrate_avg >= substrate_off_threshold)
    {
      digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);   // OFF
      relay_substrate_on = false;
      substrate_last_switch_ms = millis();
      substrate_restart_armed = false;
    }
  }
  else
  {
    if (substrate_can_switch && temp_substrate_avg < substrate_off_threshold)
    {
      digitalWrite(RELAY_SUBSTRATE_PIN, LOW);    // ON
      relay_substrate_on = true;
      substrate_last_switch_ms = millis();
      substrate_restart_armed = false;
    }
    else
    {
      if (prev_substrate_temp_rising && !substrate_temp_rising)
      {
        substrate_restart_armed = true;
      }

      if (substrate_can_switch &&
          substrate_restart_armed &&
          temp_substrate_avg <= substrate_on_threshold)
      {
        digitalWrite(RELAY_SUBSTRATE_PIN, LOW);  // ON
        relay_substrate_on = true;
        substrate_last_switch_ms = millis();
        substrate_restart_armed = false;
      }
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
        active_pgauge_samples = 20;  // fast sampling during valve
        
        if (valve_state == 0)
        {
            digitalWrite(which_valve == 1 ? RELAY_VALVE1_PIN :
                        which_valve == 2 ? RELAY_VALVE2_PIN : RELAY_VALVE3_PIN, HIGH);
            Serial.println("V: Pulsing valve " + String(which_valve));
            valve_timer_start = now;
            valve_state = 1;
        }
        else if (valve_state == 1 && (now - valve_timer_start >= pulse_time))
        {
            digitalWrite(which_valve == 1 ? RELAY_VALVE1_PIN :
                        which_valve == 2 ? RELAY_VALVE2_PIN : RELAY_VALVE3_PIN, LOW);
            Serial.println("V: Purging line");
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

  if (P_Torr > 100)
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
// ============================================================
void loop()
{
  busy_prev = busy;
  unsigned long now = millis();

  if (Serial.available() > 0)
  {
    Serial.println("Got command!");
    char s[100] = {0};
    String inputString = Serial.readStringUntil('\n');
    strcpy(s, inputString.c_str());

    Serial.println(s);
    int result = 0;

    if (s[0] == 's')
    {
      Serial.println("EMERGENCY STOP command received! Closing all valves, stopping heating.");

      digitalWrite(RELAY_SUBSTRATE_PIN, HIGH);
      digitalWrite(RELAY_DELIVERY_PIN, HIGH);
      digitalWrite(RELAY_PREC1_PIN, HIGH);
      digitalWrite(RELAY_PREC2_PIN, HIGH);

      digitalWrite(RELAY_VALVE1_PIN, LOW);
      digitalWrite(RELAY_VALVE2_PIN, LOW);
      digitalWrite(RELAY_VALVE3_PIN, LOW);

      while (1) { }
    }
    else if (s[0] == 'r')
    {
      num_pulse = 0;
      which_valve = 0;
      Serial.println("RESET command received! Resetting state!");
    }
    else
    {
      if (s[0] == 't')
      {
        tc_active = 1;
        result = sscanf(
          s,
          "t%d;%d;%d;%d",
          &temp_set_delivery,
          &temp_set_prec1,
          &temp_set_prec2,
          &temp_set_substrate
        );
      Serial.print("SETPOINTS:");
      Serial.print(temp_set_delivery); Serial.print(",");
      Serial.print(temp_set_prec1); Serial.print(",");
      Serial.print(temp_set_prec2); Serial.print(",");
      Serial.println(temp_set_substrate);
      }

      else if (s[0] == 'v')
      {
        if (busy)
        {
          Serial.println("COMMAND IGNORED. Wait for previous command to finish, or issue RESET.");
        }
        else
        {
          busy = true;
          result = sscanf(
            s,
            "v%u;%u;%u;%u",
            &which_valve,
            &num_pulse,
            &pulse_time,
            &purge_time
          );
        active_pgauge_samples = max(10, min(50, (int)(pulse_time / 10)));
        }
      }
      else
      {
        Serial.println("INVALID COMMAND!");
        return;
      }

      if (result != 4)
      {
        Serial.println("MISFORMATTED COMMAND! sscanf result: ");
        Serial.println(result);
        return;
      }
      else
      {
        Serial.println("Starting command!");
      }
    }
  }

  precursorValveActuation();

  if (num_pulse == 0)
  {
    busy = false;
    if (busy_prev) Serial.println("Previous command has completed. Ready for new command.");
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
    readCVM211PressureTorr();
  }

  if (now - lastFlowSend >= FLOW_INTERVAL_MS)
  {
    lastFlowSend = now;
    readD6FWFlow();
  }
}
