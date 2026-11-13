"""The acceptance gate must distinguish missing environments from failed proof."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GATE = Path(__file__).resolve().parents[1] / 'usable_roundtrip.py'


class GateTests(unittest.TestCase):
    def test_missing_environment_is_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'result'
            run = subprocess.run([sys.executable, GATE, '--platforms', 'dos',
                '--dosbox-x', 'boltpack-test-missing-emulator', '--output', out],
                capture_output=True, text=True)
            self.assertEqual(run.returncode, 2, run.stderr)
            record = json.loads((out / 'run.json').read_text())
            self.assertEqual(record['status'], 'unavailable')
            self.assertEqual(record['results'][0]['checks'], [])
            self.assertIn('missing tool', str(record['results'][0]['reason']))

    def test_zero_exit_without_restored_application_is_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake = root / 'fake-archiver'
            fake.write_text('#!/bin/sh\nexit 0\n')
            fake.chmod(0o755)
            out = root / 'result'
            run = subprocess.run([sys.executable, GATE, '--platforms', 'linux,dos',
                '--linux-cli', fake, '--dosbox-x', 'boltpack-test-missing-emulator',
                '--output', out], capture_output=True, text=True)
            self.assertEqual(run.returncode, 1, run.stderr)
            record = json.loads((out / 'run.json').read_text())
            self.assertEqual(record['status'], 'fail')
            self.assertEqual([r['status'] for r in record['results']], ['fail', 'unavailable'])
            self.assertTrue(list((out / 'linux').glob('command-*.log')))

    def test_command_failure_retains_status_and_log(self):
        module_spec = importlib.util.spec_from_file_location('usable_gate', GATE)
        gate = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(gate)
        with tempfile.TemporaryDirectory() as tmp:
            evidence = gate.Evidence(Path(tmp))
            with self.assertRaises(RuntimeError):
                evidence.run([sys.executable, '-c', 'print("failure evidence"); raise SystemExit(7)'])
            self.assertEqual(evidence.commands[0]['returncode'], 7)
            self.assertIn('failure evidence', (Path(tmp) / 'command-000.log').read_text())


if __name__ == '__main__':
    unittest.main()
