"""Reproducible development comparison using one shared spatial holdout."""
from __future__ import annotations
import argparse
import csv
import gc
import hashlib
import html
import json
import shutil
from pathlib import Path
from datetime import datetime, timezone
import numpy as np

from .classifiers import capability_report
from .dataset import load_dataset
from .events import EventWriter
from .model import TrainConfig, train, predict, load_artifact, _artifact_fingerprint
from .repository import ensure_repository


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(8*1024*1024),b''): result.update(data)
    return result.hexdigest()


def export_prediction_las(source: Path, destination: Path, prediction_dir: Path, expected_sha: str):
    """Preserve all bytes except Classification on a NEW legacy LAS copy.

    This also avoids rewriting the original file's duplicate Extra Bytes names.
    Confidence and model provenance remain in the accompanying prediction bundle.
    """
    import laspy
    if destination.exists(): raise FileExistsError(destination)
    if sha256(source) != expected_sha: raise ValueError('Source LAS SHA-256 changed')
    with laspy.open(source) as reader:
        header = reader.header
        if header.are_points_compressed or header.point_format.id not in range(6):
            raise ValueError('Prediction LAS export currently supports legacy uncompressed LAS only')
        n, offset, stride = header.point_count, header.offset_to_point_data, header.point_format.size
    values = np.memmap(prediction_dir/'predictions.i16',mode='r',dtype='<i2',shape=(n,))
    if np.any(values > 31) or np.any(values < -1): raise ValueError('Prediction class does not fit legacy LAS')
    shutil.copyfile(source,destination)
    flags = np.memmap(destination,mode='r+',dtype=np.dtype({'names':['classification'],'formats':['u1'],
        'offsets':[15],'itemsize':stride}),offset=offset,shape=(n,))
    counts=np.zeros(32,dtype=np.int64)
    for start in range(0,n,500_000):
        stop=min(n,start+500_000)
        codes=np.where(values[start:stop]>=0,values[start:stop],1).astype('u1')
        counts+=np.bincount(codes,minlength=32)
        flags['classification'][start:stop]=(flags['classification'][start:stop] & 224) | codes
    flags.flush(); del flags, values
    if sha256(source) != expected_sha: raise RuntimeError('Source modified unexpectedly')
    return {'path':str(destination.resolve()),'points':n,'sha256':sha256(destination),
        'predicted_class_counts':{str(i):int(v) for i,v in enumerate(counts) if v},
        'source_sha256':expected_sha,'modified_dimension':'Classification only; original classification stays in source_training.las',
        'confidence_bundle':str(prediction_dir.resolve())}


def summary_files(root: Path, results: list[dict], info: dict):
    info['warnings'] = sorted({message for result in results for message in result.get('warnings', [])})
    hag_missing=info.get('cloud',{}).get('automatic_hag',{}).get('nodata_count',0)
    points=info.get('cloud',{}).get('source_point_count',0)
    if points and hag_missing:
        info['warnings'].append(f'HAG automatico mancante sul {100*hag_missing/points:.1f}% dei punti: non considerare validata la ricostruzione del terreno.')
    predicted_noise=info.get('prediction_las',{}).get('predicted_class_counts',{}).get('7',0)
    reference_noise=info.get('cloud',{}).get('class_counts',{}).get('7',0)
    if predicted_noise>reference_noise:
        info['warnings'].append(f'Classe 7 nella predizione completa: {predicted_noise:,} punti contro {reference_noise:,} nel riferimento. Non eliminare automaticamente questi punti: la classe rumore non è validata.')
    info['macro_metric_scope']='Only reference classes represented in this holdout; absent classes are NOT validated'
    columns=['classifier','device','balanced_accuracy','accuracy','macro_f1','mean_iou','training_seconds','evaluation_seconds','train_rows','test_rows']
    with (root/'comparison.csv').open('w',newline='',encoding='utf-8') as stream:
        writer=csv.DictWriter(stream,fieldnames=columns,extrasaction='ignore');writer.writeheader();writer.writerows(results)
    (root/'comparison.json').write_text(json.dumps({'schema':'qal-comparison/1.0','results':results,**info},indent=2,allow_nan=False),encoding='utf-8')
    rows=''.join(f'<tr><td><a href="models/{html.escape(r["classifier"])}/report.html">{html.escape(r["classifier"])}</a></td>'
        f'<td>{r["device"]}</td><td>{r["balanced_accuracy"]:.4f}</td><td>{r["macro_f1"]:.4f}</td><td>{r["mean_iou"]:.4f}</td>'
        f'<td>{r["training_seconds"]:.1f} s</td><td>{r["train_rows"]:,}</td><td>{r["test_rows"]:,}</td></tr>' for r in results)
    chart_height = max(220, 70 + 50*len(results))
    chart=[f'<svg xmlns="http://www.w3.org/2000/svg" width="840" height="{chart_height}" viewBox="0 0 840 {chart_height}"><rect width="840" height="{chart_height}" fill="white"/>',
        '<style>text{font:14px sans-serif}</style><text x="20" y="25">Balanced accuracy — stesso holdout spaziale</text>']
    for i,r in enumerate(results):
        y=45+i*50
        chart.append(f'<text x="15" y="{y+20}">{html.escape(r["classifier"])}</text><rect x="220" y="{y}" width="{500*r["balanced_accuracy"]:.1f}" height="30" fill="#2563eb"/><text x="730" y="{y+20}">{r["balanced_accuracy"]:.3f}</text>')
    chart.append('</svg>');(root/'comparison.svg').write_text(''.join(chart),encoding='utf-8')
    doc=f'''<!doctype html><html lang="it"><meta charset="utf-8"><title>ALiS — confronto classificatori</title>
<style>body{{font:15px system-ui;max-width:1150px;margin:40px auto;padding:0 20px;color:#172033}}table{{border-collapse:collapse;width:100%}}td,th{{border:1px solid #cbd5e1;padding:10px;text-align:left}}pre{{white-space:pre-wrap;background:#f1f5f9;padding:16px}}img{{max-width:100%}}.note{{background:#fff7ed;padding:16px}}</style>
<h1>Confronto classificatori — Piazza Armerina</h1><p class="note">Valutazione di sviluppo su un solo rilievo, non validazione indipendente. Stessi ID train/test per ogni modello, fascia spaziale di rispetto, campionamento soltanto del training. Le classi di riferimento non sono feature né alimentano il DTM. La selezione di un modello su questo holdout richiede poi un nuovo test indipendente.</p>
<div class="note"><b>Limiti da leggere:</b><ul>{''.join('<li>'+html.escape(w)+'</li>' for w in info['warnings'])}</ul>F1/IoU macro e balanced accuracy riguardano le sole classi presenti nel riferimento di test.</div>
<p><a href="comparison.csv">Tabella CSV</a> · <a href="comparison.json">Protocollo JSON</a>{' · <a href="verification.json">Verifica integrità</a>' if info.get('verification_file') else ''}{' · <a href="predicted_reclaPzArmerina.las">LAS classificato (tutti i punti, originale preservato)</a>' if info.get('prediction_las') else ''}</p>
<table><tr><th>Classificatore / report</th><th>Device</th><th>Balanced accuracy</th><th>Macro F1</th><th>Mean IoU</th><th>Training</th><th>Punti train</th><th>Punti test</th></tr>{rows}</table>
<img src="comparison.svg" alt="Balanced accuracy dei classificatori"><h2>Blocchi spaziali comuni</h2><img src="spatial_split.svg" alt="Blocchi training blu e holdout arancioni"><h2>Protocollo e cloud</h2><pre>{html.escape(json.dumps(info,indent=2,ensure_ascii=False))}</pre>
<p>Nei report dei modelli: precision, recall, F1, IoU e supporto per classe, confusion matrix, importanza delle feature e parametri. Le classi 3/5 e le classi archeologiche non sono presenti nel training e non vengono apprese.</p></html>'''
    (root/'comparison.html').write_text(doc,encoding='utf-8')


def split_map(dataset, model: Path, root: Path, block_size: float):
    train_ids=np.load(str(model)+'.train_point_ids.npy')
    test_ids=np.load(str(model)+'.test_point_ids.npy')
    origin=np.min(dataset.xy,axis=0)
    maximum=np.max(dataset.xy,axis=0)
    # The prepared native dataset has exact sequential source-row IDs.
    if dataset.point_ids is None or not np.array_equal(dataset.point_ids,np.arange(dataset.point_count,dtype='<u8')):
        raise ValueError('Comparison map requires sequential source-row IDs')
    train_cells=np.unique(np.floor((dataset.xy[train_ids]-origin)/block_size).astype(int),axis=0)
    test_cells=np.unique(np.floor((dataset.xy[test_ids]-origin)/block_size).astype(int),axis=0)
    scale=min(680/(maximum[0]-origin[0]+block_size),530/(maximum[1]-origin[1]+block_size))
    parts=['<svg xmlns="http://www.w3.org/2000/svg" width="820" height="680" viewBox="0 0 820 680"><rect width="820" height="680" fill="white"/>',
        '<style>text{font:13px sans-serif}</style><text x="65" y="25">Blocchi XY 20 m: training blu / test arancione</text>']
    for cells,color in ((train_cells,'#93c5fd'),(test_cells,'#fdba74')):
        for col,row in cells:
            x=65+col*block_size*scale; y=600-(row+1)*block_size*scale
            parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{block_size*scale:.2f}" height="{block_size*scale:.2f}" fill="{color}" stroke="white"/>')
    for col in range(int(np.ceil((maximum[0]-origin[0])/block_size))+1):
        parts.append(f'<text x="{65+col*block_size*scale:.2f}" y="620" text-anchor="middle">{origin[0]+col*block_size:.0f}</text>')
    for row in range(int(np.ceil((maximum[1]-origin[1])/block_size))+1):
        parts.append(f'<text x="60" y="{600-row*block_size*scale:.2f}" text-anchor="end">{origin[1]+row*block_size:.0f}</text>')
    parts.append('<text x="65" y="660">Coordinate native in metri; CRS non dichiarato. Training escluso entro la fascia dai blocchi test.</text></svg>')
    (root/'spatial_split.svg').write_text(''.join(parts),encoding='utf-8')


def compare(dataset_path: Path, root: Path, *, trees=200, cap=75_000, jobs=12, predict_best=True, resume=False):
    previous=json.loads((root/'comparison.json').read_text(encoding='utf-8')) if (root/'comparison.json').exists() else {}
    if not resume and (previous or any((root/'models'/name/'model.joblib').exists() for name in ('random_forest','extra_trees','xgboost'))):
        raise FileExistsError('Use a new comparison repository; refusing to overwrite models/results')
    ensure_repository(root)
    dataset=load_dataset(dataset_path)
    radii=dataset.metadata.get('feature_processing',{}).get('radii_m',[])
    if not radii: raise ValueError('Native feature radii are required for the spatial guard')
    buffer=2*max(radii)+.001
    caps=capability_report()
    gpu=next(item for item in caps['classifiers'] if item['id']=='xgboost')
    if not gpu['available'] or 'cuda' not in gpu['devices']: raise RuntimeError('XGBoost CUDA unavailable; no silent CPU fallback')
    info={'created_utc':datetime.now(timezone.utc).isoformat(),'dataset':str(dataset_path.resolve()),
        'dataset_sha256':dataset.source_sha256,'cloud':dataset.metadata,'capabilities':caps,
        'protocol':{'seed':42,'block_size_m':20.,'test_fraction':.2,'buffer_m':buffer,
            'max_train_per_class':cap,'trees':trees,'test_subsampled':False,
            'imputation':'medians and missingness indicators fitted only on sampled training rows',
            'features':'native geometric multiscale + automatic CSF HAG; no class, ID, XY or absolute Z predictor'}}
    results=[];reference=None
    for name,device in [('random_forest','cpu'),('extra_trees','cpu'),('xgboost','cuda')]:
        print(json.dumps({'phase':'classifier','classifier':name,'device':device}),flush=True)
        model=root/'models'/name/'model.joblib'
        config=TrainConfig(classifier=name,device=device,trees=trees,n_jobs=jobs,
            max_depth=8 if name=='xgboost' else 24,min_samples_leaf=3,class_weight='balanced',
            learning_rate=.05,block_size=20.,test_fraction=.2,seed=42,repository=str(root),
            max_train_per_class=cap,spatial_buffer=buffer,impute_missing=True)
        if resume and model.exists():
            artifact=load_artifact(model)
            if artifact['artifact_id'] != _artifact_fingerprint(dataset,config):
                raise ValueError('Resume model configuration/dataset mismatch')
            saved_report=json.loads((model.parent/'report.json').read_text(encoding='utf-8'))
            if saved_report['model_sha256'] != sha256(model): raise ValueError('Resume model hash mismatch')
            for part in ('train','test'):
                ids=np.asarray(np.load(str(model)+f'.{part}_point_ids.npy'),dtype='<u8')
                if hashlib.sha256(ids.tobytes()).hexdigest()!=artifact['training']['split'][part+'_point_ids_sha256']:
                    raise ValueError('Resume point membership hash mismatch')
            print(json.dumps({'phase':'reuse-completed-model','classifier':name}),flush=True)
        else:
            artifact=train(dataset,model,config,EventWriter('compare'))
        split=artifact['training']['split'];metric=artifact['training']['metrics']
        membership=(split['train_point_ids_sha256'],split['test_point_ids_sha256'])
        if reference is not None and membership!=reference: raise AssertionError('Models did not use identical training/test points')
        reference=membership
        if not results: split_map(dataset,model,root,20.)
        reloaded=load_artifact(model)
        # Smoke-test serialization against the in-memory estimator, not training accuracy.
        sample=np.asarray(dataset.features[:1000],dtype=np.float32)
        sample=np.where(np.isfinite(sample),sample,np.nan)
        np.testing.assert_allclose(artifact['estimator'].predict_proba(artifact['preprocessor'].transform(sample)),
            reloaded['estimator'].predict_proba(reloaded['preprocessor'].transform(sample)),rtol=1e-6,atol=1e-7)
        results.append({'classifier':name,'device':device,'balanced_accuracy':metric['balanced_accuracy'],
            'accuracy':metric['accuracy'],'macro_f1':float(np.mean([r['f1'] for r in metric['per_class'] if r['support']>0])),
            'mean_iou':float(np.mean([r['iou'] for r in metric['per_class'] if r['support']>0])),
            'evaluated_classes':[r['class'] for r in metric['per_class'] if r['support']>0],
            'training_seconds':artifact['training']['training_seconds'],'evaluation_seconds':artifact['training']['evaluation_seconds'],
            'train_rows':split['train_rows'],'test_rows':split['test_rows'],'per_class':metric['per_class'],
            'split':split,'validation_complete':metric['validation_complete'],'warnings':metric['warnings'],
            'reload_prediction_verified':True,'model':str(model.resolve())})
        summary_files(root,results,info)
        del artifact,reloaded;gc.collect()
    info['identical_membership_verified']=True
    if predict_best:
        best=max(results,key=lambda item:item['balanced_accuracy'])
        bundle=root/'runs'/'best_full_prediction'
        if resume and (bundle/'result.json').exists():
            prediction_info=json.loads((bundle/'result.json').read_text(encoding='utf-8'))
            provenance=prediction_info['provenance']
            if provenance['dataset_sha256']!=dataset.source_sha256 or provenance['model_sha256']!=sha256(best['model']):
                raise ValueError('Resume prediction identity mismatch')
        else:
            predict(dataset,best['model'],bundle,events=EventWriter('predict-best'))
        info['best_on_this_holdout_only']=best['classifier']
        las_output=root/'predicted_reclaPzArmerina.las'
        if resume and las_output.exists():
            if sha256(las_output)!=previous.get('prediction_las',{}).get('sha256'): raise ValueError('Resume LAS identity mismatch')
            info['prediction_las']=previous['prediction_las']
        else:
            info['prediction_las']=export_prediction_las(dataset_path/'source_training.las',las_output,bundle,dataset.metadata['source_sha256'])
    summary_files(root,results,info)
    print(json.dumps({'phase':'complete','report':str((root/'comparison.html').resolve()),'results':results}),flush=True)
    return results


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset',type=Path,required=True);parser.add_argument('--repository',type=Path,required=True)
    parser.add_argument('--trees',type=int,default=200);parser.add_argument('--max-train-per-class',type=int,default=75_000)
    parser.add_argument('--jobs',type=int,default=12);parser.add_argument('--skip-full-prediction',action='store_true')
    parser.add_argument('--resume',action='store_true',help='Reuse completed artifacts only after verifying hashes and configuration')
    args=parser.parse_args()
    compare(args.dataset,args.repository,trees=args.trees,cap=args.max_train_per_class,jobs=args.jobs,predict_best=not args.skip_full_prediction,resume=args.resume)
