import asyncio
from ald_controller import ALDController

async def test_serial():
    print("=== Serial Communication Debug ===\n")
    
    controller = ALDController()
    
    messages_received = []
    
    # Set callback to see what we receive
    def show_response(msg):
        print(f"[RECEIVED] {msg}")
        messages_received.append(msg)
    
    controller.set_callback(show_response)
    
    try:
        # Connect
        print("Connecting to COM3...")
        await controller.connect("COM3", 9600)
        print("Connected!\n")
        
        # Wait a bit for Arduino
        print("Waiting 3 seconds for Arduino to send temperature data...")
        await asyncio.sleep(3)
        
        if len(messages_received) > 0:
            print(f"\n✓ Arduino is sending data! Received {len(messages_received)} messages")
        else:
            print("\n✗ NO DATA from Arduino - check connection or Arduino code")
            
        messages_received.clear()
        
        # Send test valve command
        print("\nSending valve command: v1;5;100;2000")
        await controller.valve(1, 5, 100, 2000)
        print("Command sent! Check if Arduino responds with 'Got command!'")
        
        # Wait to see responses
        print("Waiting 5 seconds for Arduino responses...")
        await asyncio.sleep(5)
        
        if "Got command!" in str(messages_received):
            print("\n✓ Arduino RECEIVED the command!")
        else:
            print("\n✗ Arduino did NOT respond to command")
            print(f"   Messages received: {messages_received}")
        
        # Send temp command
        print("\nSending temp command: t25;30;35;40")
        await controller.temp(25, 30, 35, 40)
        print("Command sent!\n")
        
        # Wait more
        print("Waiting 3 more seconds...")
        await asyncio.sleep(3)
        
        print(f"\nTotal messages received: {len(messages_received)}")
        
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
    
    finally:
        print("\nDisconnecting...")
        await controller.disconnect()
        print("Done!")
        print("\nCheck ald_controller.log for detailed logs")

if __name__ == "__main__":
    asyncio.run(test_serial())