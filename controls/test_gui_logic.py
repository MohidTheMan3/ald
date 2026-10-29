from ald_gui import SpikeDetector, PrecursorUsage, validate_step
import pytest


def test_back_to_back_spikes_each_detected():
    # Three pulses; pressure only partly recovers between them (never back to 100).
    trace = [100, 100, 106, 110, 107, 104, 103, 109, 112, 108, 105, 111, 113, 109, 104, 101]
    det = SpikeDetector(threshold=1.5)
    spikes = [s for s in map(det.update, trace) if s]
    assert len(spikes) == 3
    assert spikes[0] == (10, 110)          # 100 -> 110
    assert spikes[1] == (9, 112)           # trough 103 -> 112


def test_noise_below_threshold_ignored():
    det = SpikeDetector(threshold=1.5)
    assert not any(det.update(v) for v in [100, 101, 100, 101.2, 100.3, 101, 100])


def test_usage_persists_and_refill_keeps_lifetime(tmp_path):
    path = tmp_path / "usage.json"
    u = PrecursorUsage(path)
    u.record_pulse(1, 100)
    u.record_pulse(1, 150)
    u.mark_refilled(1)
    u.record_pulse(1, 100)

    e = PrecursorUsage(path).data['1']     # reload from disk
    assert (e['total_pulses'], e['total_open_ms']) == (3, 350)
    assert (e['pulses_since_refill'], e['open_ms_since_refill']) == (1, 100)
    assert e['refilled'] is not None
    assert PrecursorUsage(path).data['2']['total_pulses'] == 0


def test_validate_step_rejects_bad_recipe_steps():
    validate_step({'type': 'valve', 'valve_id': 1, 'num_pulses': 1, 'pulse_time': 100, 'purge_time': 4000})
    with pytest.raises(ValueError):
        validate_step({'type': 'valve', 'valve_id': 4, 'num_pulses': 1, 'pulse_time': 100, 'purge_time': 4000})
    with pytest.raises(ValueError):
        validate_step({'type': 'temp', 'tc2': 600, 'tc3': 0, 'tc4': 0, 'tc5': 0})
