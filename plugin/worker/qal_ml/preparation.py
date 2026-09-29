"""Bounded spatial preparation for saved models, for LAS and live-CC snapshots.

One immutable input scan; indexed tiles on disk; context halo never exported twice.
No training or automatic label promotion. Existing feature reuse is explicit.
"""
from __future__ import annotations
import argparse, ctypes, hashlib, html, json, math, os, shutil, subprocess, sys, time
from pathlib import Path
from urllib.parse import unquote
import numpy as np

GEOMETRY = ('neighbor_count density_2d density_3d eigenvalue_1 eigenvalue_2 eigenvalue_3 eigenvalues_sum pca_1 pca_2 linearity planarity sphericity anisotropy omnivariance eigenentropy surface_variation verticality normal_x normal_y normal_z normal_z_absolute dip dip_direction roughness signed_roughness mean_curvature gaussian_curvature normal_change_rate moment_order_1 barycenter_offset_ratio z_minimum z_maximum z_range z_mean z_standard_deviation z_above_minimum z_below_maximum z_relative_to_mean z_percentile_10 z_percentile_25 z_median z_percentile_75 z_percentile_90').split()
CHUNK=100_000

def save(path, obj):
    path=Path(path); path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(obj,indent=2,allow_nan=False),encoding='utf8');os.replace(temp,path)
def read(path): return json.loads(Path(path).read_text(encoding='utf8'))
def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(8*1024**2),b''): h.update(b)
    return h.hexdigest()
def emit(phase,done,total,message):
    print(json.dumps(dict(phase=phase,done=done,total=total,message=message)),flush=True)
def available_memory():
    if os.name=='nt':
        class Mem(ctypes.Structure): _fields_=[('length',ctypes.c_ulong),('load',ctypes.c_ulong)]+[(x,ctypes.c_ulonglong) for x in ('total','avail','tp','ap','tv','av','ex')]
        m=Mem();m.length=ctypes.sizeof(m);ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m));return m.avail
    return os.sysconf('SC_AVPHYS_PAGES')*os.sysconf('SC_PAGE_SIZE')
def canceled(job):
    if (job/'CANCEL').exists(): raise InterruptedError('Stopped safely; completed blocks remain reusable. Remove CANCEL to resume.')

def requirements(model):
    from .model import load_artifact
    a=load_artifact(model)  # GUI explicitly asks the user to trust executable model files.
    names=list(a['feature_schema']['names']);meta=a.get('provenance',{}).get('dataset_metadata',{})
    result={'schema':'alis-preparation-requirements/1','model':str(Path(model).resolve()),'model_sha256':digest(model),
            'features':names,'classifier_id':a.get('classifier_id'),'target_domain':a['target_domain'],
            'ground_recipe':meta.get('automatic_ground',{}),'terrain_recipe':meta.get('automatic_terrain',{}),
            'pointnet_radius':float(getattr(a['estimator'],'radius',.5)) if a.get('classifier_id')=='pointnet' else 0.}
    return result

def feature_radius(key):
    parts=key.split('@')
    if len(parts)!=2 or parts[0] not in GEOMETRY: raise ValueError('Unsupported feature: '+key)
    r=float(parts[1])
    if not math.isfinite(r) or r<=0: raise ValueError('Invalid feature radius: '+key)
    return r

class Source:
    def __init__(self,path,features,reuse):
        self.path=Path(path);self.features=features;self.reuse=reuse;self.columns={};self.kind='snapshot' if self.path.is_dir() else 'las'
        if self.kind=='snapshot':
            self.meta=read(self.path/'snapshot.json');self.n=int(self.meta['point_count']);self.origin=self.meta['origin']
            self.attrs=self.meta['columns'];self.dtype=np.dtype([('xyz','<f8',(3,)),('attrs','<f4',(len(self.attrs),))])
            if (self.path/'points.bin').stat().st_size!=self.n*self.dtype.itemsize: raise ValueError('Incomplete CloudCompare snapshot')
            self.fingerprint=digest(self.path/'points.bin')+digest(self.path/'snapshot.json')
            for key in features:
                if key in self.attrs and (reuse or key=='hag@0' or key.startswith(('sf:','rgb:'))):self.columns[key]=self.attrs.index(key)
        else:
            import laspy
            with laspy.open(self.path) as f:
                self.n=f.header.point_count;self.origin=f.header.mins.tolist();self.attrs=list(f.header.point_format.dimension_names)
                self.header=f.header.copy()
                if f.header.point_format.id in (4,5,9,10):raise ValueError('Waveform LAS needs a waveform-aware exporter; this path will not silently discard waveform payloads.')
            self.fingerprint=digest(self.path)
            for key in features:
                field=unquote(key[3:]) if key.startswith('sf:') else ('qAL_HAG' if key=='hag@0' else key)
                if field in self.attrs and (reuse or key=='hag@0' or key.startswith('sf:')):self.columns[key]=field
                if key.startswith('rgb:'):raise ValueError('LAS RGB scaling is not implied: use a CloudCompare snapshot with its explicit 0-255 channels.')
        if not self.n:raise ValueError('Empty source')
        for k in features:
            if k.startswith('sf:') and k not in self.columns:raise ValueError('Required scalar field missing: '+unquote(k[3:]))
    def batches(self):
        if self.kind=='snapshot':
            raw=np.memmap(self.path/'points.bin',dtype=self.dtype,mode='r',shape=(self.n,))
            for start in range(0,self.n,CHUNK):
                rows=raw[start:start+CHUNK];yield start,np.asarray(rows['xyz']),{k:rows['attrs'][:,v] for k,v in self.columns.items()}
        else:
            import laspy
            start=0
            with laspy.open(self.path) as f:
                for rows in f.chunk_iterator(CHUNK):
                    yield start,np.column_stack((rows.x,rows.y,rows.z)),{k:np.asarray(rows[v],dtype='f4') for k,v in self.columns.items()};start+=len(rows)

def plan(config):
    if not config.get('metric_units'):raise ValueError('Confirm metric coordinate units before preparing radius-based features.')
    req=requirements(config['model']);src=Source(config['source'],req['features'],config.get('reuse_features',False))
    if req['target_domain']!='asprs_working':raise ValueError('Blocked LAS classification currently requires an ASPRS model; archaeology-only codes are not silently mapped to ASPRS.')
    neural=req['classifier_id']=='pointnet';missing=[k for k in req['features'] if k not in src.columns]
    radii=[feature_radius(k) for k in missing if k!='hag@0'] if not neural else [req['pointnet_radius']]
    halo=max(radii or [0.])+0.002  # Small numerical margin, never shrinks model radii.
    hag='hag@0' in missing
    if hag:
        if not config.get('allow_tiled_hag'):raise ValueError('HAG is missing. Supply HAG, or explicitly approve the displayed tiled CSF/HAG recipe (edge validation required).')
        req['ground_recipe']=config.get('ground_recipe') or req['ground_recipe']
        req['terrain_recipe']=config.get('terrain_recipe') or req['terrain_recipe']
        if not req['ground_recipe'] or not req['terrain_recipe']:raise ValueError('Legacy model lacks terrain provenance: choose and record a terrain recipe first.')
        for key in ('clothResolution','classificationThreshold','rigidness','iterations','timeStep','smoothSlope'):
            if key not in req['ground_recipe']:raise ValueError('Incomplete saved CSF recipe: missing '+key)
        for key in ('gridStep','interpolateEmptyCells','maximumInterpolationEdgeLength'):
            if key not in req['terrain_recipe']:raise ValueError('Incomplete saved terrain recipe: missing '+key+'. Supply existing HAG from CC or an explicit recipe in config.json.')
        halo=max(halo,float(config.get('terrain_halo_m',20.)))
        if halo<max(2*req['ground_recipe']['clothResolution'],req['terrain_recipe'].get('maximumInterpolationEdgeLength',0)):
            raise ValueError('Terrain buffer is smaller than cloth/interpolation support.')
    size=float(config.get('block_m',50.));budget=float(config.get('memory_gib',2.))*1024**3
    if not math.isfinite(size) or size<1 or size>10000:raise ValueError('Block size must be 1..10000 m')
    if not math.isfinite(budget) or budget<512*1024**2:raise ValueError('Memory budget must be at least 0.5 GiB')
    safe=min(budget,available_memory()*.45);bytes_per_point=256+len(req['features'])*16
    limit=min(1_000_000,int((safe-384*1024**2)/bytes_per_point))
    if limit<1000:raise MemoryError('Not enough free RAM while CloudCompare is open. Free memory or reduce loaded clouds.')
    return src,dict(req,source_fingerprint=src.fingerprint,point_count=src.n,source=str(src.path.resolve()),source_kind=src.kind,
        origin=src.origin,block_m=size,halo_m=halo,memory_bytes=int(safe),max_context_points=limit,
        supplied=list(src.columns),needs_hag=hag,allow_tiled_hag=hag,threads=int(config.get('threads',4)),
        native=str(config['native']),native_sha256=digest(config['native']),
        warning='Tiled CSF/HAG is not guaranteed equivalent to global terrain. Validate seams.' if hag else 'Fixed-radius feature context includes all halo points; no decimation.')

def record_dtype(supplied):return np.dtype([('xyz','<f8',(3,)),('id','<u8'),('attrs','<f4',(len(supplied),))])

def require_disk(folder, amount):
    if shutil.disk_usage(folder).free < amount+512*1024**2:
        raise OSError('Insufficient disk space for this block / output. Completed blocks remain reusable.')

def block_checksums(folder):
    return {name:digest(folder/name) for name in ('features.f32','xy.f64','z.f64','point_ids.u64','core.u8','manifest.json')}

def verify_block(folder, entry):
    if block_checksums(folder)!=entry.get('checksums'):raise ValueError('Prepared block integrity mismatch: '+str(folder))

def index_source(src,p,job):
    index=job/'index';index.mkdir(exist_ok=True)
    if (index/'complete.json').exists():return read(index/'complete.json')['tiles']
    # An interrupted first scan is not a valid checkpoint; rebuild only owned spool files.
    for old in index.glob('tile_*.bin'):old.unlink()
    dtype=record_dtype(p['supplied']);counts={};size=p['block_m'];origin=np.asarray(p['origin'])
    for start,xyz,attrs in src.batches():
        canceled(job)
        require_disk(job,len(xyz)*dtype.itemsize)
        if not np.isfinite(xyz).all():raise ValueError('Nonfinite coordinates cannot be spatially partitioned; source remains unchanged.')
        cell=np.floor((xyz[:,:2]-origin[:2])/size).astype('i8')
        if np.abs(cell).max(initial=0)>1_000_000:raise ValueError('Spatial grid out of range')
        records=np.empty(len(xyz),dtype=dtype);records['xyz']=xyz;records['id']=np.arange(start,start+len(xyz),dtype='u8')
        for i,k in enumerate(p['supplied']):records['attrs'][:,i]=attrs[k]
        for xy in np.unique(cell,axis=0):
            key=f'{xy[0]}_{xy[1]}';chosen=np.all(cell==xy,axis=1);rows=records[chosen]
            with (index/f'tile_{key}.bin').open('ab') as f:rows.tofile(f)
            counts[key]=counts.get(key,0)+len(rows)
        emit('index',start+len(xyz),src.n,'Indexing source once; original point IDs preserved')
    tiles=[dict(key=k,count=v,bounds=[origin[0]+int(k.split('_')[0])*size,origin[1]+int(k.split('_')[1])*size,
                                     origin[0]+(int(k.split('_')[0])+1)*size,origin[1]+(int(k.split('_')[1])+1)*size]) for k,v in sorted(counts.items())]
    assert sum(t['count'] for t in tiles)==src.n
    save(index/'complete.json',{'tiles':tiles});return tiles

def context_batches(tiles,bounds,halo,job,dtype):
    x0,y0,x1,y1=bounds
    for tile in tiles:
        a,b,c,d=tile['bounds']
        if c<x0-halo or a>x1+halo or d<y0-halo or b>y1+halo:continue
        with (job/'index'/f'tile_{tile["key"]}.bin').open('rb') as f:
            while True:
                canceled(job);rows=np.fromfile(f,dtype=dtype,count=CHUNK)
                if not len(rows):break
                xy=rows['xyz'][:,:2];sel=(xy[:,0]>=x0-halo)&(xy[:,0]<=x1+halo)&(xy[:,1]>=y0-halo)&(xy[:,1]<=y1+halo)
                yield rows[sel]
def core_mask(rows,b):
    xy=rows['xyz'][:,:2];return (xy[:,0]>=b[0])&(xy[:,0]<b[2])&(xy[:,1]>=b[1])&(xy[:,1]<b[3])

def run_native(p,folder,job):
    env=os.environ.copy();host=str(Path(p['native']).parent);env['PATH']=host+os.pathsep+env.get('PATH','')
    # Native qCSF uses QApplication with the shipped minimal platform (no visible windows).
    with (folder/'native.log').open('w',encoding='utf8') as log, (folder/'native.log').open('r',encoding='utf8',errors='replace') as tail:
        proc=subprocess.Popen([p['native'],str(folder/'request.json')],stdout=log,stderr=log,env=env,
                              creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        try:
            while proc.poll() is None:
                canceled(job)
                for line in tail:
                    if line.startswith('ALIS_PROGRESS '):
                        try:emit('block feature progress',int(line.split()[1]),100,folder.name)
                        except (ValueError,IndexError):pass
                if available_memory()<256*1024**2:raise MemoryError('Free RAM safety reserve reached; block stopped before exhausting the PC.')
                time.sleep(.3)
            if proc.returncode:raise RuntimeError('Native block failed; see '+str(folder/'native.log'))
        finally:
            if proc.poll() is None:proc.kill();proc.wait()

def prepare(config,job):
    src,p=plan(config);job.mkdir(parents=True,exist_ok=True)
    if (job/'plan.json').exists():
        old=read(job/'plan.json')
        # Available RAM may vary between runs. Algorithm/input identity must not.
        for key in p:
            if key not in ('memory_bytes','max_context_points') and old.get(key)!=p[key]:raise ValueError('Job inputs/model/settings changed. Choose a new output folder.')
    save(job/'plan.json',p);tiles=index_source(src,p,job);dtype=record_dtype(p['supplied'])
    pending=[(t['key'],t['bounds']) for t in tiles];prepared=[];done=0
    while pending:
        canceled(job);key,b=pending.pop(0);folder=job/'blocks'/key
        if (folder/'ready.json').exists():
            entry=read(folder/'ready.json')
            verify_block(folder,entry)
            prepared.append(entry);done+=entry['core_count'];continue
        count=0;core=0
        for rows in context_batches(tiles,b,p['halo_m'],job,dtype):count+=len(rows);core+=int(core_mask(rows,b).sum())
        if not core:continue
        if count>p['max_context_points']:
            if min(b[2]-b[0],b[3]-b[1])<1:raise MemoryError('Halo alone exceeds memory budget. Increase memory or provide existing HAG; model radii cannot shrink.')
            xm=(b[0]+b[2])/2;ym=(b[1]+b[3])/2
            pending[0:0]=[(key+'a',[b[0],b[1],xm,ym]),(key+'b',[xm,b[1],b[2],ym]),(key+'c',[b[0],ym,xm,b[3]]),(key+'d',[xm,ym,b[2],b[3]])];continue
        require_disk(job,count*(65+4*len(p['supplied'])+4*len(p['features'])))
        folder.mkdir(parents=True,exist_ok=True);supplied={k:f'input_{i}.f32' for i,k in enumerate(p['supplied'])}
        handles={name:(folder/name).open('wb') for name in ['xyz.f64','xy.f64','z.f64','point_ids.u64','core.u8',*supplied.values()]}
        try:
            for rows in context_batches(tiles,b,p['halo_m'],job,dtype):
                rows['xyz'].tofile(handles['xyz.f64']);rows['xyz'][:,:2].tofile(handles['xy.f64']);rows['xyz'][:,2].tofile(handles['z.f64']);rows['id'].tofile(handles['point_ids.u64']);core_mask(rows,b).astype('u1').tofile(handles['core.u8'])
                for i,k in enumerate(p['supplied']):rows['attrs'][:,i].tofile(handles[supplied[k]])
        finally:
            for f in handles.values():f.close()
        request=dict(p,directory=str(folder.resolve()),point_count=count,supplied=supplied,cache_bytes=int(p['memory_bytes']*.4))
        save(folder/'request.json',request);emit('prepare',done,src.n,f'Block {key}: {core:,} core + {count-core:,} halo points')
        if p['classifier_id']=='pointnet':
            xyz=np.memmap(folder/'xyz.f64',dtype='f8',mode='r',shape=(count,3))
            with (folder/'features.f32').open('wb') as f:
                for start in range(0,count,CHUNK):(xyz[start:start+CHUNK]-p['origin']).astype('f4').tofile(f)
            del xyz
        else:run_native(p,folder,job)
        save(folder/'manifest.json',dict(schema='qal-ml-dataset/1.0',point_count=count,feature_count=len(p['features']),feature_names=p['features'],
                                        target_domain=p['target_domain'],labels_trusted=False,metadata={'preparation_plan':str(job/'plan.json'),'tiled':True}))
        entry=dict(key=key,bounds=b,core_count=core,context_count=count,features_sha256=digest(folder/'features.f32'))
        entry['checksums']=block_checksums(folder)
        save(folder/'ready.json',entry);prepared.append(entry);done+=core
        save(job/'status.json',dict(stage='preparing',points=done,total=src.n,blocks=len(prepared)))
    if done!=src.n:raise RuntimeError('Core ownership mismatch; refusing ready state')
    save(job/'prepared.json',dict(plan_sha256=digest(job/'plan.json'),blocks=prepared,point_count=done))
    save(job/'status.json',dict(stage='ready',points=done,total=done));emit('ready',done,done,'Data ready. Classify prepared blocks when required.')

def classify(job):
    from .dataset import load_dataset
    from .model import predict
    p=read(job/'plan.json');prepared=read(job/'prepared.json')
    if digest(p['model'])!=p['model_sha256']:raise ValueError('Model changed since preparation')
    if digest(job/'plan.json')!=prepared['plan_sha256']:raise ValueError('Preparation plan changed')
    n=p['point_count'];out=job/'result';out.mkdir(exist_ok=True)
    require_disk(job,n*6)
    save(job/'status.json',dict(stage='predicting',points=0,total=n))
    pred=np.memmap(out/'predictions.i16',dtype='<i2',mode='w+',shape=(n,));conf=np.memmap(out/'confidence.f32',dtype='<f4',mode='w+',shape=(n,));pred[:]=-1;conf[:]=np.nan
    done=0
    for block in prepared['blocks']:
        canceled(job);folder=job/'blocks'/block['key'];result=folder/'prediction'
        verify_block(folder,block)
        dataset=load_dataset(folder)
        if not (result/'result.json').exists():
            if result.exists() and any(result.iterdir()):
                backup=folder/('incomplete_prediction_'+str(time.time_ns()));result.rename(backup)
            emit('predict',done,n,'Classifying block '+block['key']);predict(dataset,p['model'],result,chunk_size=50000)
        resultmeta=read(result/'result.json')
        if resultmeta['provenance']['model_sha256']!=p['model_sha256']:raise ValueError('Cached prediction model mismatch')
        if resultmeta['provenance']['dataset_sha256']!=dataset.source_sha256:raise ValueError('Cached prediction dataset mismatch')
        size=block['context_count'];mask=np.memmap(folder/'core.u8',dtype='u1',mode='r',shape=(size,));ids=np.memmap(folder/'point_ids.u64',dtype='<u8',mode='r',shape=(size,))
        pp=np.memmap(result/'predictions.i16',dtype='<i2',mode='r',shape=(size,));cc=np.memmap(result/'confidence.f32',dtype='<f4',mode='r',shape=(size,))
        for start in range(0,size,CHUNK):
            sel=mask[start:start+CHUNK].astype(bool);target=ids[start:start+CHUNK][sel];pred[target]=pp[start:start+CHUNK][sel];conf[target]=cc[start:start+CHUNK][sel]
        done+=block['core_count'];pred.flush();conf.flush();save(job/'status.json',dict(stage='predicting',points=done,total=n))
        del mask,ids,pp,cc,dataset
        emit('predict',done,n,'Block committed; halo excluded')
    counts={};valid=0;confidence_sum=0.;confidence_count=0
    for start in range(0,n,CHUNK):
        values,c=np.unique(pred[start:start+CHUNK],return_counts=True)
        for k,v in zip(values,c):counts[str(int(k))]=counts.get(str(int(k)),0)+int(v)
        valid+=int((pred[start:start+CHUNK]>=0).sum())
        probabilities=np.asarray(conf[start:start+CHUNK]);finite=np.isfinite(probabilities);confidence_sum+=float(probabilities[finite].sum(dtype='f8'));confidence_count+=int(finite.sum())
    summary=dict(schema='qal-ml-prediction/1.0',point_count=n,valid_prediction_count=valid,classes=counts,
        alignment='original point index; halo excluded',provenance={'model_sha256':p['model_sha256'],'source_fingerprint':p['source_fingerprint']},
        warning=p['warning'],accuracy=None,mean_confidence=confidence_sum/confidence_count if confidence_count else None,
        output_checksums={'predictions.i16':digest(out/'predictions.i16'),'confidence.f32':digest(out/'confidence.f32')})
    save(out/'result.json',summary)
    if p['source_kind']=='las':
        import laspy
        if digest(p['source'])!=p['source_fingerprint']:raise ValueError('Source LAS changed; export refused')
        output=job/'classified.las';temp=job/'classified.partial.las';start=0
        with laspy.open(p['source']) as reader:require_disk(job,n*reader.header.point_format.size+reader.header.offset_to_point_data)
        with laspy.open(p['source']) as reader,laspy.open(temp,mode='w',header=reader.header.copy()) as writer:
            maxclass=31 if reader.header.point_format.id<=5 else 255
            for rows in reader.chunk_iterator(CHUNK):
                canceled(job);values=pred[start:start+len(rows)];sel=values>=0
                if np.any(values[sel]>maxclass):raise ValueError('Predicted classes exceed source LAS format capacity; no truncation performed')
                labels=np.array(rows.classification);labels[sel]=values[sel];rows.classification=labels;writer.write_points(rows);start+=len(rows)
            if reader.header.evlrs:writer.write_evlrs(reader.header.evlrs)
        os.replace(temp,output)
    bars=''.join(f'<tr><td>{html.escape(k)}</td><td>{v:,}</td><td><meter min="0" max="{n}" value="{v}"></meter> {100*v/n:.2f}%</td></tr>' for k,v in counts.items())
    (out/'REPORT.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>ALiS blocked prediction</title><style>body{font:17px system-ui;max-width:1000px;margin:40px auto;color:#18365c}td{padding:8px}pre{white-space:pre-wrap;overflow-wrap:anywhere}meter{width:250px}</style><h1>ALiS — blocked prediction</h1><p>All original points retained; halo excluded from final outputs. Original source labels are unchanged.</p><p><b>Prediction distribution, not accuracy:</b> no independent test labels were supplied. Class -1 means no valid prediction; the source LAS class is retained for these points.</p><table><tr><th>Class</th><th>Points</th><th>Fraction</th></tr>'+bars+'</table><h2>Provenance and confidence</h2><pre>'+html.escape(json.dumps(summary,indent=2))+'</pre><h2>Exact preparation recipe</h2><pre>'+html.escape(json.dumps(p,indent=2))+'</pre></html>',encoding='utf8')
    save(job/'status.json',dict(stage='complete',points=done,total=n));emit('complete',done,n,'Classified outputs ready; original classes unchanged.')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('action',choices=['inspect','prepare','classify']);ap.add_argument('--model');ap.add_argument('--output',type=Path);ap.add_argument('--config',type=Path);ap.add_argument('--job',type=Path)
    a=ap.parse_args()
    try:
        if a.action=='inspect':save(a.output,requirements(a.model))
        elif a.action=='prepare':prepare(read(a.config),a.job)
        else:classify(a.job)
        return 0
    except Exception as e:
        if a.job:save(a.job/'status.json',dict(stage='canceled' if isinstance(e,InterruptedError) else 'failed',error=str(e)))
        emit('error',0,1,str(e));return 1

if __name__=='__main__':sys.exit(main())
