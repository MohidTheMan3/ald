import json
from datetime import datetime
from pathlib import Path
from typing import List, Dict

class Recipe:
    def __init__(self, name: str, description: str = ""):
        self.name = name
        self.description = description
        self.created = datetime.now().isoformat()
        self.steps = []
    
    def add_valve_step(self, valve_id: int, num_pulses: int, pulse_time: int, purge_time: int):
        """Add a valve command to the recipe"""
        self.steps.append({
            'type': 'valve',
            'valve_id': valve_id,
            'num_pulses': num_pulses,
            'pulse_time': pulse_time,
            'purge_time': purge_time
        })
    
    def add_temp_step(self, tc2: int, tc3: int, tc4: int, tc5: int):
        """Add a temperature command to the recipe"""
        self.steps.append({
            'type': 'temp',
            'tc2': tc2,
            'tc3': tc3,
            'tc4': tc4,
            'tc5': tc5
        })
    
    def add_wait_step(self, duration: int):
        """Add a wait/delay step (in seconds)"""
        self.steps.append({
            'type': 'wait',
            'duration': duration
        })
    
    def to_dict(self) -> dict:
        """Convert recipe to dictionary for saving"""
        return {
            'name': self.name,
            'description': self.description,
            'created': self.created,
            'steps': self.steps
        }
    
    @staticmethod
    def from_dict(data: dict) -> 'Recipe':
        """Create recipe from dictionary"""
        recipe = Recipe(data['name'], data.get('description', ''))
        recipe.created = data.get('created', datetime.now().isoformat())
        recipe.steps = data.get('steps', [])
        return recipe
    
    def save(self, filename: str = None):
        """Save recipe to JSON file"""
        if filename is None:
            filename = f"recipes/{self.name.replace(' ', '_')}.json"
        
        Path("recipes").mkdir(exist_ok=True)
        
        with open(filename, 'w') as f:
            json.dump(self.to_dict(), f, indent=2)
    
    @staticmethod
    def load(filename: str) -> 'Recipe':
        """Load recipe from JSON file"""
        with open(filename, 'r') as f:
            data = json.load(f)
        return Recipe.from_dict(data)
    
    @staticmethod
    def list_recipes() -> List[str]:
        """List all available recipe files"""
        recipes_dir = Path("recipes")
        if not recipes_dir.exists():
            return []
        return [f.stem for f in recipes_dir.glob("*.json")]
    
    def get_summary(self) -> str:
        """Get a text summary of the recipe"""
        summary = f"Recipe: {self.name}\n"
        if self.description:
            summary += f"Description: {self.description}\n"
        summary += f"Steps: {len(self.steps)}\n"
        summary += "-" * 40 + "\n"
        
        for i, step in enumerate(self.steps, 1):
            if step['type'] == 'valve':
                summary += f"{i}. Valve {step['valve_id']}: {step['num_pulses']} pulses, {step['pulse_time']}ms pulse, {step['purge_time']}ms purge\n"
            elif step['type'] == 'temp':
                summary += f"{i}. Temperature: TC2={step['tc2']}°C, TC3={step['tc3']}°C, TC4={step['tc4']}°C, TC5={step['tc5']}°C\n"
            elif step['type'] == 'wait':
                summary += f"{i}. Wait: {step['duration']} seconds\n"
        
        return summary