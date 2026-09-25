"""Versioned research-model inference, without changing ASPRS or trusting outputs."""
from pathlib import Path
import hashlib
import json
import time

import joblib
import numpy as np

from .dataset import load_dataset, SchemaError

RAW_FEATURES = ('neighbor_count', 'planarity', 'sphericity', 'surface_variation',
    'linearity', 'normal_z_absolute', 'barycenter_offset_ratio', 'roughness',
    'z_range', 'z_standard_deviation', 'eigenvalues_sum', 'pca_1', 'pca_2')
TRANSFORM_SCHEMA = 'alis-vegetation-normalized-geometry/1'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda:f.read(8*1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def geometry_transform(matrix, feature_names, radii):
    if len(radii)!=2 or not (0 < radii[0] < radii[1] <= 3):
        raise SchemaError('Expected two ordered physical radii, maximum 3 m')
    keys = {}
    for i,key in enumerate(feature_names):
        try:
            name,radius=key.rsplit('@',1)
            normalized=(name,round(float(radius),12))
        except ValueError:
            continue
        if normalized in keys:
            raise SchemaError('Ambiguous duplicate geometry columns')
        keys[normalized]=i
    def get(name,r):
        key=(name,round(float(r),12))
        if key not in keys:
            raise SchemaError(f'Missing exact trained feature: {name}@{r:g}; radii cannot be silently changed')
        return matrix[:,keys[key]]
    columns=[]; names=[]
    for r in radii:
        for f in ('planarity','sphericity','surface_variation','linearity','normal_z_absolute','barycenter_offset_ratio'):
            columns.append(get(f,r)); names.append(f'{f}@{r:g}')
        for f in ('roughness','z_range','z_standard_deviation'):
            columns.append(get(f,r)/r); names.append(f'{f}_over_radius@{r:g}')
        columns.append(np.sqrt(np.maximum(0,get('surface_variation',r)*get('eigenvalues_sum',r)))/r)
        names.append(f'plane_rms_over_radius@{r:g}')
        eig=np.column_stack([get('pca_1',r),get('pca_2',r),get('surface_variation',r)])
        columns.append(-np.sum(eig*np.log(np.maximum(eig,1e-12)),axis=1)/np.log(3))
        names.append(f'normalized_eigenentropy@{r:g}')
    a,b=radii; na,nb=get('neighbor_count',a),get('neighbor_count',b)
    columns.append(np.log(np.maximum(nb,1)/np.maximum(na,1))/np.log(b/a)); names.append('support_growth_dimension')
    for f in ('planarity','sphericity','surface_variation','normal_z_absolute'):
        columns.append(get(f,b)-get(f,a)); names.append(f+'_coarse_minus_fine')
    x=np.column_stack(columns).astype(np.float32)
    x[~np.isfinite(x)]=np.nan
    return x,names,(na>=12)&(nb>=12)


def predict_verified(dataset_path,model_path,output,events,*,trust_model=False):
    if not trust_model:
        raise SchemaError('Joblib may execute code. Explicit trusted-local-model authorization required')
    model_path=Path(model_path); output=Path(output)
    manifest=json.loads(Path(str(model_path)+'.json').read_text(encoding='utf-8'))
    if manifest.get('schema')!='alis-vegetation-local-model/1' or manifest.get('transform')!=TRANSFORM_SCHEMA:
        raise SchemaError('Unsupported vegetation model manifest/transform')
    events.emit('progress','verify',.01,'Verifying local model hash and fixed training scales')
    if digest(model_path)!=manifest.get('model_sha256'):
        raise SchemaError('Model hash mismatch; no model code was loaded')
    artifact=joblib.load(model_path)
    if artifact.get('schema')!='alis-vegetation-research/1':
        raise SchemaError('Not a supported binary vegetation research model')
    radii=artifact['radii_m']
    if radii!=manifest['radii_m'] or artifact['feature_names']!=manifest['feature_names']:
        raise SchemaError('Manifest does not match trained schema')
    estimator=artifact['estimator']; imputer=artifact['imputer']
    if not np.array_equal(estimator.classes_,[False,True]):
        raise SchemaError('Expected binary native target: retained / vegetation')
    estimator.n_jobs=min(12,getattr(estimator,'n_jobs',12) or 12) if getattr(estimator,'n_jobs',12)!=-1 else 12
    events.emit('progress','dataset',.03,'Reading cloud features; labels are not predictors')
    ds=load_dataset(dataset_path)
    # Check compatibility before creating any output. Extra non-predictor fields
    # are allowed, but the transform only reads its exact whitelisted inputs.
    _,names,_=geometry_transform(ds.features[:1],ds.feature_names,radii)
    if artifact['feature_names']!=names:
        raise SchemaError('This adapter requires the complete version-1 multiscale schema')
    output.mkdir(parents=True,exist_ok=False)
    score=np.memmap(output/'score.f32',dtype='<f4',mode='w+',shape=(ds.point_count,))
    score[:]=np.nan
    started=time.monotonic(); unsupported=excluded=predicted=0
    for start in range(0,ds.point_count,100000):
        stop=min(ds.point_count,start+100000)
        x,_,support=geometry_transform(ds.features[start:stop],ds.feature_names,radii)
        labels=ds.labels[start:stop] if ds.labels is not None else np.ones(stop-start,dtype=np.uint8)
        protected=np.isin(labels,[2,7,9,18,22])
        valid=support & ~protected
        unsupported+=int(np.sum(~support & ~protected)); excluded+=int(protected.sum())
        if valid.any():
            score[start:stop][valid]=estimator.predict_proba(imputer.transform(x[valid]))[:,1]
            predicted+=int(valid.sum())
        events.emit('progress','predict',.05+.9*stop/ds.point_count,f'Vegetation model [CPU]: {stop:,} / {ds.point_count:,} points')
    score.flush()
    threshold=float(artifact['threshold'])
    report={'schema':'alis-vegetation-score/1','point_count':ds.point_count,'model_path':str(model_path.resolve()),
        'model_sha256':manifest['model_sha256'],'model_name':manifest.get('name',model_path.stem),
        'model_threshold':threshold,'radii_metres':radii,'minimum_neighbors':12,
        'source_dataset_sha256':ds.source_sha256,'predicted_points':predicted,
        'unsupported_non_ground':unsupported,'excluded_points':excluded,
        'proposed_vegetation':int(np.sum(score>=threshold)), 'elapsed_seconds':time.monotonic()-started,
        'validation':manifest.get('validation',{}),'training_cloud_profile':manifest.get('cloud_profile',{}),
        'current_cloud_profile':ds.metadata,'transform':TRANSFORM_SCHEMA,
        'device':'cpu','certified':False,'working_classes_modified':False,
        'feature_keys':list(ds.feature_names),
        'score_is_calibrated_probability':False,'ground_protected':True,
        'limitations':'Single-survey experimental model; aggregate metrics do not establish local or cross-site reliability. Rejected proposals are retained/uncertain, NOT Building.'}
    (output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False),encoding='utf-8')
    events.emit('completed','done',1.,'Vegetation scores ready; threshold can be changed without another prediction',report)
    return report
