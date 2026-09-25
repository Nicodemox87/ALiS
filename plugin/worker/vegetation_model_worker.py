"""CLI adapter for explicit, verified local vegetation model inference."""
import argparse
import sys
from pathlib import Path
from qal_ml.events import EventWriter
from qal_ml.vegetation import predict_verified

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset',type=Path,required=True)
    p.add_argument('--model',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--trust-local-model',action='store_true')
    args=p.parse_args(); events=EventWriter('vegetation-predict')
    try:
        predict_verified(args.dataset,args.model,args.output,events,trust_model=args.trust_local_model)
        return 0
    except Exception as error:
        events.emit('error','failed',0,str(error)); print(str(error),file=sys.stderr); return 1

if __name__=='__main__':
    raise SystemExit(main())
