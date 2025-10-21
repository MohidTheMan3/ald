from pydantic import BaseModel, field_validator
from typing import Literal

class ValveCommand(BaseModel):
    valve_id: Literal[1, 2, 3]
    num_pulses: int
    pulse_time_ms: int
    purge_time_ms: int
    
    @field_validator('num_pulses')
    @classmethod
    def check_pulses(cls, v):
        if v <= 0:
            raise ValueError('Pulses must be positive')
        if v > 1000:
            raise ValueError('Too many pulses (max 1000)')
        return v
    
    @field_validator('pulse_time_ms')
    @classmethod
    def check_pulse_time(cls, v):
        if v < 10:
            raise ValueError('Pulse time too short (min 10ms)')
        if v > 10000:
            raise ValueError('Pulse time too long (max 10s)')
        return v
    
    @field_validator('purge_time_ms')
    @classmethod
    def check_purge_time(cls, v):
        if v < 0:
            raise ValueError('Purge time cannot be negative')
        if v > 15000:
            raise ValueError('Purge time too long (max 15s)')
        return v

class TempCommand(BaseModel):
    tc2: float
    tc3: float
    tc4: float
    tc5: float
    
    @field_validator('tc2', 'tc3', 'tc4', 'tc5')
    @classmethod
    def check_temp(cls, v):
        if v < 0:
            raise ValueError('Temperature cannot be negative')
        if v > 500:
            raise ValueError('Temperature too high (max 500°C)')
        return v

class JobConfig(BaseModel):
    name: str
    valve_commands: list[ValveCommand]
    temp_command: TempCommand
    
    @field_validator('name')
    @classmethod
    def check_name(cls, v):
        if not v or len(v.strip()) == 0:
            raise ValueError('Job name cannot be empty')
        return v.strip()