import asyncio
import serial_asyncio
import logging
from ald_models import ValveCommand, TempCommand

class ALDController:
    def __init__(self):
        self.reader = None
        self.writer = None
        self.connected = False
        self.logger = self.setup_logger()
        self.message_callback = None
        self.read_task = None
        
    def setup_logger(self):
        logger = logging.getLogger('ALD_Controller')
        logger.setLevel(logging.INFO)
        
        formatter = logging.Formatter('%(asctime)s - %(levelname)s - %(message)s')
        
        # Console output
        console_handler = logging.StreamHandler()
        console_handler.setFormatter(formatter)
        logger.addHandler(console_handler)
        
        # File output
        file_handler = logging.FileHandler('ald_controller.log')
        file_handler.setFormatter(formatter)
        logger.addHandler(file_handler)
        
        return logger
    
    async def connect(self, port="COM3", baudrate=9600):
        try:
            self.logger.info(f"Connecting to {port}...")
            
            self.reader, self.writer = await serial_asyncio.open_serial_connection(
                url=port, baudrate=baudrate
            )
            
            self.connected = True
            self.logger.info(f"Connected to {port}")
            
            # Start reading in background
            self.read_task = asyncio.create_task(self.continuous_read())
            
            # Arduino needs time to boot up
            await asyncio.sleep(2)
            
            return True
            
        except Exception as e:
            self.logger.error(f"Connection failed: {e}")
            self.connected = False
            raise
    
    async def disconnect(self):
        if self.read_task:
            self.read_task.cancel()
            try:
                await self.read_task
            except asyncio.CancelledError:
                pass
        
        if self.writer:
            self.writer.close()
            await self.writer.wait_closed()
        
        self.connected = False
        self.logger.info("Disconnected")
    
    async def send(self, data):
        if not self.connected:
            raise RuntimeError("Not connected")
        
        try:
            if not data.endswith('\n'):
                data += '\n'
            
            self.writer.write(data.encode())
            await self.writer.drain()
            self.logger.info(f"Sent: {data.strip()}")
            
        except Exception as e:
            self.logger.error(f"Send failed: {e}")
            raise
    
    async def read_line(self):
        if not self.connected:
            raise RuntimeError("Not connected")
        
        try:
            data = await self.reader.readline()
            msg = data.decode().strip()
            
            if msg:
                self.logger.info(f"Received: {msg}")
            
            return msg
            
        except Exception as e:
            self.logger.error(f"Read failed: {e}")
            raise
    
    async def continuous_read(self):
        while self.connected:
            try:
                data = await self.read_line()
                if data and self.message_callback:
                    await self.message_callback(data)
                    
            except Exception as e:
                if self.connected:
                    self.logger.error(f"Read error: {e}")
                break
            
            await asyncio.sleep(0.01)
    
    def set_callback(self, callback):
        self.message_callback = callback
    
    async def is_alive(self):
        return self.connected and self.writer and not self.writer.is_closing()
    
    # Commands for Arduino
    async def test(self):
        await self.send("TEST")
        
    async def valve(self, valve_id, num_pulses, pulse_time, purge_time):
        cmd = f"v{valve_id};{num_pulses};{pulse_time};{purge_time}"
        await self.send(cmd)
    
    async def temp(self, tc2, tc3, tc4, tc5):
        cmd = f"t{tc2};{tc3};{tc4};{tc5}"
        await self.send(cmd)
    
    async def begin(self):
        await self.send("BEGIN")