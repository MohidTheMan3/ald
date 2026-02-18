// ald_manual_control.ino
// This file is for manual control of the ALD system.
// While running manual_gui.py on the control computer, users can set temperature points for heating elements and actuate valves one at a time.
// Useful for testing individual components of the ALD system.

// CMU Hacker Fab 2025
// Joel Gonzalez, Haewon Uhm, Atharva Raut
// Updated 2025-Dec-05
// Atharva: Pressure gauge reading verified in old GUI (also calibrated, only difference in 3rd significant digit being +-1)
// Atharva: Flow sensor reading verified in old GUI (close to max value with door closed, goes to )
// DO NOT flash old code since pin mappings have changed!!

// PIN ALLOCATION
// 0-1    Reserved (RX,TX)
// 2      RELAY1 (Substrate heater)
// 3-10   Thermocouples
// 11     RELAY2 (Delivery line tape)
// A0     Pressure Gauge
// A1-A2  Precursor tapes --> TODO: 12-13 and repurpose A1 for flow sensor
// A3-A5  Valves
// toggle this if you want the system to do nothing
#define DO_NOTHING 0
// relay pins to control relays for heating elements
// relay 1 -> substrate heater
// relay 2 -> delivery line tape
// relay 3 -> precursor 1
// relay 4 -> precursor 2
#define RELAY1_PIN 2
#define RELAY2_PIN 11      // changed from A0 to use that as an analog read pin for pressure gauge
#define RELAY3_PIN 12
#define RELAY4_PIN A2
// relay pins to control relays for ALD valves
// relay 6 -> valve 1
// relay 7 -> valve 2
// relay 8 -> valve 3
#define RELAY6_PIN A3
#define RELAY7_PIN A4
#define RELAY8_PIN A5
// Stinger CVM211GBL pressure gauge analog read
// Analog read pin (rewired from previous relay2_pin)
#define PGAUGE_PIN A0               // Wire to DB5 pin on CVM211GBL
#define ADC_REF_V 5.0               // Arduino Uno default ADC reference
#define CVM211_DIVIDER_RATIO 1.622    // (Rtop+Rbottom)/Rbottom of resistive divider; 1.622 for ideal 5.1:8.2 divider (max voltage at pin=5V)
// ratio calibrated for better representation of actual resistor values
#define FLOW_SENSE_PIN A1               // From flow sensor; ensure 5V voltage regulator or divide 5.7V-->5V for safety
#define D6FW_DIVIDER_RATIO 2    // 1 if using 5V regulator, ~1.14 if dividing 5.7V-->5V
const double D6FW04A1_LUT[5] = {1.00,1.58,2.88,4.11,5.00};   // from datasheet; corresponds to flow rate 0-4 m/s in increments of 1 m/s
// we will use linear extrapolation based on the LUT reading for non-linear output readings


#include <Adafruit_MAX31855.h>

int32_t rawData = 0;

const int num_samples_pgauge = 200;
const int num_samples_flow_sense = 100;

const int num_samples = 10;
double tc1_readings[num_samples];
double tc2_readings[num_samples];
double tc3_readings[num_samples];
double tc4_readings[num_samples];
double tc5_readings[num_samples];
double tc6_readings[num_samples];
double tc7_readings[num_samples];
double tc8_readings[num_samples];

int index = 0;
int count = 0;

double tc1_avg = 0.0;
double tc2_avg = 0.0;
double tc3_avg = 0.0;
double tc4_avg = 0.0;
double tc5_avg = 0.0;
double tc6_avg = 0.0;
double tc7_avg = 0.0;
double tc8_avg = 0.0;

double current_reading[8];

// pins 3-10
// K-type: index 0,1,2,3 (pins 3,4,5,6) -> tc5, tc2, tc3, tc4
// J-type: index 4,5,6,7 (pins 7,8,9,10)
Adafruit_MAX31855 thermocouples[8] = {Adafruit_MAX31855(3), Adafruit_MAX31855(4), Adafruit_MAX31855(5), Adafruit_MAX31855(6), Adafruit_MAX31855(7), Adafruit_MAX31855(8), Adafruit_MAX31855(9), Adafruit_MAX31855(10)};

// set temperature setpoint for heating elements
int tc_active = 0;
int temp_sp2 = 0; // delivery line (K-type, pin 4)
int temp_sp3 = 0; // precursor 1 (K-type, pin 5)
int temp_sp4 = 0; // precursor 2 (K-type, pin 6)
int temp_sp5 = 0; // substrate heater (K-type, pin 3 - FIRST K-TYPE PORT)

// Hysteresis offsets for heating elements (in °C)
// Turn ON when temp < setpoint - HYSTERESIS_OFFSET
// Turn OFF when temp > setpoint + HYSTERESIS_OFFSET
const int HYSTERESIS_TC2 = 5;  // delivery line tape
const int HYSTERESIS_TC3 = 5;  // precursor 1
const int HYSTERESIS_TC4 = 5;  // precursor 2
const int HYSTERESIS_TC5 = 5;  // substrate heater

// Track relay states for hysteresis logic
bool relay2_on = false;
bool relay3_on = false;
bool relay4_on = false;
bool relay1_on = false;

unsigned int which_valve = 0; // 1, 2, or 3
unsigned int num_pulse = 0; // positive integer value
unsigned int pulse_time = 0; // ms
unsigned int purge_time = 0; // ms

bool busy = false;
bool busy_prev = false;

void setup()
{
  if (DO_NOTHING)
    while(1);

  Serial.begin(9600);

  // relay pins
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  pinMode(RELAY6_PIN, OUTPUT);
  pinMode(RELAY7_PIN, OUTPUT);
  pinMode(RELAY8_PIN, OUTPUT);

  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
  digitalWrite(RELAY3_PIN, HIGH);
  digitalWrite(RELAY4_PIN, HIGH);
  digitalWrite(RELAY6_PIN, LOW);
  digitalWrite(RELAY7_PIN, LOW);
  digitalWrite(RELAY8_PIN, LOW);  // Active HIGH MOSFET

  // pressure gauge (analog input: 1-8 V, scaled down to 5V range)
  pinMode(PGAUGE_PIN, INPUT);  
  pinMode(FLOW_SENSE_PIN, INPUT);  

  // K-type: pins 3,4,5,6
  // J-type: pins 7,8,9,10
  for (int i=0;i<7;i++)
    thermocouples[i].begin();

  Serial.begin(9600);
  while (!Serial)
    Serial.println("Waiting for serial...");

  Serial.println("Setup complete!");
}

// takes moving average of thermocouple data
void readThermocouples()
{ 
  for (int i=0; i<7; ++i)
  {
    double curr_val = thermocouples[i].readCelsius();
    if (!isnan(curr_val)) // check for faulty NaN readings
      current_reading[i] = thermocouples[i].readCelsius();
  }

  // tc1_readings[index] = current_reading[0];
  tc2_readings[index] = current_reading[3];
  tc3_readings[index] = current_reading[2];
  tc4_readings[index] = current_reading[1];
  tc5_readings[index] = current_reading[0];
  // tc6_readings[index] = current_reading[5];
  // tc7_readings[index] = current_reading[6];
  // tc8_readings[index] = current_reading[7];
    
  index = (index + 1) % num_samples;

  if (count < num_samples)
    count = count + 1;

  double sum1 = 0, sum2 = 0, sum3 = 0, sum4 = 0, sum5 = 0, sum6 = 0, sum7 = 0, sum8 = 0;
  for (int i = 0; i<count; i= i+1)
  {
    // sum1 += tc1_readings[i];
    sum2 += tc2_readings[i];
    sum3 += tc3_readings[i];
    sum4 += tc4_readings[i];
    sum5 += tc5_readings[i];
    // sum6 += tc6_readings[i];
    // sum7 += tc7_readings[i];
    // sum8 += tc8_readings[i];
  }

  // tc1_avg = sum1 / count;
  tc2_avg = sum2 / count;
  tc3_avg = sum3 / count;
  tc4_avg = sum4 / count;
  tc5_avg = sum5 / count;
  // tc6_avg = sum6 / count;
  // tc7_avg = sum7 / count;
  // tc8_avg = sum8 / count;

  Serial.println("T: " + String(tc2_avg) + "; " + String(tc3_avg) + "; " + String(tc4_avg) + ";" + String(tc5_avg));
  delay(1000);
}

void actuateHeatingElements()
{
  if (tc_active)
  {
    // TC2 (delivery line tape) - hysteresis control
    if (tc2_avg < (temp_sp2 - HYSTERESIS_TC2) && !relay2_on)
    {
      digitalWrite(RELAY2_PIN, LOW);   // Turn heater ON
      relay2_on = true;
    }
    else if (tc2_avg > (temp_sp2 + HYSTERESIS_TC2) && relay2_on)
    {
      digitalWrite(RELAY2_PIN, HIGH);  // Turn heater OFF
      relay2_on = false;
    }

    // TC3 (precursor 1) - hysteresis control
    if (tc3_avg < (temp_sp3 - HYSTERESIS_TC3) && !relay3_on)
    {
      digitalWrite(RELAY3_PIN, LOW);   // Turn heater ON
      relay3_on = true;
    }
    else if (tc3_avg > (temp_sp3 + HYSTERESIS_TC3) && relay3_on)
    {
      digitalWrite(RELAY3_PIN, HIGH);  // Turn heater OFF
      relay3_on = false;
    }

    // TC4 (precursor 2) - hysteresis control
    if (tc4_avg < (temp_sp4 - HYSTERESIS_TC4) && !relay4_on)
    {
      digitalWrite(RELAY4_PIN, LOW);   // Turn heater ON
      relay4_on = true;
    }
    else if (tc4_avg > (temp_sp4 + HYSTERESIS_TC4) && relay4_on)
    {
      digitalWrite(RELAY4_PIN, HIGH);  // Turn heater OFF
      relay4_on = false;
    }

    // TC5 (substrate heater) - hysteresis control
    if (tc5_avg < (temp_sp5 - HYSTERESIS_TC5) && !relay1_on)
    {
      digitalWrite(RELAY1_PIN, LOW);   // Turn heater ON
      relay1_on = true;
    }
    else if (tc5_avg > (temp_sp5 + HYSTERESIS_TC5) && relay1_on)
    {
      digitalWrite(RELAY1_PIN, HIGH);  // Turn heater OFF
      relay1_on = false;
    }
  }
}

void precursorValveActuation()
{
  // actual purge time between pulses may be slightly higher when using "if" conditional instead of "when"
  if(num_pulse>0)
  {
    switch(which_valve)
    {
      case 1:
        Serial.println("V: Pulsing valve 1");
        digitalWrite(RELAY6_PIN, HIGH);
        delay(pulse_time);

        Serial.println("V: Purging line");
        digitalWrite(RELAY6_PIN, LOW);
        delay(purge_time);
        num_pulse--;
        break;

      case 2:
        Serial.println("V: Pulsing valve 2");
        digitalWrite(RELAY7_PIN, HIGH);
        delay(pulse_time);

        Serial.println("V: Purging line");
        digitalWrite(RELAY7_PIN, LOW);
        delay(purge_time);
        num_pulse--;
        break;

      case 3:
        Serial.println("V: Pulsing valve 3");
        digitalWrite(RELAY8_PIN, HIGH);
        delay(pulse_time);

        Serial.println("V: Purging line");
        digitalWrite(RELAY8_PIN, LOW);
        delay(purge_time);
        num_pulse--;
        break;

      default:
        Serial.println("V: No valve selected");
        return;
    }
  } else
  { 
    // close valves when no pulses required (precautionary)
    digitalWrite(RELAY6_PIN, LOW);
    digitalWrite(RELAY7_PIN, LOW);
    digitalWrite(RELAY8_PIN, LOW);
  }
}

// Pressure Gauge Functions (specifically for Stinger CVM211GBL)
// Read raw analog and return reconstructed gauge pin voltage
static double cvm211_readGaugeVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < num_samples_pgauge; ++i) acc += analogRead(PGAUGE_PIN);
  double adcCounts = acc / (double)num_samples_pgauge;
  double vadc = (adcCounts * ADC_REF_V) / 1023.0;   // 10-bit ADC
  double vgauge = vadc * CVM211_DIVIDER_RATIO;      // compensate for resistive divider
  return vgauge;
}

// Convert gauge voltage (log-linear model, CVM211GBL) to pressure (Torr): P = 10^(V - 5)
static double cvm211_logLinear_toTorr(double v) {
  if (v < 1.0) v = 1.0;            // clamp to CVM211GBL range
  if (v > 8.0) v = 8.0;
  return pow(10.0, v - 5.0);
}

// Public helper: read pressure in Torr (log-linear only)
void readCVM211PressureTorr() {
  double v = cvm211_readGaugeVolts();
  double P_Torr = cvm211_logLinear_toTorr(v);
  double P_mTorr = 1000*P_Torr;
  if (P_Torr > 100) Serial.println("P: " + String(P_Torr) + " Torr");
  else Serial.println("P: " + String(P_mTorr) + " mTorr");
}
// END pressure gauge functions

// Flow Sensor Functions (specifically for MEMS Flow Sensor D6F-W10A1)
// Read raw analog and return reconstructed pin voltage
static double d6fw_readGaugeVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < num_samples_flow_sense; ++i) acc += analogRead(FLOW_SENSE_PIN);
  double adcCounts = acc / (double)num_samples_flow_sense;
  double vadc = (adcCounts * ADC_REF_V) / 1023.0;   // 10-bit ADC
  double vgauge = vadc * D6FW_DIVIDER_RATIO;      // compensate for resistive divider if any
  return vgauge;
}

// Convert flow sensor voltage (log-linear model, CVM211GBL) to pressure (Torr): P = 10^(V - 5)
static double d6fw_nonLinear_to_mps(double v) {
  double flow_rate_extrapolated;
  int low = 0;
  int high = 4;
  for (int i=0; i<=4; ++i){
    if (D6FW04A1_LUT[i] <= v) low = i;
    if (D6FW04A1_LUT[4-i] > v) high = (4-i);
  }
  if (D6FW04A1_LUT[high]==D6FW04A1_LUT[low]) flow_rate_extrapolated = 0;    // error?
  else flow_rate_extrapolated = ((v-D6FW04A1_LUT[low])*high + (D6FW04A1_LUT[high]-v)*low)/(D6FW04A1_LUT[high]-D6FW04A1_LUT[low]);    //linear extrapolation
  return flow_rate_extrapolated;
}

// Public helper: read flow rate in m/s (non-linear read relationship based on datasheet LUT)
void readD6FWFlow() {
  double v = d6fw_readGaugeVolts();
  double flow_rate = d6fw_nonLinear_to_mps(v);
  Serial.println("F: " + String(flow_rate) + " m/s");
}
// END flow sensor functions

void loop()
{ 
  busy_prev = busy;

  // command parsing code
  if ((Serial.available() > 0))
  {
    Serial.println("Got command!");
    char s[100] = {0};
    String inputString = Serial.readStringUntil('\n'); // Read until newline character
    strcpy(s, inputString.c_str());
    
    // s = "s";              // STOP command: exit loop 
    // s = "r";              // RESET command: reset pulse counter 
    // s = "t100;200;150;90";  // example temp. command
    // s = "v2;5;1000;3000";   // example valve command

    Serial.println(s);
    int result = 0;

    // stop command
    if (s[0] == 's')
    {
      Serial.println("EMERGENCY STOP command received! Closing all valves, Stopping heating! Shutdown");
      digitalWrite(RELAY1_PIN, HIGH);
      digitalWrite(RELAY2_PIN, HIGH);
      digitalWrite(RELAY3_PIN, HIGH);
      digitalWrite(RELAY4_PIN, HIGH);
      digitalWrite(RELAY6_PIN, LOW);
      digitalWrite(RELAY7_PIN, LOW);
      digitalWrite(RELAY8_PIN, LOW);  // Active HIGH MOSFET
      while(1){
        // do nothing - EMERGENCY STOP state
        // need to restart program to recover from this state
      }
    }
    // reset command
    else if (s[0] == 'r')
    {
      num_pulse = 0;
      which_valve = 0;
      Serial.println("RESET command received! Resetting state!");
    } else
    {
      // temperature command
      if (s[0] == 't')
      {
        tc_active = 1;
        result = sscanf(s, "t%d;%d;%d;%d", &temp_sp2, &temp_sp3, &temp_sp4, &temp_sp5);
      } else if (s[0] == 'v') // ALD valve command
      {
        if (busy) Serial.println("COMMAND IGNORED. Wait for previous command to finish, or issue RESET.");
        else
        {
          busy = true;
          result = sscanf(s, "v%u;%u;%u;%u", &which_valve, &num_pulse, &pulse_time, &purge_time);    
        }
      } else
      {
        Serial.println("INVALID COMMAND!");
        return;
      }

      // unable to parse command-line input properly
      if (result != 4)
      {
        Serial.println("MISFORMATTED COMMAND! sscanf result: ");
        Serial.println(result);
        return;
      } else {
        Serial.println("Starting command!");
      }
    }
  }
  
  // ALD valve actuation
  // moved outside the conditional so it runs passively when nothing in serial port
  precursorValveActuation();
  if (num_pulse == 0)
  {
    busy = false;
    if(busy_prev) Serial.println("Previous command has completed. Ready for new command.");
  }
  // need to ensure that python doesn't send new command till acknowledgement is received
  // TODO: @Modid, suggest best method to convey it back to python
  // Current implementation: simply ignore new pulse commands when previous is ongoing

  // heating control loop
  readThermocouples();
  actuateHeatingElements();

  // read pressure gauge and send to python
  readCVM211PressureTorr();

  // read flow sensor and send to python
  readD6FWFlow();
}