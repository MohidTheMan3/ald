// ald_manual_control.ino
// This file is for manual control of the ALD system.
// While running manual_gui.py on the control computer, users can set temperature points for heating elements and actuate valves one at a time.
// Useful for testing individual components of the ALD system.

// CMU Hacker Fab 2025
// Joel Gonzalez, Haewon Uhm, Atharva Raut

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

// set temperature setpoint for heating elements
int tc_active = 0;
int temp_sp2 = 0; // delivery line
int temp_sp3 = 0; // precursor 1
int temp_sp4 = 0; // precursor 2
int temp_sp5 = 0; // substrate heater

unsigned int which_valve = 0; // 1, 2, or 3
unsigned int num_pulse = 0; // positive integer value
unsigned int pulse_time = 0; // ms
unsigned int purge_time = 0; // ms

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
  tc2_readings[index] = current_reading[1];
  tc3_readings[index] = current_reading[2];
  tc4_readings[index] = current_reading[3];
  tc5_readings[index] = current_reading[4];
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
    if (tc2_avg > temp_sp2)
      digitalWrite(RELAY2_PIN, HIGH);
    else
      digitalWrite(RELAY2_PIN, LOW);

    if (tc3_avg > temp_sp3)
      digitalWrite(RELAY3_PIN, HIGH);
    else
      digitalWrite(RELAY3_PIN, LOW);

    if (tc4_avg > temp_sp4)
      digitalWrite(RELAY4_PIN, HIGH);
    else
      digitalWrite(RELAY4_PIN, LOW);

    if (tc5_avg > temp_sp5)
      digitalWrite(RELAY1_PIN, HIGH);
    else
      digitalWrite(RELAY1_PIN, LOW);
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

void loop()
{ 
  // command parsing code
  if ((Serial.available() > 0))
  {
    Serial.println("Got command!");
    char s[100] = {0};
    String inputString = Serial.readStringUntil('\n'); // Read until newline character
    strcpy(s, inputString.c_str());
    
    // s = "s";              // STOP command: exit loop 
    // s = "r";              // RESET command: reset pulse counter 
    // s = "t;100;200;150;90";  // example temp. command
    // s = "v;2;5;1000;3000";   // example valve command

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
        if (num_pulse != 0)
        {
          Serial.println("COMMAND IGNORED. Wait for previous command to finish, or issue RESET.")
        } else
        {
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
    Serial.println("Previous command has completed. Ready for new command.")
  }
  // need to ensure that python doesn't send new command till acknowledgement is received
  // TODO: @Modid, suggest best method to convey it back to python
  // Current implementation: simply ignore new pulse commands when previous is ongoing

  // heating control loop
  readThermocouples();
  actuateHeatingElements();
}