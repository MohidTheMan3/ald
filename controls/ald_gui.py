import sys
import asyncio
from collections import deque
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                            QHBoxLayout, QLabel, QLineEdit, QPushButton, 
                            QTextEdit, QTabWidget, QGroupBox, QMessageBox)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QColor, QPalette
from PyQt6.QtCharts import QChart, QChartView, QLineSeries, QValueAxis
from qasync import QEventLoop, asyncSlot
from pydantic import ValidationError

from ald_controller import ALDController
from ald_models import ValveCommand, TempCommand

class ALDMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.controller = ALDController()
        
        # Temperature data storage
        self.temp_data = {
            'tc2': deque(maxlen=100),
            'tc3': deque(maxlen=100),
            'tc4': deque(maxlen=100),
            'tc5': deque(maxlen=100),
            'time': deque(maxlen=100)
        }
        self.time_counter = 0
        
        self.setup_ui()
        
        # Auto-connect callback
        def handle_response(msg):
            self.log_text.append(f"[ARDUINO] {msg}")
            self.parse_temperature_data(msg)
            self.handle_arduino_status(msg)
        
        self.controller.set_callback(handle_response)
        
        # Timer to update connection status
        self.status_timer = QTimer()
        self.status_timer.timeout.connect(self.update_status)
        self.status_timer.start(1000)
        
        # Timer to update graph
        self.graph_timer = QTimer()
        self.graph_timer.timeout.connect(self.update_graph)
        self.graph_timer.start(500)  # Update every 500ms
    
    def parse_temperature_data(self, msg):
        """Parse temperature data from Arduino messages"""
        # Arduino sends: "T: 25.0; 30.0; 35.0;40.0"
        # Format: "T: tc2; tc3; tc4;tc5"
        if msg.upper().startswith('T:'):
            try:
                # Remove 'T:' prefix and clean up
                temp_str = msg[2:].strip()
                # Split by semicolon and handle spaces
                temps = [t.strip() for t in temp_str.split(';')]
                if len(temps) >= 4:
                    self.temp_data['tc2'].append(float(temps[0]))
                    self.temp_data['tc3'].append(float(temps[1]))
                    self.temp_data['tc4'].append(float(temps[2]))
                    self.temp_data['tc5'].append(float(temps[3]))
                    self.temp_data['time'].append(self.time_counter)
                    self.time_counter += 1
            except (ValueError, IndexError) as e:
                # Debug: print malformed messages
                # print(f"Failed to parse temp: {msg} - {e}")
                pass  # Ignore malformed messages
    
    def handle_arduino_status(self, msg):
        """Handle status messages from Arduino"""
        msg_lower = msg.lower()
        
        # Command acknowledgment
        if "previous command has completed" in msg_lower:
            # System ready for new commands
            pass
        
        # Command ignored (overlap detected)
        elif "command ignored" in msg_lower:
            QMessageBox.warning(
                self,
                "Command Ignored",
                "Arduino is still processing previous valve command.\n\n"
                "Wait for completion or use 'Reset Valves' button."
            )
        
        # Emergency stop confirmation
        elif "emergency stop" in msg_lower and "received" in msg_lower:
            self.status_label.setText("🚨 EMERGENCY STOP ACTIVE")
            self.status_label.setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;")
        
        # Reset confirmation  
        elif "reset command received" in msg_lower:
            pass  # Already handled in reset_system method
    
    def setup_ui(self):
        self.setWindowTitle("ALD Control System")
        self.setGeometry(100, 100, 1100, 800)
        
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        
        # Status bar at top
        self.status_label = QLabel("Not Connected")
        self.status_label.setStyleSheet("background-color: #e74c3c; color: white; padding: 10px; font-weight: bold;")
        main_layout.addWidget(self.status_label)
        
        # Connection controls
        conn_layout = QHBoxLayout()
        conn_layout.addWidget(QLabel("COM Port:"))
        self.port_input = QLineEdit("COM3")
        self.port_input.setMaximumWidth(100)
        conn_layout.addWidget(self.port_input)
        
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.connect_arduino)
        conn_layout.addWidget(self.connect_btn)
        
        self.disconnect_btn = QPushButton("Disconnect")
        self.disconnect_btn.clicked.connect(self.disconnect_arduino)
        self.disconnect_btn.setEnabled(False)
        conn_layout.addWidget(self.disconnect_btn)
        
        # Emergency stop button
        self.estop_btn = QPushButton("EMERGENCY STOP")
        self.estop_btn.clicked.connect(self.emergency_stop)
        self.estop_btn.setStyleSheet("background-color: #c0392b; color: white; font-weight: bold; font-size: 14px; padding: 10px;")
        self.estop_btn.setEnabled(False)
        conn_layout.addWidget(self.estop_btn)
        
        # Reset button (softer alternative to emergency stop)
        self.reset_btn = QPushButton("Reset Valves")
        self.reset_btn.clicked.connect(self.reset_system)
        self.reset_btn.setStyleSheet("background-color: #e67e22; color: white; font-weight: bold; padding: 5px;")
        self.reset_btn.setEnabled(False)
        conn_layout.addWidget(self.reset_btn)
        
        conn_layout.addStretch()
        main_layout.addLayout(conn_layout)
        
        # Tabs
        self.tabs = QTabWidget()
        main_layout.addWidget(self.tabs)
        
        self.setup_control_tab()
        self.setup_monitor_tab()
        self.setup_log_tab()
        
        # Log at bottom
        main_layout.addWidget(QLabel("Arduino Log:"))
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(150)
        main_layout.addWidget(self.log_text)
    
    def setup_control_tab(self):
        widget = QWidget()
        layout = QHBoxLayout(widget)
        
        # Valve controls
        valve_group = QGroupBox("Valve Command")
        valve_layout = QVBoxLayout()
        
        h = QHBoxLayout()
        h.addWidget(QLabel("Valve ID (1-3):"))
        self.valve_id_input = QLineEdit("1")
        self.valve_id_input.setMaximumWidth(100)
        h.addWidget(self.valve_id_input)
        valve_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("Number of Pulses:"))
        self.num_pulses_input = QLineEdit("10")
        self.num_pulses_input.setMaximumWidth(100)
        h.addWidget(self.num_pulses_input)
        valve_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("Pulse Time (ms):"))
        self.pulse_time_input = QLineEdit("100")
        self.pulse_time_input.setMaximumWidth(100)
        h.addWidget(self.pulse_time_input)
        valve_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("Purge Time (ms):"))
        self.purge_time_input = QLineEdit("4000")
        self.purge_time_input.setMaximumWidth(100)
        h.addWidget(self.purge_time_input)
        valve_layout.addLayout(h)
        
        send_valve_btn = QPushButton("Send Valve Command")
        send_valve_btn.clicked.connect(self.send_valve)
        valve_layout.addWidget(send_valve_btn)
        
        valve_layout.addStretch()
        valve_group.setLayout(valve_layout)
        layout.addWidget(valve_group)
        
        # Temperature controls
        temp_group = QGroupBox("Temperature Setpoints")
        temp_layout = QVBoxLayout()
        
        h = QHBoxLayout()
        h.addWidget(QLabel("TC2 (Delivery Line):"))
        self.tc2_input = QLineEdit("25")
        self.tc2_input.setMaximumWidth(100)
        h.addWidget(self.tc2_input)
        h.addWidget(QLabel("°C"))
        temp_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("TC3 (Precursor 1):"))
        self.tc3_input = QLineEdit("30")
        self.tc3_input.setMaximumWidth(100)
        h.addWidget(self.tc3_input)
        h.addWidget(QLabel("°C"))
        temp_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("TC4 (Precursor 2):"))
        self.tc4_input = QLineEdit("35")
        self.tc4_input.setMaximumWidth(100)
        h.addWidget(self.tc4_input)
        h.addWidget(QLabel("°C"))
        temp_layout.addLayout(h)
        
        h = QHBoxLayout()
        h.addWidget(QLabel("TC5 (Substrate Heater):"))
        self.tc5_input = QLineEdit("40")
        self.tc5_input.setMaximumWidth(100)
        h.addWidget(self.tc5_input)
        h.addWidget(QLabel("°C"))
        temp_layout.addLayout(h)
        
        send_temp_btn = QPushButton("Send Temperature Command")
        send_temp_btn.clicked.connect(self.send_temp)
        temp_layout.addWidget(send_temp_btn)
        
        temp_layout.addStretch()
        temp_group.setLayout(temp_layout)
        layout.addWidget(temp_group)
        
        self.tabs.addTab(widget, "Control")
    
    def setup_monitor_tab(self):
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        # Current readings
        readings_group = QGroupBox("Current Temperature Readings")
        readings_layout = QHBoxLayout()
        
        self.tc2_reading = QLabel("TC2: --°C")
        self.tc2_reading.setStyleSheet("font-size: 14px; padding: 5px;")
        readings_layout.addWidget(self.tc2_reading)
        
        self.tc3_reading = QLabel("TC3: --°C")
        self.tc3_reading.setStyleSheet("font-size: 14px; padding: 5px;")
        readings_layout.addWidget(self.tc3_reading)
        
        self.tc4_reading = QLabel("TC4: --°C")
        self.tc4_reading.setStyleSheet("font-size: 14px; padding: 5px;")
        readings_layout.addWidget(self.tc4_reading)
        
        self.tc5_reading = QLabel("TC5: --°C")
        self.tc5_reading.setStyleSheet("font-size: 14px; padding: 5px;")
        readings_layout.addWidget(self.tc5_reading)
        
        readings_group.setLayout(readings_layout)
        layout.addWidget(readings_group)
        
        # Temperature graph
        self.setup_temperature_chart()
        layout.addWidget(self.chart_view)
        
        self.tabs.addTab(widget, "Temperature Monitor")
    
    def setup_temperature_chart(self):
        """Setup the temperature monitoring chart"""
        self.chart = QChart()
        self.chart.setTitle("Temperature vs Time")
        self.chart.setAnimationOptions(QChart.AnimationOption.NoAnimation)
        
        # Create series for each thermocouple
        self.tc2_series = QLineSeries()
        self.tc2_series.setName("TC2 (Delivery)")
        
        self.tc3_series = QLineSeries()
        self.tc3_series.setName("TC3 (Precursor 1)")
        
        self.tc4_series = QLineSeries()
        self.tc4_series.setName("TC4 (Precursor 2)")
        
        self.tc5_series = QLineSeries()
        self.tc5_series.setName("TC5 (Substrate)")
        
        self.chart.addSeries(self.tc2_series)
        self.chart.addSeries(self.tc3_series)
        self.chart.addSeries(self.tc4_series)
        self.chart.addSeries(self.tc5_series)
        
        # Setup axes
        self.axis_x = QValueAxis()
        self.axis_x.setTitleText("Time (samples)")
        self.axis_x.setRange(0, 100)
        
        self.axis_y = QValueAxis()
        self.axis_y.setTitleText("Temperature (°C)")
        self.axis_y.setRange(0, 100)
        
        self.chart.addAxis(self.axis_x, Qt.AlignmentFlag.AlignBottom)
        self.chart.addAxis(self.axis_y, Qt.AlignmentFlag.AlignLeft)
        
        self.tc2_series.attachAxis(self.axis_x)
        self.tc2_series.attachAxis(self.axis_y)
        self.tc3_series.attachAxis(self.axis_x)
        self.tc3_series.attachAxis(self.axis_y)
        self.tc4_series.attachAxis(self.axis_x)
        self.tc4_series.attachAxis(self.axis_y)
        self.tc5_series.attachAxis(self.axis_x)
        self.tc5_series.attachAxis(self.axis_y)
        
        # Create chart view
        self.chart_view = QChartView(self.chart)
        from PyQt6.QtGui import QPainter
        self.chart_view.setRenderHint(QPainter.RenderHint.Antialiasing)
    
    def update_graph(self):
        """Update the temperature graph with latest data"""
        if len(self.temp_data['time']) == 0:
            return
        
        # Clear series
        self.tc2_series.clear()
        self.tc3_series.clear()
        self.tc4_series.clear()
        self.tc5_series.clear()
        
        # Add points
        for i, t in enumerate(self.temp_data['time']):
            self.tc2_series.append(t, self.temp_data['tc2'][i])
            self.tc3_series.append(t, self.temp_data['tc3'][i])
            self.tc4_series.append(t, self.temp_data['tc4'][i])
            self.tc5_series.append(t, self.temp_data['tc5'][i])
        
        # Update current readings
        if len(self.temp_data['tc2']) > 0:
            self.tc2_reading.setText(f"TC2: {self.temp_data['tc2'][-1]:.1f}°C")
            self.tc3_reading.setText(f"TC3: {self.temp_data['tc3'][-1]:.1f}°C")
            self.tc4_reading.setText(f"TC4: {self.temp_data['tc4'][-1]:.1f}°C")
            self.tc5_reading.setText(f"TC5: {self.temp_data['tc5'][-1]:.1f}°C")
        
        # Auto-scale Y axis
        all_temps = list(self.temp_data['tc2']) + list(self.temp_data['tc3']) + \
                   list(self.temp_data['tc4']) + list(self.temp_data['tc5'])
        if all_temps:
            min_temp = min(all_temps)
            max_temp = max(all_temps)
            range_temp = max_temp - min_temp
            self.axis_y.setRange(max(0, min_temp - range_temp * 0.1), 
                                max_temp + range_temp * 0.1)
        
        # Auto-scale X axis
        if len(self.temp_data['time']) > 0:
            min_time = min(self.temp_data['time'])
            max_time = max(self.temp_data['time'])
            self.axis_x.setRange(min_time, max_time + 1)
    
    def setup_log_tab(self):
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        # Test and BEGIN buttons
        h = QHBoxLayout()
        test_btn = QPushButton("Send TEST Command")
        test_btn.clicked.connect(self.send_test)
        h.addWidget(test_btn)
        
        begin_btn = QPushButton("Send BEGIN Command")
        begin_btn.clicked.connect(self.send_begin)
        begin_btn.setStyleSheet("background-color: #27ae60; color: white; font-weight: bold;")
        h.addWidget(begin_btn)
        
        layout.addLayout(h)
        
        # Full log
        layout.addWidget(QLabel("Full Communication Log:"))
        self.full_log = QTextEdit()
        self.full_log.setReadOnly(True)
        layout.addWidget(self.full_log)
        
        self.tabs.addTab(widget, "Commands & Log")
    
    @asyncSlot()
    async def connect_arduino(self):
        port = self.port_input.text()
        try:
            await self.controller.connect(port)
            self.status_label.setText(f"Connected to {port}")
            self.status_label.setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;")
            self.connect_btn.setEnabled(False)
            self.disconnect_btn.setEnabled(True)
            self.estop_btn.setEnabled(True)
            self.reset_btn.setEnabled(True)
            self.log_text.append(f"Connected to {port}")
        except Exception as e:
            QMessageBox.critical(self, "Connection Error", f"Failed to connect: {e}")
    
    @asyncSlot()
    async def disconnect_arduino(self):
        try:
            await self.controller.disconnect()
            self.status_label.setText("Not Connected")
            self.status_label.setStyleSheet("background-color: #e74c3c; color: white; padding: 10px; font-weight: bold;")
            self.connect_btn.setEnabled(True)
            self.disconnect_btn.setEnabled(False)
            self.estop_btn.setEnabled(False)
            self.reset_btn.setEnabled(False)
            self.log_text.append("Disconnected")
        except Exception as e:
            QMessageBox.critical(self, "Disconnect Error", f"Failed to disconnect: {e}")
    
    @asyncSlot()
    async def send_valve(self):
        try:
            valve_id = int(self.valve_id_input.text())
            num_pulses = int(self.num_pulses_input.text())
            pulse_time = int(self.pulse_time_input.text())
            purge_time = int(self.purge_time_input.text())
            
            await self.controller.valve(valve_id, num_pulses, pulse_time, purge_time)
            self.log_text.append(f"Sent: v{valve_id};{num_pulses};{pulse_time};{purge_time}")
            
        except ValidationError as e:
            QMessageBox.warning(self, "Validation Error", str(e))
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid numbers")
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
    
    @asyncSlot()
    async def send_temp(self):
        try:
            tc2 = float(self.tc2_input.text())
            tc3 = float(self.tc3_input.text())
            tc4 = float(self.tc4_input.text())
            tc5 = float(self.tc5_input.text())
            
            await self.controller.temp(tc2, tc3, tc4, tc5)
            self.log_text.append(f"Sent: t{tc2};{tc3};{tc4};{tc5}")
            
        except ValidationError as e:
            QMessageBox.warning(self, "Validation Error", str(e))
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid numbers")
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
    
    @asyncSlot()
    async def send_test(self):
        try:
            await self.controller.test()
            self.log_text.append("Sent: TEST")
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
    
    @asyncSlot()
    async def send_begin(self):
        try:
            await self.controller.begin()
            self.log_text.append("Sent: BEGIN")
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
    
    @asyncSlot()
    async def emergency_stop(self):
        """Send emergency stop command to Arduino - LOCKS SYSTEM"""
        reply = QMessageBox.critical(
            self, 
            "⚠️ EMERGENCY STOP ⚠️", 
            "WARNING: This will LOCK the Arduino in emergency stop state!\n\n"
            "- All valves will close\n"
            "- All heaters will turn off\n"
            "- Arduino will require RESTART to recover\n\n"
            "Only use this for true emergencies!\n\n"
            "For normal stop, use 'Reset Valves' button instead.\n\n"
            "Proceed with EMERGENCY STOP?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No
        )
        
        if reply == QMessageBox.StandardButton.Yes:
            try:
                await self.controller.estop()
                self.log_text.append("!!! EMERGENCY STOP TRIGGERED !!!")
                self.log_text.append("!!! ARDUINO LOCKED - RESTART REQUIRED !!!")
                self.status_label.setText("🚨 EMERGENCY STOP - Arduino Locked")
                self.status_label.setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;")
                
                # Disable all controls except disconnect
                self.estop_btn.setEnabled(False)
                self.reset_btn.setEnabled(False)
                
                QMessageBox.information(
                    self,
                    "Emergency Stop Activated",
                    "Emergency stop activated.\n\n"
                    "Arduino is now locked in safe state.\n"
                    "Disconnect, restart Arduino, then reconnect."
                )
                
            except Exception as e:
                QMessageBox.critical(self, "Error", f"Failed to send emergency stop: {e}")
    
    @asyncSlot()
    async def reset_system(self):
        """Send reset command - clears pulse counter without locking"""
        try:
            await self.controller.reset()
            self.log_text.append("Reset command sent - pulse counter cleared")
            QMessageBox.information(
                self,
                "Reset Complete",
                "Valve pulse counter has been reset.\n\n"
                "System is ready for new commands."
            )
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to send reset: {e}")
    
    def reset_status_after_estop(self):
        """Reset status label after emergency stop"""
        if self.controller.connected:
            self.status_label.setText(f"Connected to {self.port_input.text()}")
            self.status_label.setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;")
    
    @asyncSlot()
    async def update_status(self):
        if await self.controller.is_alive():
            if "Not Connected" in self.status_label.text():
                self.status_label.setText(f"Connected to {self.port_input.text()}")
                self.status_label.setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;")

def main():
    app = QApplication(sys.argv)
    
    # Setup async event loop
    loop = QEventLoop(app)
    asyncio.set_event_loop(loop)
    
    window = ALDMainWindow()
    window.show()
    
    with loop:
        loop.run_forever()

if __name__ == "__main__":
    main()