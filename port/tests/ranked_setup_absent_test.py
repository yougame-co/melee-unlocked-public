"""Production variants contain neither the hook parser key nor the CPU bypass marker."""
from pathlib import Path
import sys

# The repository-wide *_test.py loop runs without binary arguments. CTest supplies
# the built variants for the actual binary check.
assert len(sys.argv) in (1, 3), 'expected non-hook CPU parser and adapter binaries'
for arg in sys.argv[1:]:
    data = Path(arg).read_bytes()
    for marker in (b'rankedCpu', b'agent-ranked-cpu', b'Gold II (test)'):
        assert marker not in data, f'{arg}: hook marker {marker!r} escaped into production variant'
if len(sys.argv) == 3:
    print('Ranked CPU hook absent from non-hook parser and game adapter')
