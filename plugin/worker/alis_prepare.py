# Keep the signed macOS application immutable at runtime.
import sys
sys.dont_write_bytecode = True

from qal_ml.preparation import main
if __name__ == '__main__':
    raise SystemExit(main())
