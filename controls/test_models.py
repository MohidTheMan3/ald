import pytest
from pydantic import ValidationError
from ald_models import ValveCommand, TempCommand, JobConfig

class TestValveCommand:
    def test_valid_valve(self):
        cmd = ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=100, purge_time_ms=5000)
        assert cmd.valve_id == 1
        assert cmd.num_pulses == 10
    
    def test_invalid_valve_id(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=5, num_pulses=10, pulse_time_ms=100, purge_time_ms=5000)
    
    def test_negative_pulses(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=1, num_pulses=-5, pulse_time_ms=100, purge_time_ms=5000)
    
    def test_too_many_pulses(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=1, num_pulses=2000, pulse_time_ms=100, purge_time_ms=5000)
    
    def test_pulse_time_too_short(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=5, purge_time_ms=5000)
    
    def test_pulse_time_too_long(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=20000, purge_time_ms=5000)
    
    def test_negative_purge(self):
        with pytest.raises(ValidationError):
            ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=100, purge_time_ms=-100)

class TestTempCommand:
    def test_valid_temps(self):
        cmd = TempCommand(tc2=25.0, tc3=30.0, tc4=35.0, tc5=40.0)
        assert cmd.tc2 == 25.0
    
    def test_negative_temp(self):
        with pytest.raises(ValidationError):
            TempCommand(tc2=-10, tc3=30, tc4=35, tc5=40)
    
    def test_temp_too_high(self):
        with pytest.raises(ValidationError):
            TempCommand(tc2=600, tc3=30, tc4=35, tc5=40)
    
    def test_all_zeros(self):
        cmd = TempCommand(tc2=0, tc3=0, tc4=0, tc5=0)
        assert cmd.tc2 == 0

class TestJobConfig:
    def test_valid_job(self):
        valve1 = ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=100, purge_time_ms=5000)
        temp = TempCommand(tc2=100, tc3=150, tc4=200, tc5=250)
        job = JobConfig(name="Test Job", valve_commands=[valve1], temp_command=temp)
        assert job.name == "Test Job"
    
    def test_empty_name(self):
        valve1 = ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=100, purge_time_ms=5000)
        temp = TempCommand(tc2=100, tc3=150, tc4=200, tc5=250)
        with pytest.raises(ValidationError):
            JobConfig(name="", valve_commands=[valve1], temp_command=temp)
    
    def test_whitespace_name(self):
        valve1 = ValveCommand(valve_id=1, num_pulses=10, pulse_time_ms=100, purge_time_ms=5000)
        temp = TempCommand(tc2=100, tc3=150, tc4=200, tc5=250)
        with pytest.raises(ValidationError):
            JobConfig(name="   ", valve_commands=[valve1], temp_command=temp)