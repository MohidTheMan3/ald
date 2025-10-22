# ALD Control System

Modern Python control software for Atomic Layer Deposition equipment at CMU Hacker Fab.

## Quick Start

```bash
# Install dependencies
pip install PyQt6 PyQt6-Charts qasync pydantic pyserial-asyncio pytest pytest-asyncio pyyaml

# Run GUI
python ald_gui.py
```

## Motivation

### The Problem

The original Tkinter-based GUI for our ALD system suffered from critical reliability issues that made it unsuitable for lab use:

- **Unpredictable crashes** - Threading implementation caused the GUI to freeze or crash during experiments
- **Wrong values sent** - Manual string parsing occasionally sent corrupted commands to hardware
- **No input validation** - Invalid parameters could damage equipment or ruin samples
- **Poor error handling** - Crashes provided no useful debugging information
- **Difficult to maintain** - Threading bugs were hard to reproduce and fix

These issues meant users couldn't trust the software during expensive deposition runs, wasting time and materials.

### Our Solution

This complete rewrite addresses all reliability issues while adding new capabilities:

- **Async architecture** - Replaces threading with asyncio for stable, non-blocking communication
- **Input validation** - Pydantic models prevent invalid commands before they reach hardware
- **Professional GUI** - PyQt6 provides a modern, crash-resistant interface
- **Real-time monitoring** - Temperature graphs and status indicators for better process control
- **Safety features** - Emergency stop and reset functions for lab safety
- **Comprehensive logging** - All operations logged for troubleshooting and reproducibility


### Impact on Hacker Fab Workflow

```
Recipe → ALD Deposition →  Characterization
        (This software)│    (Analysis)
```

This software is the critical middle step that enables:
- Thin film deposition for MEMS, sensors, and nanodevices
- Controlled growth of Al₂O₃, TiO₂, ZnO, and other materials
- Reproducible results through validated parameter control

---

## Features

### Current Features (v1.0)

- Modern PyQt6 GUI with professional, stable interface
- Real-time temperature monitoring with live graphs of 4 thermocouples
- Input validation to prevent equipment damage from invalid commands
- Emergency stop for immediate system lockdown
- Async serial communication for non-blocking, reliable Arduino control
- Comprehensive logging of all operations for troubleshooting
- Reset function for soft reset without full emergency stop

### Interface Overview

**Main Control Interface:**
```
Status: Connected to COM3
[Connect] [Disconnect] [ESTOP] [Reset]
Valve Control   Temperature 
Valve ID: [1]    TC2: [100] °C
Pulses: [10]     TC3: [150] °C
Pulse: [100]     TC4: [200] °C
Purge: [4000]    TC5: [250] °C
Send Command      Send Command
```

**Temperature Monitoring:**
- Real-time graph updates every 500ms
- Auto-scaling axes
- Color-coded lines for each sensor

---

## Installation

### Prerequisites

- Python 3.9 or newer
- Arduino Uno with uploaded firmware
- Windows, Mac, or Linux

### Step-by-Step Installation

#### Method 1: Automatic (Recommended)

**Windows:**
```batch
python -m pip install PyQt6 PyQt6-Charts qasync pydantic pyserial-asyncio pytest pytest-asyncio pyyaml
```

**Mac/Linux:**
```bash
pip3 install PyQt6 PyQt6-Charts qasync pydantic pyserial-asyncio pytest pytest-asyncio pyyaml
```

#### Method 2: From requirements.txt

```bash
pip install -r requirements.txt
```

#### Verify Installation

```bash
python -c "from ald_controller import ALDController; print('Installation successful')"
```

### Arduino Setup

1. Open `ald_manual_control.ino` in Arduino IDE
2. Select **Tools → Board → Arduino Uno**
3. Select **Tools → Port → [Your COM Port]**
4. Click **Upload**
5. Wait for "Done uploading"

---

## Usage

### Quick Start

1. Connect Arduino to computer via USB
2. Run GUI: Run `python ald_gui.py`
3. Enter COM port (Usually COM3 on Windows, /dev/ttyUSB0 on Linux)
4. Click Connect
5. Use Control tab to send commands

### Daily Operation

#### Starting a Deposition Run

1. Set temperature targets for all heating zones
2. Click "Send Temperature Command"
3. Wait for temperatures to stabilize (monitor graph)
4. Configure valve parameters (pulses, timing)
5. Click "Send Valve Command"
6. Monitor progress in Arduino log

#### Emergency Procedures

**Normal Stop** (use this):
- Click orange "Reset Valves" button
- Clears current command
- System remains operational

**Emergency Stop** (Emergencies only):
- Click red "EMERGENCY STOP" button
- All valves close, heaters off
- Arduino locks (requires restart)

### Command Reference

#### Valve Control

```
Command format: v[valve_id];[num_pulses];[pulse_time_ms];[purge_time_ms]
Example: v1;10;100;4000
```

Parameters:
- `valve_id`: 1, 2, or 3
- `num_pulses`: 1-1000 (number of ALD cycles)
- `pulse_time_ms`: 10-10000 (precursor exposure time)
- `purge_time_ms`: 0-15000 (nitrogen purge duration)

#### Temperature Control

```
Command format: t[tc2];[tc3];[tc4];[tc5]
Example: t100;150;200;250
```

Parameters:
- All temperatures in °C (integers only)
- Valid range: 0-500°C
- TC2: Delivery line
- TC3: Precursor 1
- TC4: Precursor 2
- TC5: Substrate heater

#### Emergency Commands

- `s` - Emergency stop (locks Arduino)
- `r` - Reset pulse counter (soft reset)

---

## System Architecture

### Overview

```
┌───────────────────────────────────────────┐
│            ALD Control System             │
 ─────────────────────────────────────────┤
│                                           │
│  ┌──────────────┐     ┌────────────────┐  │
│  │  ald_gui.py  │────→│ald_controller  │  │
│  │   (PyQt6)    │     │     .py        │  │
│  │              │     │   (asyncio)    │  │
│  └──────────────┘     └────────────────┘  │
│         │                      │          │
│         └──────────────────────┘          │
│                    │                      │
└────────────────────┼──────────────────────┘
                     │ USB Serial
          ┌──────────┼──────────┐
          │    Arduino Uno      │
          │  (Firmware v2.0)    │
          └──────────┬──────────┘
                     │
     ┌───────────────┴────────────────┐
     │                                │
┌────┴────────┐              ┌────────┴────────┐
│Thermocouples│              │  Relay Control  │
│ (MAX31855)  │              │                 │
│ - TC2-TC5   │              │ - Valves 1-3    │
│             │              │ - Heaters 1-4   │
└─────────────┘              └─────────────────┘
```

### Component Responsibilities

**ald_gui.py** (GUI Layer)
- User interface and interaction
- Input validation via Pydantic
- Real-time data visualization
- Error handling and user feedback

**ald_controller.py** (Communication Layer)
- Async serial communication
- Command formatting
- Response parsing
- Connection management

**ald_models.py** (Data Layer)
- Pydantic validation models
- Type safety
- Range checking

**Arduino Firmware** (Hardware Layer)
- Thermocouple reading (MAX31855)
- Relay control (MOSFETs + traditional)
- Command parsing
- Safety interlocks

### Data Flow

**Command Flow** (PC → Arduino):
```
User Input → Pydantic Validation → Format Command → 
Serial Write → Arduino Parse → Execute
```

**Response Flow** (Arduino → PC):
```
Arduino Sensor → Serial Print → Async Read → 
Parse Data → Update GUI
```

---

## Tech Stack

### Software Stack

Main dependencies:

PyQt6 (6.6+) - GUI framework for the interface
PyQt6-Charts (6.6+) - Makes the temperature graphs
qasync (0.24+) - Lets Qt and asyncio work together
Pydantic (2.5+) - Validates inputs before sending to Arduino
pyserial-asyncio (0.6+) - Talks to Arduino without blocking
pytest (7.4+) - For running tests
PyYAML (6.0+) - Config files (planned feature)

### Hardware Stack

Arduino Uno (ATmega328P) - The main microcontroller
MAX31855 chips (4x) - Read K-type thermocouples for temperature
MOSFETs (Active HIGH) - Control pneumatic valves
Relays (Active LOW) - Switch heaters on and off

### System Requirements

**Minimum:**
- OS: Windows 10, macOS 10.14, or Linux
- Python: 3.9+
- RAM: 512MB
- Storage: 50MB
- USB: 1 port

**Recommended:**
- Python 3.11+
- RAM: 1GB
- Display: 1280x720 or higher

---

## Development

### Project Structure

```
controls
  ald_gui.py              # Main GUI application
  ald_controller.py       # Serial communication
  ald_models.py           # Data validation models
  test_controller.py      # Unit tests
  test_models.py          # Validation tests
  demo_controller.py      # Interactive demo
  requirements.txt        # Python dependencies
  README.md               # This file
  DEVELOPMENT_LOG.md      # Development history
  ARCHITECTURE.md         # Technical design docs
```

### Running Tests

```bash
# Run all tests
pytest -v

# Run specific test suite
pytest test_controller.py::TestBasic -v
pytest test_models.py -v

```

### My Git Workflow

To keep commits organized:
- Made a new branch for big features
- Committed whenever something worked
- Wrote clear commit messages ("Add temperature validation")
- Merged to main after testing

---

## Troubleshooting

### Can't Connect to Arduino

Try these in order:
1. Make sure Arduino is plugged in (power LED should be on)
2. Check the COM port:
   - Windows: Device Manager → Ports
   - Linux: `ls /dev/ttyUSB*`
   - Mac: `ls /dev/tty.usb*`
3. Close Arduino IDE (it might be using the port)
4. Try a different USB cable or port
5. Press the reset button on Arduino

### "Command Ignored" Error

This means the previous command is still running.

**Fix:** Wait for "Previous command has completed" or click the orange "Reset Valves" button

### Temperature Graph Not Updating

**Possible reasons:**
- Thermocouples not plugged in → Check wiring
- Wrong thermocouple type → Use K-type
- Arduino not sending data → Check Serial Monitor
- Parsing error → Look in `ald_controller.log`

### GUI Crashes

1. Restart the GUI
2. Check `ald_controller.log` for errors
3. Make sure you have Python 3.9+
4. Reinstall: `pip install --upgrade -r requirements.txt`

### "Validation Error"

Your input is outside the valid range:
- Valve ID: 1, 2, or 3
- Pulse time: 10-10000 ms
- Purge time: 0-15000 ms
- Temperature: 0-500°C (integers only)

### Need More Help?

1. Check the log file: `ald_controller.log`
2. Run tests: `pytest -v`
3. Open a GitHub issue with your error message and log file

---

## Future Improvements

### Done
- Async serial communication
- PyQt6 GUI with tabs
- Real-time temperature graphs
- Input validation
- Emergency stop
- Logging

### Working On Now
- Recipe management (save/load common settings)
- Export data to CSV
- Configuration files
- Run multiple depositions automatically

### Ideas for Later
- Historical data analysis
- Batch processing
- Web interface for remote monitoring
- Better safety features

---

## Contact

**Author:** Mohid Rattu
**Email:** hackerfab@cmu.edu  
**GitHub:** [github.com/MohidTheMan3/ald](https://github.com/MohidTheMan3/ald/tree/gui-revamp)

**Thanks to:**
- CMU Hacker Fab team
- 18-610 instructors
- Everyone who helped test

**Helpful Links:**
- [What is ALD?](https://en.wikipedia.org/wiki/Atomic_layer_deposition)
- [MAX31855 Datasheet](https://www.analog.com/en/products/max31855.html)
- [PyQt6 Docs](https://www.riverbankcomputing.com/static/Docs/PyQt6/)

---

**CMU Hacker Fab 2025**
