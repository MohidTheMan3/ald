import asyncio
import sys
from ald_controller import ALDController

async def demo():
    print("ALD Controller Demo")
    
    controller = ALDController()
    
    def show_response(msg):
        print(f"[ARDUINO] {msg}")
    
    controller.set_callback(show_response)
    
    port = input("Serial port (default COM3): ").strip()
    if not port:
        port = "COM3"
    
    try:
        print(f"\nConnecting to {port}...")
        await controller.connect(port)
        print("Connected!")
        
        while True:
            print("\nMenu:")
            print("1. Test command")
            print("2. Valve command")
            print("3. Temperature command")
            print("4. BEGIN command")
            print("5. Check connection")
            print("6. Exit")
            
            choice = input("Choice (1-6): ").strip()
            
            if choice == "1":
                print("Sending TEST...")
                await controller.test()
                
            elif choice == "2":
                try:
                    valve_id = int(input("Valve ID (1-3): "))
                    pulses = int(input("Pulses: "))
                    pulse_time = int(input("Pulse time (ms): "))
                    purge_time = int(input("Purge time (ms): "))
                    
                    print(f"Sending: v{valve_id};{pulses};{pulse_time};{purge_time}")
                    await controller.valve(valve_id, pulses, pulse_time, purge_time)
                    
                except ValueError:
                    print("Invalid input!")
                    
            elif choice == "3":
                try:
                    tc2 = float(input("TC2: "))
                    tc3 = float(input("TC3: "))
                    tc4 = float(input("TC4: "))
                    tc5 = float(input("TC5: "))
                    
                    print(f"Sending: t{tc2};{tc3};{tc4};{tc5}")
                    await controller.temp(tc2, tc3, tc4, tc5)
                    
                except ValueError:
                    print("Invalid input!")
                    
            elif choice == "4":
                print("Sending BEGIN...")
                await controller.begin()
                
            elif choice == "5":
                alive = await controller.is_alive()
                print("Connected" if alive else "Disconnected")
                
            elif choice == "6":
                break
                
            else:
                print("Invalid choice!")
            
            await asyncio.sleep(0.5)
    
    except Exception as e:
        print(f"Error: {e}")
        print("Check Arduino connection and COM port")
        
    except KeyboardInterrupt:
        print("\nInterrupted")
        
    finally:
        print("\nDisconnecting...")
        await controller.disconnect()
        print("Done!")

if __name__ == "__main__":
    try:
        asyncio.run(demo())
    except KeyboardInterrupt:
        print("\nInterrupted")
        sys.exit(0)