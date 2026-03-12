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
        self._lock = asyncio.Lock()  # Lock to prevent concurrent serial operations
        self._message_queue = asyncio.Queue()  # Queue for incoming messages
        self._process_task = None
        
    def setup_logger(self):
        logger = logging.getLogger('ALD_Controller')
        logger.setLevel(logging.INFO)
        
        # Prevent duplicate handlers
        if logger.handlers:
            return logger
        
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
    
    async def connect(self, port="COM3", baudrate=115200):
        try:
            self.logger.info(f"Connecting to {port}...")
            
            self.reader, self.writer = await serial_asyncio.open_serial_connection(
                url=port, baudrate=baudrate
            )
            
            self.connected = True
            self.logger.info(f"Connected to {port}")
            
            # Start reading in background
            self.read_task = asyncio.create_task(self.continuous_read())
            
            # Start message processing task
            self._process_task = asyncio.create_task(self._process_messages())
            
            # Arduino needs time to boot up
            await asyncio.sleep(2)
            
            return True
            
        except Exception as e:
            self.logger.error(f"Connection failed: {e}")
            self.connected = False
            raise
    
    async def disconnect(self):
        self.connected = False
        
        if self.read_task:
            self.read_task.cancel()
            try:
                await self.read_task
            except asyncio.CancelledError:
                pass
        
        if self._process_task:
            self._process_task.cancel()
            try:
                await self._process_task
            except asyncio.CancelledError:
                pass
        
        if self.writer:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass
        
        self.reader = None
        self.writer = None
        self.logger.info("Disconnected")
    
    async def send(self, data):
        if not self.connected:
            raise RuntimeError("Not connected")
        
        async with self._lock:
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
            return None
        
        try:
            data = await asyncio.wait_for(self.reader.readline(), timeout=0.1)
            msg = data.decode().strip()
            
            if msg:
                self.logger.info(f"Received: {msg}")
            
            return msg
            
        except asyncio.TimeoutError:
            return None
        except Exception as e:
            if self.connected:
                self.logger.error(f"Read failed: {e}")
            raise
    
    async def continuous_read(self):
        """Read serial data continuously and queue messages for processing"""
        while self.connected:
            try:
                # Don't hold lock while reading - just read and queue
                if self.reader:
                    try:
                        data = await asyncio.wait_for(self.reader.readline(), timeout=0.1)
                        msg = data.decode().strip()
                        
                        if msg:
                            self.logger.info(f"Received: {msg}")
                            # Put message in queue instead of calling callback directly
                            await self._message_queue.put(msg)
                            
                    except asyncio.TimeoutError:
                        pass  # No data available, that's fine
                        
            except asyncio.CancelledError:
                break
            except Exception as e:
                if self.connected:
                    self.logger.error(f"Read error: {e}")
                break
            
            await asyncio.sleep(0.01)
    
    async def _process_messages(self):
        """Process queued messages by calling the callback"""
        while self.connected:
            try:
                # Wait for a message with timeout
                try:
                    msg = await asyncio.wait_for(self._message_queue.get(), timeout=0.1)
                    if msg and self.message_callback:
                        # Call callback synchronously (it should not be async)
                        self.message_callback(msg)
                except asyncio.TimeoutError:
                    pass
                    
            except asyncio.CancelledError:
                break
            except Exception as e:
                self.logger.error(f"Message processing error: {e}")
            
            await asyncio.sleep(0.001)
    
    def set_callback(self, callback):
        self.message_callback = callback
    
    def is_connected(self):
        """Synchronous connection check"""
        return self.connected and self.writer is not None
    
    # Commands for Arduino
    async def test(self):
        await self.send("TEST")
        
    async def valve(self, valve_id, num_pulses, pulse_time, purge_time):
        # Validate with Pydantic
        cmd_obj = ValveCommand(
            valve_id=valve_id,
            num_pulses=num_pulses,
            pulse_time_ms=pulse_time,
            purge_time_ms=purge_time
        )
        cmd = f"v{cmd_obj.valve_id};{cmd_obj.num_pulses};{cmd_obj.pulse_time_ms};{cmd_obj.purge_time_ms}"
        await self.send(cmd)
    
    async def temp(self, tc2, tc3, tc4, tc5):
        # Validate with Pydantic
        cmd_obj = TempCommand(tc2=tc2, tc3=tc3, tc4=tc4, tc5=tc5)
        cmd = f"t{int(cmd_obj.tc2)};{int(cmd_obj.tc3)};{int(cmd_obj.tc4)};{int(cmd_obj.tc5)}"
        await self.send(cmd)
    
    async def begin(self):
        await self.send("BEGIN")
    
    async def estop(self):
        """Emergency stop - locks system, requires restart"""
        await self.send("s")
        self.logger.critical("EMERGENCY STOP - System locked, Arduino needs restart")
    
    async def reset(self):
        """Reset pulse counter without emergency stop"""
        await self.send("r")
        self.logger.info("Reset command sent - pulse counter cleared")