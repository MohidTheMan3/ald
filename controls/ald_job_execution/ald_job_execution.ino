// ald_job_execution.ino
// This file reads thermocouple values and uses them to actuate relays connected to heating elements in order to maintain them at a predetermined setpoint using bang-bang controls.
// It also actuates ALD valves based on set intervals.
// It reads user input to execute a "job", which is defined as follows:

// ALD job is defined as:
// Number of pulses for each valve (whole number)
// Duration of pulses for each valve (milliseconds)
// Temperature set points for each heating element (celsius)
// An example command looks like the following:
// 3000,8,500,12,200,4,1200,300,180,120,250
// This would mean:
// Purge time of 3000ms.
// Eight 500ms pulses for ALD valve 1.
// Twelve 200ms pulses for ALD valve 2.
// Four 1200ms pulses for ALD valve 3.
// IMPORTANT! The pulses will continue one after another,
// so we will see valve 1 -> valve 2 -> valve 3 with purging time in between each.
// The valves will remain CLOSED during the purging time to allow carrier gas to purge the lines.
// Then, thermocouple 1 is set at 300.0C. TC2 at 180.0C. TC3 at 120.0C. And TC4 at 250.0C.
// Note that a job must have data for every field.

// CMU Hacker Fab 2025
// Joel Gonzalez, Haewon Uhm

// toggle this if you want the system to do nothing
#define DO_NOTHING 0
// relay pins to control relays for heating elements
// relay 1 -> substrate heater
// relay 2 -> delivery line tape
// relay 3 -> precursor 1
// relay 4 -> precursor 2
#define RELAY1_PIN 2
#define RELAY2_PIN A0
#define RELAY3_PIN A1
#define RELAY4_PIN A2
// relay pins to control relays for ALD valves
// relay 6 -> valve 1
// relay 7 -> valve 2
// relay 8 -> valve 3
#define RELAY6_PIN A3
#define RELAY7_PIN A4
#define RELAY8_PIN A5

#include <Adafruit_MAX31855.h>

int32_t rawData = 0;

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
Adafruit_MAX31855 thermocouples[8] = {Adafruit_MAX31855(3), Adafruit_MAX31855(4), Adafruit_MAX31855(5), Adafruit_MAX31855(6), Adafruit_MAX31855(7), Adafruit_MAX31855(8), Adafruit_MAX31855(9), Adafruit_MAX31855(10)};

// set temperature setpoints for heating elements
int temp_sp2 = 0.0; // 
bool sp2_reached = LOW;
int temp_sp3 = 0.0;
bool sp3_reached = LOW;
int temp_sp4 = 0.0; // 
bool sp4_reached = LOW;
int temp_sp5 = 0.0; // substrate heater
bool sp5_reached = LOW;
bool all_sp_reached = HIGH; // check for all temperature setpoints

unsigned int current_valve = 1;
unsigned int valve_actuation_started = 0;

unsigned int num_pulse1 = 0;
unsigned long previousMillis_1 = 0;
unsigned long pulse_time1 = 0; // Interval for toggling the pin (in milliseconds)
bool outputState_1 = HIGH;

unsigned int num_pulse2 = 0;
unsigned long previousMillis_2 = 0;
unsigned long pulse_time2 = 0; // Interval for toggling the pin (in milliseconds)
bool outputState_2 = HIGH;

unsigned int num_pulse3 = 0;
unsigned long previousMillis_3 = 0;
unsigned long pulse_time3 = 0; // Interval for toggling the pin (in milliseconds)
bool outputState_3 = HIGH;

unsigned long previousMillis_4 = 0;
unsigned int purge_time = 0; // milliseconds
bool purging = LOW;

bool JOB_IN_PROGRESS = LOW;
float prev_tc4_avg = 0.0;
bool temp_rising = false;

bool relay2_on = false;
bool relay2_restart_armed = false;

unsigned long relay2_last_switch_ms = 0;
const unsigned long relay2_lockout_ms = 5000;

void setup()
{
  if (DO_NOTHING)
    while(1);

  Serial.begin(115200);

  // relay pins
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  pinMode(RELAY6_PIN, OUTPUT);
  pinMode(RELAY7_PIN, OUTPUT);
  pinMode(RELAY8_PIN, OUTPUT);

  // K-type: pins 3,4,5,6
  // J-type: pins 7,8,9,10
  for (int i=0;i<7;i++)
    thermocouples[i].begin();

  Serial.begin(115200);
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

  tc1_readings[index] = current_reading[0];
  tc2_readings[index] = current_reading[1];
  tc3_readings[index] = current_reading[2];
  tc4_readings[index] = current_reading[3];
  tc5_readings[index] = current_reading[4];
  tc6_readings[index] = current_reading[5];
  tc7_readings[index] = current_reading[6];
  tc8_readings[index] = current_reading[7];
    
  index = (index + 1) % num_samples;

  if (count < num_samples)
    count = count + 1;

  double sum1 = 0, sum2 = 0, sum3 = 0, sum4 = 0, sum5 = 0, sum6 = 0, sum7 = 0, sum8 = 0;
  for (int i = 0; i<count; i= i+1)
  {
    sum1 += tc1_readings[i];
    sum2 += tc2_readings[i];
    sum3 += tc3_readings[i];
    sum4 += tc4_readings[i];
    sum5 += tc5_readings[i];
    sum6 += tc6_readings[i];
    sum7 += tc7_readings[i];
    sum8 += tc8_readings[i];
  }

  tc1_avg = sum1 / count;
  tc2_avg = sum2 / count;
  tc3_avg = sum3 / count;
  tc4_avg = sum4 / count;
  tc5_avg = sum5 / count;
  tc6_avg = sum6 / count;
  tc7_avg = sum7 / count;
  tc8_avg = sum8 / count;

  Serial.println(String(tc1_avg) + "; " + String(tc2_avg) + "; " + String(tc3_avg) + "; " + String(tc4_avg) + ";" + String(tc5_avg) + ";" + String(tc6_avg) + ";" + String(tc7_avg) + ";" + String(tc8_avg) + ";");
}

void actuateHeatingElements()
{
  float off_threshold = temp_sp2 - 1.3;   // e.g. 118.7 for setpoint 120
  float on_threshold  = temp_sp2 + 1.0;   // e.g. 121.0
  float trend_epsilon = 0.25;

  bool can_switch = (millis() - relay2_last_switch_ms) >= relay2_lockout_ms;

  // Update trend
  bool prev_temp_rising = temp_rising;

  if (tc4_avg > prev_tc4_avg + trend_epsilon)
  {
      temp_rising = true;
  }
  else if (tc4_avg < prev_tc4_avg - trend_epsilon)
  {
      temp_rising = false;
  }

  if (relay2_on)
  {
      // Turn OFF early while heating up
      if (can_switch && temp_rising && tc4_avg >= off_threshold)
      {
          digitalWrite(RELAY4_PIN, HIGH);   // heater OFF
          relay2_on = false;
          relay2_last_switch_ms = millis();
          relay2_restart_armed = false;
      }
  }
  else
  {
      // Cold-start case
      if (can_switch && tc4_avg < off_threshold)
      {
          digitalWrite(RELAY4_PIN, LOW);    // heater ON
          relay2_on = true;
          relay2_last_switch_ms = millis();
          relay2_restart_armed = false;
      }
      else
      {
          // Arm restart once temperature has genuinely turned around
          if (prev_temp_rising && !temp_rising)
          {
              relay2_restart_armed = true;
          }

          // Restart on the way down
          if (can_switch && relay2_restart_armed && tc4_avg <= on_threshold)
          {
              digitalWrite(RELAY4_PIN, LOW);   // heater ON
              relay2_on = true;
              relay2_last_switch_ms = millis();
              relay2_restart_armed = false;
          }
      }
  }

  prev_tc4_avg = tc4_avg;

  if (tc3_avg > temp_sp3)
  {
    digitalWrite(RELAY3_PIN, HIGH);
    sp3_reached = 1; // hit the setpoint
  }
  else
  {
    digitalWrite(RELAY3_PIN, LOW);
  }

  if (tc4_avg > temp_sp4)
  {
    digitalWrite(RELAY4_PIN, HIGH);
    sp4_reached = 1; // hit the setpoint
  }
  else
  {
    digitalWrite(RELAY4_PIN, LOW);
  }

  if (tc5_avg > temp_sp5)
  {
    digitalWrite(RELAY1_PIN, HIGH);
    sp5_reached = 1; // hit the setpoint
  }
  else
  {
    digitalWrite(RELAY1_PIN, LOW);
  }

  if (sp2_reached && sp3_reached && sp4_reached && sp5_reached)
    all_sp_reached = HIGH;
}

// close ALD valves to allow for system purge using carrier gas
void closeALDValves()
{
  digitalWrite(RELAY6_PIN, HIGH);
  digitalWrite(RELAY7_PIN, HIGH);
  digitalWrite(RELAY8_PIN, HIGH);
}

void precursorValveActuation()
{
  // valve actuation
  unsigned long currentMillis = millis();

  // valve 1
  if ((currentMillis - previousMillis_1 >= pulse_time1) && (num_pulse1 > 0) && (current_valve == 1)) {
    // Save the last time you toggled the pin
    previousMillis_1 = currentMillis;

    // Toggle output pin and set the output pin to the new state
    outputState_1 = !outputState_1;
    digitalWrite(RELAY6_PIN, outputState_1);

    // we just closed the valve, so allow for purging
    if ((outputState_1 == HIGH))
    {
      // delay(purge_time);
      num_pulse1--;
      current_valve = 2; // move to next valve
    }
  }

  // valve 2
  if ((currentMillis - previousMillis_2 >= pulse_time2) && (num_pulse2 > 0) && (current_valve == 2)) {
    // Save the last time you toggled the pin
    previousMillis_2 = currentMillis;

    // Toggle output pin and set the output pin to the new state
    outputState_2 = !outputState_2;
    digitalWrite(RELAY7_PIN, outputState_2);

    // we just closed the valve, so allow for purging
    if ((outputState_2 == HIGH))
    {
      // delay(purge_time);
      num_pulse2--;
      current_valve = 3; // move to next valve
    }
  }

  // valve 3
  if ((currentMillis - previousMillis_3 >= pulse_time3) && (num_pulse3 > 0) && (current_valve == 3)) {
    // Save the last time you toggled the pin
    previousMillis_3 = currentMillis;

    // Toggle output pin and set the output pin to the new state
    outputState_3 = !outputState_3;
    digitalWrite(RELAY8_PIN, outputState_3);

    // we just closed the valve, so allow for purging
    if ((outputState_3 == HIGH))
    {
      // delay(purge_time);
      num_pulse3--;
      current_valve = 1; // move to next valve
    }
  }
}

void loop()
{
  if (!JOB_IN_PROGRESS)
  {
    closeALDValves();
    
    // job parsing code
    if ((Serial.available() > 0))
    {
      Serial.println("Got job!");
      char s[100] = {0};
      String inputString = Serial.readStringUntil('\n'); // Read until newline character
      strcpy(s, inputString.c_str());
      
      // s = "3000,8,500,12,200,4,1200,300,180,120,250"; // example job

      Serial.println(s);
      int result = sscanf(s, "%u;%u;%lu;%u;%lu;%u;%lu;%d;%d;%d;%d", &purge_time, &num_pulse1, &pulse_time1, &num_pulse2, &pulse_time2, &num_pulse3, &pulse_time3, &temp_sp2, &temp_sp3, &temp_sp4, &temp_sp5);

      // unable to parse command-line input properly
      if (result != 11)
      {
        Serial.println("BAD JOB! sscanf result: ");
        Serial.println(result);
        return;
      } else {
        Serial.println("Starting job!");
        JOB_IN_PROGRESS = HIGH;
      }
    }
  }

  // run job
  if (JOB_IN_PROGRESS)
  {
    // heating control loop
    readThermocouples();
    actuateHeatingElements();

    // ALD valve control once temperature targets are hit
    if (all_sp_reached)
    {
      if (!valve_actuation_started)
      {
        Serial.println("Enter anything to begin ALD valve actuation...");
        while(Serial.available() == 0);
        valve_actuation_started = 1;
      }

      precursorValveActuation();
    }

    // check if all ALD pulses have finished
    if ((num_pulse1 == 0) && (num_pulse2 == 0) && (num_pulse3 == 0))
    {
      JOB_IN_PROGRESS = 0;
      Serial.println("Job is done!");
      while(1);
    }
  }
}
