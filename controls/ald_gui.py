import sys
import asyncio
import csv
from datetime import datetime, timedelta
from collections import deque
import winsound
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                            QHBoxLayout, QLabel, QLineEdit, QPushButton, 
                            QTextEdit, QTabWidget, QGroupBox, QMessageBox, QFileDialog,
                            QInputDialog, QProgressBar, QGridLayout)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QColor, QPalette, QPen
from PyQt6.QtCharts import QChart, QChartView, QLineSeries, QValueAxis, QDateTimeAxis
from qasync import QEventLoop, asyncSlot
from pydantic import ValidationError

from ald_controller import ALDController
from ald_models import ValveCommand, TempCommand
from ald_recipe import Recipe

class ALDMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.controller = ALDController()
        
        # Temperature setpoints for safety monitoring
        self.temp_setpoints = {'tc2': 25, 'tc3': 30, 'tc4': 35, 'tc5': 40}
        
        # Temperature data storage
        self.temp_data = {
            'tc2': deque(maxlen=10000),
            'tc3': deque(maxlen=10000),
            'tc4': deque(maxlen=10000),
            'tc5': deque(maxlen=10000),
            'time': deque(maxlen=10000),
            'timestamp': deque(maxlen=10000)
        }
        # Pressure and flow data storage
        self.pressure_data = {
            'value': deque(maxlen=10000),
            'unit': deque(maxlen=10000),
            'time': deque(maxlen=10000),
            'timestamp': deque(maxlen=10000)
        }
        self.flow_data = {
            'value': deque(maxlen=10000),
            'time': deque(maxlen=10000),
            'timestamp': deque(maxlen=10000)
        }
        self.time_counter = 0
        self.start_time = None
        self.valve_job_running = False
        
        # Job timing tracking
        self.job_start_time = None
        self.job_total_duration = 0
        self.job_timer = QTimer()
        self.job_timer.timeout.connect(self.update_job_progress)
        
        # Flow alarm state tracking
        self.flow_alarm_threshold = 2.0  # m/s
        self.flow_alarm_active = False
        self.flow_alarm_dialog = None
        
        # Command history for optional CSV export
        self.command_history = []
        
        # Recipe management
        self.current_recipe = None
        self.recipe_running = False
        self.recipe_step_index = 0
        self.recipe_total_steps = 0
        
        # Lock to prevent concurrent async operations
        self.operation_in_progress = False
        
        self.setup_ui()
        
        # Auto-connect callback
        def handle_response(msg):
            self.log_text.append(f"[ARDUINO] {msg}")
            self.parse_temperature_data(msg)
            self.parse_pressure_data(msg)
            self.parse_flow_data(msg)
            self.handle_arduino_status(msg)
        
        self.controller.set_callback(handle_response)
        
        # Timer to update connection status
        self.status_timer = QTimer()
        self.status_timer.timeout.connect(self.update_status)
        self.status_timer.start(1000)
        
        # Timer to update graph - reduced frequency to prevent lag
        self.graph_timer = QTimer()
        self.graph_timer.timeout.connect(self.update_graph)
        self.graph_timer.start(1000)
        
        # Batch update counter to reduce graph redraws
        self.graph_update_pending = False
    
    def parse_temperature_data(self, msg):
        """Parse temperature data from Arduino messages"""
        if msg.upper().startswith('T:'):
            try:
                temp_str = msg[2:].strip()
                temps = [t.strip() for t in temp_str.split(';')]
                if len(temps) >= 4:
                    now = datetime.now()
                    if self.start_time is None:
                        self.start_time = now
                    
                    tc2_val = float(temps[0])
                    tc3_val = float(temps[1])
                    tc4_val = float(temps[2])
                    tc5_val = float(temps[3])
                    
                    self.temp_data['tc2'].append(tc2_val)
                    self.temp_data['tc3'].append(tc3_val)
                    self.temp_data['tc4'].append(tc4_val)
                    self.temp_data['tc5'].append(tc5_val)
                    self.temp_data['time'].append(self.time_counter)
                    self.temp_data['timestamp'].append(now)
                    self.time_counter += 1
                    self.graph_update_pending = True
                    
                    # Safety check - temperature overheat protection
                    self.check_temperature_safety(tc2_val, tc3_val, tc4_val, tc5_val)
                    
            except (ValueError, IndexError) as e:
                pass
    
    def check_temperature_safety(self, tc2, tc3, tc4, tc5):
        """Check if any temperature exceeds setpoint by more than 50°C"""
        overheat_elements = []
        
        if tc2 > self.temp_setpoints['tc2'] + 50:
            overheat_elements.append(f"TC2 (Delivery Line): {tc2:.1f}°C (setpoint: {self.temp_setpoints['tc2']}°C)")
        if tc3 > self.temp_setpoints['tc3'] + 50:
            overheat_elements.append(f"TC3 (Precursor 1): {tc3:.1f}°C (setpoint: {self.temp_setpoints['tc3']}°C)")
        if tc4 > self.temp_setpoints['tc4'] + 50:
            overheat_elements.append(f"TC4 (Precursor 2): {tc4:.1f}°C (setpoint: {self.temp_setpoints['tc4']}°C)")
        if tc5 > self.temp_setpoints['tc5'] + 50:
            overheat_elements.append(f"TC5 (Substrate Heater): {tc5:.1f}°C (setpoint: {self.temp_setpoints['tc5']}°C)")
        
        if overheat_elements:
            self.trigger_overheat_emergency_stop(overheat_elements)
    
    @asyncSlot()
    async def trigger_overheat_emergency_stop(self, overheat_elements):
        """Trigger emergency stop due to overheating"""
        # Don't check operation_in_progress for safety-critical function
        self.operation_in_progress = True
        try:
            await self.controller.estop()
            self.valve_job_running = False
            self.job_timer.stop()
            
            self.log_text.append("!!! EMERGENCY STOP - OVERHEAT DETECTED !!!")
            self.status_label.setText("🚨 OVERHEAT - EMERGENCY STOP")
            self.status_label.setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;")
            
            overheat_msg = "\n".join(overheat_elements)
            QMessageBox.critical(
                self,
                "⚠️ OVERHEAT EMERGENCY STOP ⚠️",
                f"Emergency stop triggered due to overheating!\n\n"
                f"The following elements exceeded setpoint by >50°C:\n\n"
                f"{overheat_msg}\n\n"
                f"Arduino is now locked. Disconnect and restart required."
            )
            
            self.estop_btn.setEnabled(False)
            self.reset_btn.setEnabled(False)
            
        except Exception as e:
            self.log_text.append(f"[ERROR] Failed to send emergency stop: {e}")
            QMessageBox.critical(self, "Error", f"Failed to send emergency stop: {e}")
        finally:
            self.operation_in_progress = False

    def parse_pressure_data(self, msg):
        """Parse pressure data from Arduino messages"""
        if msg.upper().startswith('P:'):
            try:
                parts = msg[2:].strip().split()
                if len(parts) >= 2:
                    value = float(parts[0])
                    unit = parts[1]
                    self.pressure_data['value'].append(value)
                    self.pressure_data['unit'].append(unit)
                    self.pressure_data['time'].append(self.time_counter)
                    self.pressure_data['timestamp'].append(datetime.now())
            except (ValueError, IndexError):
                pass

    def parse_flow_data(self, msg):
        """Parse flow data from Arduino messages"""
        if msg.upper().startswith('F:'):
            try:
                parts = msg[2:].strip().split()
                if len(parts) >= 1:
                    value = float(parts[0])
                    self.flow_data['value'].append(value)
                    self.flow_data['time'].append(self.time_counter)
                    self.flow_data['timestamp'].append(datetime.now())
            except (ValueError, IndexError):
                pass

    def handle_arduino_status(self, msg):
        """Handle status messages from Arduino"""
        msg_lower = msg.lower()
        
        if "previous command has completed" in msg_lower or "ready for new command" in msg_lower:
            self.valve_job_running = False
            self.job_timer.stop()
            self.progress_bar.setValue(100)
            self.time_remaining_label.setText("Complete")
            self.log_text.append(f"[DEBUG] Set valve_job_running = False (job complete)")
        
        elif "command ignored" in msg_lower:
            self.log_text.append(f"[DEBUG] Arduino ignored command")
            QMessageBox.warning(
                self,
                "Command Ignored",
                "Arduino is still processing previous valve command.\n\n"
                "Wait for completion or use 'Reset Valves' button."
            )
        
        elif "emergency stop" in msg_lower and "received" in msg_lower:
            self.valve_job_running = False
            self.job_timer.stop()
            self.log_text.append(f"[DEBUG] Set valve_job_running = False (emergency stop)")
            self.status_label.setText("🚨 EMERGENCY STOP ACTIVE")
            self.status_label.setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;")
        
        elif "reset command received" in msg_lower:
            self.valve_job_running = False
            self.job_timer.stop()
            self.progress_bar.setValue(0)
            self.time_remaining_label.setText("--")
            self.log_text.append(f"[DEBUG] Set valve_job_running = False (reset)")
    
    def setup_ui(self):
        self.setWindowTitle("ALD Control System")
        self.setGeometry(100, 100, 1200, 900)
        
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
        
        self.estop_btn = QPushButton("EMERGENCY STOP")
        self.estop_btn.clicked.connect(self.emergency_stop)
        self.estop_btn.setStyleSheet("background-color: #c0392b; color: white; font-weight: bold; font-size: 14px; padding: 10px;")
        self.estop_btn.setEnabled(False)
        conn_layout.addWidget(self.estop_btn)
        
        self.reset_btn = QPushButton("Reset Valves")
        self.reset_btn.clicked.connect(self.reset_system)
        self.reset_btn.setStyleSheet("background-color: #e67e22; color: white; font-weight: bold; padding: 5px;")
        self.reset_btn.setEnabled(False)
        conn_layout.addWidget(self.reset_btn)
        
        conn_layout.addStretch()
        main_layout.addLayout(conn_layout)
        
        # Progress bar for job timing
        progress_layout = QHBoxLayout()
        progress_layout.addWidget(QLabel("Job Progress:"))
        self.progress_bar = QProgressBar()
        self.progress_bar.setMinimum(0)
        self.progress_bar.setMaximum(100)
        self.progress_bar.setValue(0)
        self.progress_bar.setTextVisible(True)
        progress_layout.addWidget(self.progress_bar)
        
        self.time_remaining_label = QLabel("--")
        self.time_remaining_label.setMinimumWidth(120)
        self.time_remaining_label.setStyleSheet("font-weight: bold; padding: 5px;")
        progress_layout.addWidget(self.time_remaining_label)
        main_layout.addLayout(progress_layout)
        
        # Tabs
        self.tabs = QTabWidget()
        main_layout.addWidget(self.tabs)
        
        self.setup_monitor_tab()  # Main control + monitoring tab
        self.setup_recipe_tab()
        self.setup_log_tab()
        
        # Log at bottom
        main_layout.addWidget(QLabel("Arduino Log:"))
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(120)
        main_layout.addWidget(self.log_text)
    
    def setup_monitor_tab(self):
        """Setup the combined process monitor tab with all controls and monitoring"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        # Top section: Controls side by side
        controls_layout = QHBoxLayout()
        
        # Valve Controls
        valve_group = QGroupBox("Valve Control")
        valve_layout = QGridLayout()
        
        valve_layout.addWidget(QLabel("Valve ID (1-3):"), 0, 0)
        self.valve_id_input = QLineEdit("1")
        self.valve_id_input.setMaximumWidth(80)
        valve_layout.addWidget(self.valve_id_input, 0, 1)
        
        valve_layout.addWidget(QLabel("Pulses:"), 1, 0)
        self.num_pulses_input = QLineEdit("10")
        self.num_pulses_input.setMaximumWidth(80)
        valve_layout.addWidget(self.num_pulses_input, 1, 1)
        
        valve_layout.addWidget(QLabel("Pulse (ms):"), 2, 0)
        self.pulse_time_input = QLineEdit("100")
        self.pulse_time_input.setMaximumWidth(80)
        valve_layout.addWidget(self.pulse_time_input, 2, 1)
        
        valve_layout.addWidget(QLabel("Purge (ms):"), 3, 0)
        self.purge_time_input = QLineEdit("4000")
        self.purge_time_input.setMaximumWidth(80)
        valve_layout.addWidget(self.purge_time_input, 3, 1)
        
        send_valve_btn = QPushButton("Send Valve Command")
        send_valve_btn.clicked.connect(self.send_valve)
        send_valve_btn.setStyleSheet("background-color: #9b59b6; color: white; font-weight: bold;")
        valve_layout.addWidget(send_valve_btn, 4, 0, 1, 2)
        
        valve_group.setLayout(valve_layout)
        controls_layout.addWidget(valve_group)
        
        # Temperature Setpoints
        temp_group = QGroupBox("Temperature Setpoints")
        temp_layout = QGridLayout()
        
        temp_layout.addWidget(QLabel("TC2 (Delivery):"), 0, 0)
        self.tc2_input = QLineEdit("25")
        self.tc2_input.setMaximumWidth(80)
        temp_layout.addWidget(self.tc2_input, 0, 1)
        temp_layout.addWidget(QLabel("°C"), 0, 2)
        
        temp_layout.addWidget(QLabel("TC3 (Precursor 1):"), 1, 0)
        self.tc3_input = QLineEdit("30")
        self.tc3_input.setMaximumWidth(80)
        temp_layout.addWidget(self.tc3_input, 1, 1)
        temp_layout.addWidget(QLabel("°C"), 1, 2)
        
        temp_layout.addWidget(QLabel("TC4 (Precursor 2):"), 2, 0)
        self.tc4_input = QLineEdit("35")
        self.tc4_input.setMaximumWidth(80)
        temp_layout.addWidget(self.tc4_input, 2, 1)
        temp_layout.addWidget(QLabel("°C"), 2, 2)
        
        temp_layout.addWidget(QLabel("TC5 (Substrate):"), 3, 0)
        self.tc5_input = QLineEdit("40")
        self.tc5_input.setMaximumWidth(80)
        temp_layout.addWidget(self.tc5_input, 3, 1)
        temp_layout.addWidget(QLabel("°C"), 3, 2)
        
        send_temp_btn = QPushButton("Set Temperatures")
        send_temp_btn.clicked.connect(self.send_temp)
        send_temp_btn.setStyleSheet("background-color: #3498db; color: white; font-weight: bold;")
        temp_layout.addWidget(send_temp_btn, 4, 0, 1, 3)
        
        temp_group.setLayout(temp_layout)
        controls_layout.addWidget(temp_group)
        
        # Pressure & Flow readings
        sensors_group = QGroupBox("Pressure & Flow")
        sensors_layout = QVBoxLayout()
        
        self.pressure_reading = QLabel("Pressure: -- mTorr")
        self.pressure_reading.setStyleSheet("font-size: 16px; font-weight: bold; padding: 10px;")
        sensors_layout.addWidget(self.pressure_reading)
        
        self.flow_reading = QLabel("Flow: -- m/s")
        self.flow_reading.setStyleSheet("font-size: 16px; font-weight: bold; padding: 10px;")
        sensors_layout.addWidget(self.flow_reading)
        
        sensors_layout.addStretch()
        sensors_group.setLayout(sensors_layout)
        controls_layout.addWidget(sensors_group)
        
        layout.addLayout(controls_layout)
        
        # Temperature readings with setpoint comparison - compact horizontal layout
        readings_group = QGroupBox("Temperature Readings (Current → Setpoint → Delta)")
        readings_layout = QHBoxLayout()
        
        # TC2
        tc2_box = QVBoxLayout()
        tc2_label = QLabel("TC2 Delivery")
        tc2_label.setStyleSheet("font-size: 10px; color: #7f8c8d;")
        tc2_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc2_box.addWidget(tc2_label)
        self.tc2_reading = QLabel("--°C")
        self.tc2_reading.setStyleSheet("font-size: 16px; font-weight: bold; color: #2980b9;")
        self.tc2_reading.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc2_box.addWidget(self.tc2_reading)
        tc2_sp_row = QHBoxLayout()
        self.tc2_setpoint_display = QLabel("→ 25°C")
        self.tc2_setpoint_display.setStyleSheet("font-size: 11px; color: #7f8c8d;")
        tc2_sp_row.addWidget(self.tc2_setpoint_display)
        self.tc2_delta = QLabel("--")
        self.tc2_delta.setStyleSheet("font-size: 11px;")
        tc2_sp_row.addWidget(self.tc2_delta)
        tc2_box.addLayout(tc2_sp_row)
        readings_layout.addLayout(tc2_box)
        
        readings_layout.addWidget(QLabel("|"))
        
        # TC3
        tc3_box = QVBoxLayout()
        tc3_label = QLabel("TC3 Precursor 1")
        tc3_label.setStyleSheet("font-size: 10px; color: #7f8c8d;")
        tc3_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc3_box.addWidget(tc3_label)
        self.tc3_reading = QLabel("--°C")
        self.tc3_reading.setStyleSheet("font-size: 16px; font-weight: bold; color: #27ae60;")
        self.tc3_reading.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc3_box.addWidget(self.tc3_reading)
        tc3_sp_row = QHBoxLayout()
        self.tc3_setpoint_display = QLabel("→ 30°C")
        self.tc3_setpoint_display.setStyleSheet("font-size: 11px; color: #7f8c8d;")
        tc3_sp_row.addWidget(self.tc3_setpoint_display)
        self.tc3_delta = QLabel("--")
        self.tc3_delta.setStyleSheet("font-size: 11px;")
        tc3_sp_row.addWidget(self.tc3_delta)
        tc3_box.addLayout(tc3_sp_row)
        readings_layout.addLayout(tc3_box)
        
        readings_layout.addWidget(QLabel("|"))
        
        # TC4
        tc4_box = QVBoxLayout()
        tc4_label = QLabel("TC4 Precursor 2")
        tc4_label.setStyleSheet("font-size: 10px; color: #7f8c8d;")
        tc4_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc4_box.addWidget(tc4_label)
        self.tc4_reading = QLabel("--°C")
        self.tc4_reading.setStyleSheet("font-size: 16px; font-weight: bold; color: #e67e22;")
        self.tc4_reading.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc4_box.addWidget(self.tc4_reading)
        tc4_sp_row = QHBoxLayout()
        self.tc4_setpoint_display = QLabel("→ 35°C")
        self.tc4_setpoint_display.setStyleSheet("font-size: 11px; color: #7f8c8d;")
        tc4_sp_row.addWidget(self.tc4_setpoint_display)
        self.tc4_delta = QLabel("--")
        self.tc4_delta.setStyleSheet("font-size: 11px;")
        tc4_sp_row.addWidget(self.tc4_delta)
        tc4_box.addLayout(tc4_sp_row)
        readings_layout.addLayout(tc4_box)
        
        readings_layout.addWidget(QLabel("|"))
        
        # TC5
        tc5_box = QVBoxLayout()
        tc5_label = QLabel("TC5 Substrate")
        tc5_label.setStyleSheet("font-size: 10px; color: #7f8c8d;")
        tc5_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc5_box.addWidget(tc5_label)
        self.tc5_reading = QLabel("--°C")
        self.tc5_reading.setStyleSheet("font-size: 16px; font-weight: bold; color: #c0392b;")
        self.tc5_reading.setAlignment(Qt.AlignmentFlag.AlignCenter)
        tc5_box.addWidget(self.tc5_reading)
        tc5_sp_row = QHBoxLayout()
        self.tc5_setpoint_display = QLabel("→ 40°C")
        self.tc5_setpoint_display.setStyleSheet("font-size: 11px; color: #7f8c8d;")
        tc5_sp_row.addWidget(self.tc5_setpoint_display)
        self.tc5_delta = QLabel("--")
        self.tc5_delta.setStyleSheet("font-size: 11px;")
        tc5_sp_row.addWidget(self.tc5_delta)
        tc5_box.addLayout(tc5_sp_row)
        readings_layout.addLayout(tc5_box)
        
        # Export button inline
        readings_layout.addStretch()
        export_btn = QPushButton("Export CSV")
        export_btn.clicked.connect(self.export_temperature_data)
        export_btn.setMaximumWidth(100)
        readings_layout.addWidget(export_btn)
        
        readings_group.setLayout(readings_layout)
        readings_group.setMaximumHeight(100)
        layout.addWidget(readings_group)
        
        # Temperature graph with time axis - give it more space
        self.setup_temperature_chart()
        self.chart_view.setMinimumHeight(350)
        layout.addWidget(self.chart_view, stretch=1)
        
        self.tabs.addTab(widget, "Process Monitor")
    
    def setup_recipe_tab(self):
        widget = QWidget()
        layout = QVBoxLayout(widget)
        
        # Recipe builder section
        builder_group = QGroupBox("Recipe Builder")
        builder_layout = QVBoxLayout()
        
        # Recipe name
        name_layout = QHBoxLayout()
        name_layout.addWidget(QLabel("Recipe Name:"))
        self.recipe_name_input = QLineEdit()
        self.recipe_name_input.setPlaceholderText("e.g., Al2O3_50cycles")
        name_layout.addWidget(self.recipe_name_input)
        builder_layout.addLayout(name_layout)
        
        # Recipe description
        desc_layout = QHBoxLayout()
        desc_layout.addWidget(QLabel("Description:"))
        self.recipe_desc_input = QLineEdit()
        self.recipe_desc_input.setPlaceholderText("Optional description")
        desc_layout.addWidget(self.recipe_desc_input)
        builder_layout.addLayout(desc_layout)
        
        # Buttons for adding steps
        step_buttons = QHBoxLayout()
        
        add_valve_btn = QPushButton("Add Current Valve Settings")
        add_valve_btn.clicked.connect(self.add_valve_to_recipe)
        step_buttons.addWidget(add_valve_btn)
        
        add_temp_btn = QPushButton("Add Current Temp Settings")
        add_temp_btn.clicked.connect(self.add_temp_to_recipe)
        step_buttons.addWidget(add_temp_btn)
        
        add_wait_btn = QPushButton("Add Wait Step")
        add_wait_btn.clicked.connect(self.add_wait_to_recipe)
        step_buttons.addWidget(add_wait_btn)
        
        builder_layout.addLayout(step_buttons)
        
        # Recipe preview
        builder_layout.addWidget(QLabel("Recipe Steps:"))
        self.recipe_preview = QTextEdit()
        self.recipe_preview.setReadOnly(True)
        self.recipe_preview.setMaximumHeight(150)
        builder_layout.addWidget(self.recipe_preview)
        
        # Save/Clear buttons
        save_buttons = QHBoxLayout()
        
        save_recipe_btn = QPushButton("Save Recipe")
        save_recipe_btn.clicked.connect(self.save_recipe)
        save_buttons.addWidget(save_recipe_btn)
        
        clear_recipe_btn = QPushButton("Clear Recipe")
        clear_recipe_btn.clicked.connect(self.clear_recipe)
        save_buttons.addWidget(clear_recipe_btn)
        
        save_buttons.addStretch()
        builder_layout.addLayout(save_buttons)
        
        builder_group.setLayout(builder_layout)
        layout.addWidget(builder_group)
        
        # Recipe execution section
        exec_group = QGroupBox("Run Recipe")
        exec_layout = QVBoxLayout()
        
        # Recipe selector
        select_layout = QHBoxLayout()
        select_layout.addWidget(QLabel("Select Recipe:"))
        self.recipe_selector = QLineEdit()
        self.recipe_selector.setReadOnly(True)
        self.recipe_selector.setPlaceholderText("Click 'Load Recipe' to choose")
        select_layout.addWidget(self.recipe_selector)
        
        load_btn = QPushButton("Load Recipe")
        load_btn.clicked.connect(self.load_recipe)
        select_layout.addWidget(load_btn)
        exec_layout.addLayout(select_layout)
        
        # Recipe info display
        self.recipe_info = QTextEdit()
        self.recipe_info.setReadOnly(True)
        self.recipe_info.setMaximumHeight(120)
        exec_layout.addWidget(self.recipe_info)
        
        # Recipe progress
        recipe_progress_layout = QHBoxLayout()
        recipe_progress_layout.addWidget(QLabel("Recipe Step:"))
        self.recipe_step_label = QLabel("--")
        self.recipe_step_label.setStyleSheet("font-weight: bold;")
        recipe_progress_layout.addWidget(self.recipe_step_label)
        recipe_progress_layout.addStretch()
        exec_layout.addLayout(recipe_progress_layout)
        
        # Run button
        self.run_recipe_btn = QPushButton("Run Recipe")
        self.run_recipe_btn.clicked.connect(self.run_recipe)
        self.run_recipe_btn.setStyleSheet("background-color: #3498db; color: white; font-weight: bold; padding: 10px;")
        self.run_recipe_btn.setEnabled(False)
        exec_layout.addWidget(self.run_recipe_btn)
        
        exec_group.setLayout(exec_layout)
        layout.addWidget(exec_group)
        
        layout.addStretch()
        self.tabs.addTab(widget, "Recipes")
    
    def setup_temperature_chart(self):
        """Setup the temperature monitoring chart with time-based X axis"""
        self.chart = QChart()
        self.chart.setTitle("Temperature vs Time")
        self.chart.setAnimationOptions(QChart.AnimationOption.NoAnimation)
        
        # Create series for each thermocouple
        self.tc2_series = QLineSeries()
        self.tc2_series.setName("TC2 (Delivery)")
         # Match series color to reading label color
        self.tc2_series.setPen(QPen(QColor('#2980b9'), 2))
        
        self.tc3_series = QLineSeries()
        self.tc3_series.setName("TC3 (Precursor 1)")
        self.tc3_series.setPen(QPen(QColor('#27ae60'), 2))
        
        self.tc4_series = QLineSeries()
        self.tc4_series.setName("TC4 (Precursor 2)")
        self.tc4_series.setPen(QPen(QColor('#e67e22'), 2))
        
        self.tc5_series = QLineSeries()
        self.tc5_series.setName("TC5 (Substrate)")
        self.tc5_series.setPen(QPen(QColor('#c0392b'), 2))
        
        self.chart.addSeries(self.tc2_series)
        self.chart.addSeries(self.tc3_series)
        self.chart.addSeries(self.tc4_series)
        self.chart.addSeries(self.tc5_series)
        
        # Setup time-based X axis
        self.axis_x = QValueAxis()
        self.axis_x.setTitleText("Time (seconds)")
        self.axis_x.setRange(0, 120)
        self.axis_x.setLabelFormat("%d")
        
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
    
    def format_delta(self, current, setpoint):
        """Format delta value with color coding"""
        if current is None:
            return "--", "color: #7f8c8d;"
        
        delta = current - setpoint
        if abs(delta) <= 2:
            # At setpoint (within 2°C)
            return f"±{abs(delta):.1f}°C", "color: #27ae60; font-weight: bold;"  # Green
        elif delta > 0:
            # Over setpoint
            if delta > 50:
                return f"+{delta:.1f}°C", "color: #c0392b; font-weight: bold; background-color: #ffcccc;"  # Red with highlight
            elif delta > 20:
                return f"+{delta:.1f}°C", "color: #c0392b; font-weight: bold;"  # Red
            else:
                return f"+{delta:.1f}°C", "color: #e67e22; font-weight: bold;"  # Orange
        else:
            # Under setpoint
            return f"{delta:.1f}°C", "color: #3498db; font-weight: bold;"  # Blue
    
    def update_graph(self):
        """Update the temperature graph with latest data - optimized for performance"""
        if not self.graph_update_pending or len(self.temp_data['timestamp']) == 0:
            self.update_sensor_readings()
            return
        
        self.graph_update_pending = False
        
        if self.start_time is None:
            return
        
        max_display_points = 500
        
        data_len = len(self.temp_data['timestamp'])
        start_idx = max(0, data_len - max_display_points)
        
        self.tc2_series.clear()
        self.tc3_series.clear()
        self.tc4_series.clear()
        self.tc5_series.clear()
        
        timestamps = list(self.temp_data['timestamp'])
        tc2_vals = list(self.temp_data['tc2'])
        tc3_vals = list(self.temp_data['tc3'])
        tc4_vals = list(self.temp_data['tc4'])
        tc5_vals = list(self.temp_data['tc5'])
        
        for i in range(start_idx, data_len):
            time_sec = (timestamps[i] - self.start_time).total_seconds()
            self.tc2_series.append(time_sec, tc2_vals[i])
            self.tc3_series.append(time_sec, tc3_vals[i])
            self.tc4_series.append(time_sec, tc4_vals[i])
            self.tc5_series.append(time_sec, tc5_vals[i])
        
        self.update_sensor_readings()
        
        if data_len > 0:
            all_temps = tc2_vals[start_idx:] + tc3_vals[start_idx:] + \
                       tc4_vals[start_idx:] + tc5_vals[start_idx:]
            if all_temps:
                min_temp = min(all_temps)
                max_temp = max(all_temps)
                range_temp = max_temp - min_temp if max_temp > min_temp else 10
                self.axis_y.setRange(max(0, min_temp - range_temp * 0.1), 
                                    max_temp + range_temp * 0.1)
            
            max_time = (timestamps[-1] - self.start_time).total_seconds()
            min_time = (timestamps[start_idx] - self.start_time).total_seconds() if start_idx > 0 else 0
            self.axis_x.setRange(min_time, max(max_time + 5, 120))
    
    def update_sensor_readings(self):
        """Update the sensor reading labels with setpoint comparison"""
        if len(self.temp_data['tc2']) > 0:
            tc2_val = self.temp_data['tc2'][-1]
            tc3_val = self.temp_data['tc3'][-1]
            tc4_val = self.temp_data['tc4'][-1]
            tc5_val = self.temp_data['tc5'][-1]
            
            # Update readings
            self.tc2_reading.setText(f"{tc2_val:.1f}°C")
            self.tc3_reading.setText(f"{tc3_val:.1f}°C")
            self.tc4_reading.setText(f"{tc4_val:.1f}°C")
            self.tc5_reading.setText(f"{tc5_val:.1f}°C")
            
            # Update setpoint displays (with arrow prefix)
            self.tc2_setpoint_display.setText(f"→ {self.temp_setpoints['tc2']}°C")
            self.tc3_setpoint_display.setText(f"→ {self.temp_setpoints['tc3']}°C")
            self.tc4_setpoint_display.setText(f"→ {self.temp_setpoints['tc4']}°C")
            self.tc5_setpoint_display.setText(f"→ {self.temp_setpoints['tc5']}°C")
            
            # Update deltas with color coding
            delta_text, delta_style = self.format_delta(tc2_val, self.temp_setpoints['tc2'])
            self.tc2_delta.setText(delta_text)
            self.tc2_delta.setStyleSheet(delta_style)
            
            delta_text, delta_style = self.format_delta(tc3_val, self.temp_setpoints['tc3'])
            self.tc3_delta.setText(delta_text)
            self.tc3_delta.setStyleSheet(delta_style)
            
            delta_text, delta_style = self.format_delta(tc4_val, self.temp_setpoints['tc4'])
            self.tc4_delta.setText(delta_text)
            self.tc4_delta.setStyleSheet(delta_style)
            
            delta_text, delta_style = self.format_delta(tc5_val, self.temp_setpoints['tc5'])
            self.tc5_delta.setText(delta_text)
            self.tc5_delta.setStyleSheet(delta_style)
        
        if len(self.pressure_data['value']) > 0:
            unit = self.pressure_data['unit'][-1] if self.pressure_data['unit'] else 'mTorr'
            self.pressure_reading.setText(f"Pressure: {self.pressure_data['value'][-1]:.2f} {unit}")
        
        if len(self.flow_data['value']) > 0:
            self.flow_reading.setText(f"Precursor Gas Box Airflow: {self.flow_data['value'][-1]:.2f} m/s")
            # Check flow alarm status
            self.check_flow_alarm()
    
    def check_flow_alarm(self):
        """Check flow sensor and trigger/dismiss non-latching alarm"""
        if len(self.flow_data['value']) == 0:
            return
        
        current_flow = self.flow_data['value'][-1]
        
        # Flow is below threshold - alarm should be active
        if current_flow < self.flow_alarm_threshold:
            if not self.flow_alarm_active:
                self.flow_alarm_active = True
                self.show_flow_alarm()
        # Flow is above threshold - alarm should be dismissed
        else:
            if self.flow_alarm_active:
                self.flow_alarm_active = False
                self.dismiss_flow_alarm()
    
    def show_flow_alarm(self):
        """Display non-latching flow alarm dialog with sound"""
        if self.flow_alarm_dialog is None or not self.flow_alarm_dialog.isVisible():
            self.flow_alarm_dialog = QMessageBox(self)
            self.flow_alarm_dialog.setWindowTitle("⚠️ AIRFLOW ALARM")
            self.flow_alarm_dialog.setText(
                f"⚠️ AIRFLOW WARNING ⚠️\n\n"
                f"Precursor Gas Box airflow has dropped below {self.flow_alarm_threshold} m/s!\n\n"
                f"Current airflow: {self.flow_data['value'][-1]:.2f} m/s\n\n"
                f"Please check ventilation system."
            )
            self.flow_alarm_dialog.setIcon(QMessageBox.Icon.Warning)
            self.flow_alarm_dialog.setStandardButtons(QMessageBox.StandardButton.NoButton)
            self.flow_alarm_dialog.show()
            
            # Play alarm sound
            try:
                winsound.Beep(1000, 500)  # 1000 Hz for 500ms
            except Exception as e:
                print(f"Could not play alarm sound: {e}")
    
    def dismiss_flow_alarm(self):
        """Dismiss the flow alarm dialog"""
        if self.flow_alarm_dialog is not None:
            self.flow_alarm_dialog.close()
            self.flow_alarm_dialog = None
    
    
    def update_job_progress(self):
        """Update the job progress bar and time remaining"""
        if self.job_start_time is None or self.job_total_duration == 0:
            return
        
        elapsed = (datetime.now() - self.job_start_time).total_seconds() * 1000
        remaining_ms = max(0, self.job_total_duration - elapsed)
        progress = min(100, int((elapsed / self.job_total_duration) * 100))
        
        self.progress_bar.setValue(progress)
        
        remaining_sec = remaining_ms / 1000
        if remaining_sec >= 60:
            mins = int(remaining_sec // 60)
            secs = int(remaining_sec % 60)
            self.time_remaining_label.setText(f"{mins}m {secs}s remaining")
        else:
            self.time_remaining_label.setText(f"{remaining_sec:.1f}s remaining")
        
        if remaining_ms <= 0:
            self.job_timer.stop()
            self.progress_bar.setValue(100)
            self.time_remaining_label.setText("Complete")

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
        
        # Export command log button
        export_log_layout = QHBoxLayout()
        export_log_btn = QPushButton("Export Command Log to CSV")
        export_log_btn.clicked.connect(self.export_command_log)
        export_log_layout.addWidget(export_log_btn)
        export_log_layout.addStretch()
        layout.addLayout(export_log_layout)
        
        # Full log
        layout.addWidget(QLabel("Full Communication Log:"))
        self.full_log = QTextEdit()
        self.full_log.setReadOnly(True)
        layout.addWidget(self.full_log)
        
        self.tabs.addTab(widget, "Commands & Log")
    
    def calculate_valve_duration(self, num_pulses, pulse_time, purge_time):
        """Calculate total duration of a valve command in milliseconds"""
        return num_pulses * (pulse_time + purge_time)
    
    @asyncSlot()
    async def connect_arduino(self):
        if self.operation_in_progress:
            return
            
        port = self.port_input.text()
        self.operation_in_progress = True
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
        finally:
            self.operation_in_progress = False
    
    @asyncSlot()
    async def disconnect_arduino(self):
        self.operation_in_progress = True
        try:
            await self.controller.disconnect()
        except Exception as e:
            self.log_text.append(f"[WARNING] Disconnect error (forcing cleanup): {e}")
        finally:
            # Always update UI state even if disconnect had errors
            self.status_label.setText("Not Connected")
            self.status_label.setStyleSheet("background-color: #e74c3c; color: white; padding: 10px; font-weight: bold;")
            self.connect_btn.setEnabled(True)
            self.disconnect_btn.setEnabled(False)
            self.estop_btn.setEnabled(False)
            self.reset_btn.setEnabled(False)
            self.valve_job_running = False
            self.job_timer.stop()
            self.operation_in_progress = False
            self.log_text.append("Disconnected")
    
    @asyncSlot()
    async def send_valve(self):
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
            
        if self.valve_job_running:
            self.log_text.append(f"[DEBUG] Blocked valve command - job already running")
            QMessageBox.warning(
                self,
                "Job Already Running",
                "A valve command is already in progress.\n\n"
                "Please wait for it to complete or click 'Reset Valves' to cancel."
            )
            return
        
        # Validate inputs first (before any async operations)
        try:
            valve_id_text = self.valve_id_input.text().strip()
            num_pulses_text = self.num_pulses_input.text().strip()
            pulse_time_text = self.pulse_time_input.text().strip()
            purge_time_text = self.purge_time_input.text().strip()
            
            if not all([valve_id_text, num_pulses_text, pulse_time_text, purge_time_text]):
                QMessageBox.warning(self, "Input Error", "All valve fields must be filled in.")
                return
            
            valve_id = int(valve_id_text)
            num_pulses = int(num_pulses_text)
            pulse_time = int(pulse_time_text)
            purge_time = int(purge_time_text)
            
            # Range validation
            if valve_id < 1 or valve_id > 3:
                QMessageBox.warning(self, "Input Error", "Valve ID must be 1, 2, or 3")
                return
            if num_pulses < 1 or num_pulses > 10000:
                QMessageBox.warning(self, "Input Error", "Number of pulses must be between 1 and 10000")
                return
            if pulse_time < 1 or pulse_time > 60000:
                QMessageBox.warning(self, "Input Error", "Pulse time must be between 1 and 60000 ms")
                return
            if purge_time < 0 or purge_time > 60000:
                QMessageBox.warning(self, "Input Error", "Purge time must be between 0 and 60000 ms")
                return
                
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid whole numbers for all valve fields.")
            return
        
        self.operation_in_progress = True
        try:
            # Calculate and start progress tracking
            self.job_total_duration = self.calculate_valve_duration(num_pulses, pulse_time, purge_time)
            self.job_start_time = datetime.now()
            self.progress_bar.setValue(0)
            self.job_timer.start(100)
            
            self.valve_job_running = True
            self.log_text.append(f"[DEBUG] Set valve_job_running = True")
            self.log_text.append(f"[DEBUG] Estimated duration: {self.job_total_duration/1000:.1f}s")
            
            await self.controller.valve(valve_id, num_pulses, pulse_time, purge_time)
            
            cmd_str = f"v{valve_id};{num_pulses};{pulse_time};{purge_time}"
            self.log_text.append(f"Sent: {cmd_str}")
            self.log_command('VALVE', cmd_str, f'Valve {valve_id}, {num_pulses} pulses')
            
        except ValidationError as e:
            self.valve_job_running = False
            self.job_timer.stop()
            QMessageBox.warning(self, "Validation Error", str(e))
        except Exception as e:
            self.valve_job_running = False
            self.job_timer.stop()
            QMessageBox.critical(self, "Error", str(e))
        finally:
            self.operation_in_progress = False
    
    @asyncSlot()
    async def send_temp(self):
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
            
        if self.valve_job_running:
            reply = QMessageBox.question(
                self,
                "Valve Job Running",
                "A valve command is currently in progress.\n\n"
                "Sending temperature commands during valve operation\n"
                "may cause the GUI to freeze.\n\n"
                "Continue anyway?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No
            )
            if reply == QMessageBox.StandardButton.No:
                return
        
        # Validate inputs first (before any async operations)
        try:
            tc2_text = self.tc2_input.text().strip()
            tc3_text = self.tc3_input.text().strip()
            tc4_text = self.tc4_input.text().strip()
            tc5_text = self.tc5_input.text().strip()
            
            # Check for empty or invalid inputs
            if not all([tc2_text, tc3_text, tc4_text, tc5_text]):
                QMessageBox.warning(self, "Input Error", "All temperature fields must be filled in.")
                return
            
            tc2 = int(float(tc2_text))
            tc3 = int(float(tc3_text))
            tc4 = int(float(tc4_text))
            tc5 = int(float(tc5_text))
            
            # Range validation
            for name, val in [("TC2", tc2), ("TC3", tc3), ("TC4", tc4), ("TC5", tc5)]:
                if val < 0 or val > 500:
                    QMessageBox.warning(self, "Input Error", f"{name} must be between 0 and 500°C")
                    return
                    
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid numbers for all temperature fields.\n\nExample: 50, 30.5, etc.")
            return
        
        self.operation_in_progress = True
        try:
            # Update setpoints for safety monitoring and display
            self.temp_setpoints = {'tc2': tc2, 'tc3': tc3, 'tc4': tc4, 'tc5': tc5}
            
            # Update setpoint display labels immediately (with arrow prefix)
            self.tc2_setpoint_display.setText(f"→ {tc2}°C")
            self.tc3_setpoint_display.setText(f"→ {tc3}°C")
            self.tc4_setpoint_display.setText(f"→ {tc4}°C")
            self.tc5_setpoint_display.setText(f"→ {tc5}°C")
            
            await self.controller.temp(tc2, tc3, tc4, tc5)
            
            cmd_str = f"t{tc2};{tc3};{tc4};{tc5}"
            self.log_text.append(f"Sent: {cmd_str}")
            self.log_command('TEMP', cmd_str, f'Targets: TC2={tc2}, TC3={tc3}, TC4={tc4}, TC5={tc5}')
            
        except ValidationError as e:
            QMessageBox.warning(self, "Validation Error", str(e))
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
        finally:
            self.operation_in_progress = False
    
    @asyncSlot()
    async def send_test(self):
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
            
        self.operation_in_progress = True
        try:
            await self.controller.test()
            self.log_text.append("Sent: TEST")
            self.log_command('TEST', 'TEST', 'Test communication')
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
        finally:
            self.operation_in_progress = False
    
    @asyncSlot()
    async def send_begin(self):
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
            
        self.operation_in_progress = True
        try:
            await self.controller.begin()
            self.log_text.append("Sent: BEGIN")
            self.log_command('BEGIN', 'BEGIN', 'Start sequence')
        except Exception as e:
            QMessageBox.critical(self, "Error", str(e))
        finally:
            self.operation_in_progress = False
    
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
            self.operation_in_progress = True
            try:
                await self.controller.estop()
                self.job_timer.stop()
                self.valve_job_running = False
                self.log_text.append("!!! EMERGENCY STOP TRIGGERED !!!")
                self.log_text.append("!!! ARDUINO LOCKED - RESTART REQUIRED !!!")
                self.status_label.setText("🚨 EMERGENCY STOP - Arduino Locked")
                self.status_label.setStyleSheet("background-color: #c0392b; color: white; padding: 10px; font-weight: bold;")
                
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
            finally:
                self.operation_in_progress = False
    
    @asyncSlot()
    async def reset_system(self):
        """Send reset command - clears pulse counter without locking"""
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
            
        self.operation_in_progress = True
        try:
            await self.controller.reset()
            self.valve_job_running = False
            self.job_timer.stop()
            self.progress_bar.setValue(0)
            self.time_remaining_label.setText("--")
            self.log_text.append("Reset command sent - pulse counter cleared")
            self.log_text.append(f"[DEBUG] Set valve_job_running = False (manual reset)")
            QMessageBox.information(
                self,
                "Reset Complete",
                "Valve pulse counter has been reset.\n\n"
                "System is ready for new commands."
            )
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to send reset: {e}")
        finally:
            self.operation_in_progress = False
    
    def reset_status_after_estop(self):
        """Reset status label after emergency stop"""
        if self.controller.connected:
            self.status_label.setText(f"Connected to {self.port_input.text()}")
            self.status_label.setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;")
    
    def update_status(self):
        """Update connection status - non-async to prevent task conflicts"""
        try:
            if self.controller.is_connected():
                if "Not Connected" in self.status_label.text():
                    self.status_label.setText(f"Connected to {self.port_input.text()}")
                    self.status_label.setStyleSheet("background-color: #27ae60; color: white; padding: 10px; font-weight: bold;")
        except Exception:
            pass  # Ignore errors in status check
    
    def log_command(self, log_type, command, details=''):
        """Add command to history for later export"""
        self.command_history.append({
            'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
            'type': log_type,
            'command': command,
            'details': details
        })
    
    def export_command_log(self):
        """Export command history to CSV file"""
        if len(self.command_history) == 0:
            QMessageBox.warning(self, "No Commands", "No commands have been sent yet.")
            return
        
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "Export Command Log",
            f"command_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
            "CSV Files (*.csv)"
        )
        
        if filename:
            try:
                with open(filename, 'w', newline='') as f:
                    writer = csv.writer(f)
                    writer.writerow(['Timestamp', 'Type', 'Command', 'Details'])
                    
                    for entry in self.command_history:
                        writer.writerow([
                            entry['timestamp'],
                            entry['type'],
                            entry['command'],
                            entry['details']
                        ])
                
                QMessageBox.information(
                    self,
                    "Export Successful",
                    f"Command log exported to:\n{filename}\n\n{len(self.command_history)} commands saved."
                )
            except Exception as e:
                QMessageBox.critical(self, "Export Failed", f"Could not export log:\n{e}")
    
    def export_temperature_data(self):
        """Export temperature data to CSV file"""
        if len(self.temp_data['time']) == 0:
            QMessageBox.warning(self, "No Data", "No temperature data to export yet.")
            return
        
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "Export Temperature Data",
            f"temp_data_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
            "CSV Files (*.csv)"
        )
        
        if filename:
            try:
                with open(filename, 'w', newline='') as f:
                    writer = csv.writer(f)
                    writer.writerow(['Timestamp', 'Time (s)', 'TC2 (°C)', 'TC3 (°C)', 'TC4 (°C)', 'TC5 (°C)'])
                    
                    for i in range(len(self.temp_data['time'])):
                        time_sec = (self.temp_data['timestamp'][i] - self.start_time).total_seconds() if self.start_time else 0
                        writer.writerow([
                            self.temp_data['timestamp'][i].strftime('%Y-%m-%d %H:%M:%S.%f')[:-3],
                            f"{time_sec:.1f}",
                            self.temp_data['tc2'][i],
                            self.temp_data['tc3'][i],
                            self.temp_data['tc4'][i],
                            self.temp_data['tc5'][i]
                        ])
                
                QMessageBox.information(
                    self,
                    "Export Successful",
                    f"Temperature data exported to:\n{filename}\n\n{len(self.temp_data['time'])} samples saved."
                )
            except Exception as e:
                QMessageBox.critical(self, "Export Failed", f"Could not export data:\n{e}")
    
    def add_valve_to_recipe(self):
        """Add current valve settings to recipe"""
        if self.current_recipe is None:
            self.current_recipe = Recipe(
                self.recipe_name_input.text() or "Unnamed Recipe",
                self.recipe_desc_input.text()
            )
        
        try:
            valve_id = int(self.valve_id_input.text())
            num_pulses = int(self.num_pulses_input.text())
            pulse_time = int(self.pulse_time_input.text())
            purge_time = int(self.purge_time_input.text())
            
            self.current_recipe.add_valve_step(valve_id, num_pulses, pulse_time, purge_time)
            self.update_recipe_preview()
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid valve parameters")
    
    def add_temp_to_recipe(self):
        """Add current temperature settings to recipe"""
        if self.current_recipe is None:
            self.current_recipe = Recipe(
                self.recipe_name_input.text() or "Unnamed Recipe",
                self.recipe_desc_input.text()
            )
        
        try:
            tc2 = int(float(self.tc2_input.text()))
            tc3 = int(float(self.tc3_input.text()))
            tc4 = int(float(self.tc4_input.text()))
            tc5 = int(float(self.tc5_input.text()))
            
            self.current_recipe.add_temp_step(tc2, tc3, tc4, tc5)
            self.update_recipe_preview()
        except ValueError:
            QMessageBox.warning(self, "Input Error", "Please enter valid temperature parameters")
    
    def add_wait_to_recipe(self):
        """Add wait step to recipe"""
        if self.current_recipe is None:
            self.current_recipe = Recipe(
                self.recipe_name_input.text() or "Unnamed Recipe",
                self.recipe_desc_input.text()
            )
        
        duration, ok = QInputDialog.getInt(
            self,
            "Wait Duration",
            "Enter wait time (seconds):",
            60, 1, 3600, 1
        )
        
        if ok:
            self.current_recipe.add_wait_step(duration)
            self.update_recipe_preview()
    
    def update_recipe_preview(self):
        """Update the recipe preview text"""
        if self.current_recipe:
            self.recipe_preview.setText(self.current_recipe.get_summary())
    
    def save_recipe(self):
        """Save current recipe to file"""
        if self.current_recipe is None or len(self.current_recipe.steps) == 0:
            QMessageBox.warning(self, "No Recipe", "No recipe to save. Add some steps first.")
            return
        
        if not self.recipe_name_input.text():
            QMessageBox.warning(self, "No Name", "Please enter a recipe name.")
            return
        
        self.current_recipe.name = self.recipe_name_input.text()
        self.current_recipe.description = self.recipe_desc_input.text()
        
        try:
            self.current_recipe.save()
            QMessageBox.information(
                self,
                "Recipe Saved",
                f"Recipe '{self.current_recipe.name}' saved successfully!"
            )
        except Exception as e:
            QMessageBox.critical(self, "Save Failed", f"Could not save recipe:\n{e}")
    
    def clear_recipe(self):
        """Clear current recipe"""
        self.current_recipe = None
        self.recipe_preview.clear()
        self.recipe_name_input.clear()
        self.recipe_desc_input.clear()
    
    def load_recipe(self):
        """Load a recipe from file"""
        recipes = Recipe.list_recipes()
        if not recipes:
            QMessageBox.information(self, "No Recipes", "No saved recipes found.\n\nCreate and save a recipe first!")
            return
        
        recipe_name, ok = QInputDialog.getItem(
            self,
            "Load Recipe",
            "Select a recipe:",
            recipes,
            0,
            False
        )
        
        if ok and recipe_name:
            try:
                self.current_recipe = Recipe.load(f"recipes/{recipe_name}.json")
                self.recipe_selector.setText(recipe_name)
                self.recipe_info.setText(self.current_recipe.get_summary())
                self.run_recipe_btn.setEnabled(True)
            except Exception as e:
                QMessageBox.critical(self, "Load Failed", f"Could not load recipe:\n{e}")
    
    @asyncSlot()
    async def run_recipe(self):
        """Execute the loaded recipe"""
        if self.current_recipe is None:
            QMessageBox.warning(self, "No Recipe", "Please load a recipe first.")
            return
        
        if self.recipe_running:
            QMessageBox.warning(self, "Recipe Running", "A recipe is already running.")
            return
        
        if self.operation_in_progress:
            QMessageBox.warning(self, "Busy", "Another operation is in progress. Please wait.")
            return
        
        # Calculate total recipe duration
        total_duration_ms = 0
        for step in self.current_recipe.steps:
            if step['type'] == 'valve':
                total_duration_ms += self.calculate_valve_duration(
                    step['num_pulses'], step['pulse_time'], step['purge_time']
                )
            elif step['type'] == 'wait':
                total_duration_ms += step['duration'] * 1000
        
        total_duration_str = f"{total_duration_ms/1000:.0f} seconds ({total_duration_ms/60000:.1f} minutes)"
        
        reply = QMessageBox.question(
            self,
            "Run Recipe",
            f"Run recipe '{self.current_recipe.name}'?\n\n"
            f"Steps: {len(self.current_recipe.steps)}\n"
            f"Estimated duration: {total_duration_str}\n\n"
            f"You can use Emergency Stop if needed.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        )
        
        if reply != QMessageBox.StandardButton.Yes:
            return
        
        self.recipe_running = True
        self.run_recipe_btn.setEnabled(False)
        self.recipe_total_steps = len(self.current_recipe.steps)
        
        # Setup overall recipe progress tracking
        self.job_total_duration = total_duration_ms
        self.job_start_time = datetime.now()
        self.progress_bar.setValue(0)
        self.job_timer.start(100)
        
        try:
            for i, step in enumerate(self.current_recipe.steps, 1):
                self.recipe_step_index = i
                self.recipe_step_label.setText(f"{i}/{self.recipe_total_steps}")
                self.log_text.append(f"[RECIPE] Step {i}/{self.recipe_total_steps}")
                
                if step['type'] == 'valve':
                    self.log_text.append(f"[RECIPE] Executing valve command...")
                    await self.controller.valve(
                        step['valve_id'],
                        step['num_pulses'],
                        step['pulse_time'],
                        step['purge_time']
                    )
                    self.valve_job_running = True
                    
                    # Wait for valve job to complete
                    timeout = 0
                    while self.valve_job_running and timeout < 600:
                        await asyncio.sleep(1)
                        timeout += 1
                    
                    if timeout >= 600:
                        raise Exception("Valve command timed out")
                
                elif step['type'] == 'temp':
                    self.log_text.append(f"[RECIPE] Setting temperatures...")
                    await self.controller.temp(
                        step['tc2'],
                        step['tc3'],
                        step['tc4'],
                        step['tc5']
                    )
                    # Update setpoints for safety monitoring
                    self.temp_setpoints = {
                        'tc2': step['tc2'],
                        'tc3': step['tc3'],
                        'tc4': step['tc4'],
                        'tc5': step['tc5']
                    }
                    # Update display (with arrow prefix)
                    self.tc2_setpoint_display.setText(f"→ {step['tc2']}°C")
                    self.tc3_setpoint_display.setText(f"→ {step['tc3']}°C")
                    self.tc4_setpoint_display.setText(f"→ {step['tc4']}°C")
                    self.tc5_setpoint_display.setText(f"→ {step['tc5']}°C")
                
                elif step['type'] == 'wait':
                    self.log_text.append(f"[RECIPE] Waiting {step['duration']} seconds...")
                    await asyncio.sleep(step['duration'])
            
            self.log_text.append(f"[RECIPE] Recipe '{self.current_recipe.name}' completed successfully!")
            self.recipe_step_label.setText("Complete")
            QMessageBox.information(self, "Recipe Complete", "Recipe executed successfully!")
            
        except Exception as e:
            self.log_text.append(f"[RECIPE] Error: {e}")
            QMessageBox.critical(self, "Recipe Failed", f"Recipe execution failed:\n{e}")
        
        finally:
            self.recipe_running = False
            self.run_recipe_btn.setEnabled(True)
            self.job_timer.stop()
            self.progress_bar.setValue(100)
            self.time_remaining_label.setText("Complete")

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