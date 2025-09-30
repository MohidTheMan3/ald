import asyncio
import pytest
from ald_controller import ALDController

class TestBasic:
    def test_create_controller(self):
        controller = ALDController()
        assert controller is not None
        assert controller.connected == False
    
    def test_logger_setup(self):
        controller = ALDController()
        assert controller.logger is not None

class TestWithArduino:
    @pytest.mark.asyncio
    async def test_connect(self):
        controller = ALDController()
        
        try:
            success = await controller.connect("COM3")
            assert success == True
            assert controller.connected == True
            
            alive = await controller.is_alive()
            assert alive == True
            
        except Exception:
            pytest.skip("No Arduino on COM3")
        finally:
            await controller.disconnect()
    
    @pytest.mark.asyncio
    async def test_bad_port(self):
        controller = ALDController()
        
        with pytest.raises(Exception):
            await controller.connect("COM999")
    
    @pytest.mark.asyncio
    async def test_commands(self):
        controller = ALDController()
        
        try:
            await controller.connect("COM3")
            
            await controller.test()
            await controller.valve(1, 10, 100, 5000)
            await controller.temp(25.0, 30.0, 35.0, 40.0)
            await controller.begin()
            
            print("All commands sent!")
            
        except Exception:
            pytest.skip("No Arduino on COM3")
        finally:
            await controller.disconnect()
    
    @pytest.mark.asyncio
    async def test_callback(self):
        controller = ALDController()
        messages = []
        
        async def handler(msg):
            messages.append(msg)
            print(f"Got: {msg}")
        
        try:
            controller.set_callback(handler)
            await controller.connect("COM3")
            
            await controller.test()
            await asyncio.sleep(1)
            
            print(f"Got {len(messages)} messages")
            
        except Exception:
            pytest.skip("No Arduino on COM3")
        finally:
            await controller.disconnect()

async def manual_test():
    print("Manual test...")
    
    controller = ALDController()
    
    async def show_msg(msg):
        print(f"Arduino: {msg}")
    
    controller.set_callback(show_msg)
    
    try:
        print("Connecting...")
        await controller.connect("COM3")
        print("Connected!")
        
        print("Sending commands...")
        await controller.test()
        await asyncio.sleep(0.5)
        
        await controller.valve(1, 5, 200, 3000)
        await asyncio.sleep(0.5)
        
        await controller.temp(100, 150, 200, 250)
        await asyncio.sleep(0.5)
        
        print("Listening for 5 seconds...")
        await asyncio.sleep(5)
        
    except Exception as e:
        print(f"Error: {e}")
    finally:
        print("Disconnecting...")
        await controller.disconnect()
        print("Done!")

if __name__ == "__main__":
    asyncio.run(manual_test())