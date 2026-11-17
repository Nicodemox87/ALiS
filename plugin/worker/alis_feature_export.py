# SPDX-License-Identifier: GPL-2.0-or-later
import sys
sys.dont_write_bytecode = True
from qal_ml.disk_features import main
if __name__ == '__main__':
    raise SystemExit(main())
