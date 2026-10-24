import os
import subprocess
import sys

env=dict(os.environ,SDL_AUDIODRIVER="dummy")
for mode in ("--strict-force","--strict-bridge"):
    result=subprocess.run([sys.argv[1],mode],env=env,capture_output=True,text=True,timeout=30)
    assert result.returncode not in (0,7), (mode,result.returncode,result.stderr)
    assert "STRICT_STATIC" in result.stderr and "interpreter execution" in result.stderr, (mode,result.stderr)
print("Strict mode rejects explicit forced and bridge interpreter execution")
