"""Publish a readiness marker only after real GPU computation and dependency checks."""
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
import torch

assert sys.prefix != sys.base_prefix, 'Use the isolated ALiS virtual environment'
assert torch.cuda.is_available(), 'CUDA not available'
x = torch.ones((128, 128), device='cuda')
assert (x @ x)[0, 0].item() == 128
subprocess.run([sys.executable, '-m', 'pip', 'check'], check=True)
result = {'schema': 'alis-runtime/1', 'python': sys.executable,
          'torch': torch.__version__, 'device': torch.cuda.get_device_name(0),
          'verified_utc': datetime.now(timezone.utc).isoformat()}
Path(sys.prefix, 'alis-runtime-ready.json').write_text(json.dumps(result, indent=2), encoding='utf8')
print(json.dumps(result))
