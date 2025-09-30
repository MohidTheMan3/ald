ALD CONTROLLER

Simple Python controller for ALD equipment.

SETUP:
  pip install pyserial-asyncio pytest pytest-asyncio

FILES:
  ald_controller.py - main controller
  demo_controller.py - interactive demo  
  test_controller.py - tests

TEST WITHOUT ARDUINO:
  python -c "from ald_controller import ALDController; print('Worked')"
  pytest test_controller.py::TestBasic -v

RUN DEMO:
  python demo_controller.py

Enter your COM port and use menu to send commands.

COMMANDS SENT TO ARDUINO:
  TEST - test communication
  v1;10;100;5000 - valve command (valve_id;pulses;pulse_time;purge_time)
  t25;30;35;40 - temperature (tc2;tc3;tc4;tc5)
  BEGIN - start sequence

LOGS:
  All communication saved to ald_controller.log