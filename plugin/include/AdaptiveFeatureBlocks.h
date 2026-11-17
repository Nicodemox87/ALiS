// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FeatureCheckpoint.h"
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <functional>

namespace alis {
struct AdaptiveCore {
    double left=0,bottom=0,right=0,top=0;bool includeRight=false,includeTop=false;
    bool owns(double x,double y)const{return x>=left&&y>=bottom&&(x<right||(includeRight&&x==right))&&(y<top||(includeTop&&y==top));}
    bool context(double x,double y,double halo)const{return x>=left-halo&&x<=right+halo&&y>=bottom-halo&&y<=top+halo;}
    AdaptiveCore child(int k)const{const double mx=left+(right-left)/2,my=bottom+(top-bottom)/2;return {k%2?mx:left,k/2?my:bottom,k%2?right:mx,k/2?top:my,k%2?includeRight:false,k/2?includeTop:false};}
};
struct AdaptiveBlockResult {quint64 corePoints=0,contextPoints=0;int leaves=0;double seconds=0;};
class AdaptiveFeatureBlocks {
public:
    QString folder,workerExecutable;QStringList keys;QJsonObject metadata;double halo=0,bytesPerPoint=0;
    // Budget is queried again for every leaf, not fixed when the dialog opened.
    std::function<quint64()> ramBudget;
    std::function<void()> tick;
    std::function<void(QString)> log;
    std::function<void(const QString&,quint64,const std::array<double,3>&,quint64,std::function<void(int)>)> compute;
    std::function<void(quint64,int)> progress;
    std::function<void(quint64,quint64,double,double)> leafCompleted;
    // An optional additional limit can only REDUCE capacity (also used by tests).
    quint64 maxContextPoints=0;int workerThreads=0;
    AdaptiveBlockResult result;quint64 completedCore=0;
    void heartbeat(){if(tick)tick();}
    quint64 capacity(quint64 bytes)const{
        checkpointCheck(bytes>268435456&&bytesPerPoint>0&&!keys.isEmpty(),"Too little free RAM for an isolated feature worker");
        const long double usable=bytes-268435456;
        const long double perThreadScratch=std::max(0,workerThreads)*96.L;
        quint64 n=quint64(usable/(bytesPerPoint+perThreadScratch));
        // Native candidate scratch is capped at 200k per thread, not a
        // whole-context multiplier for million-point leaves.
        if(n>=200000)n=quint64(std::max(0.L,usable-perThreadScratch*200000)/bytesPerPoint);
        n=std::min(n,quint64(std::numeric_limits<int>::max())/quint64(keys.size()));if(maxContextPoints)n=std::min(n,maxContextPoints);return n;
    }
    // Spools contain original source row ID (u32) + global XYZ (3 f64).
    // A parent is read only from disk during subdivision, never cloned in CC.
    QString run(const QString& spool,const AdaptiveCore& core,quint64 expected,const QString& finalPath){
        QElapsedTimer timer;timer.start();result={};completedCore=0;int nodes=0;
        process(spool,core,expected,finalPath,0,nodes);result.seconds=timer.elapsed()/1000.;checkpointCheck(result.corePoints==expected,"Adaptive parent ownership mismatch");return finalPath;
    }
private:
    void process(const QString& input,const AdaptiveCore& core,quint64 expected,const QString& output,int depth,int& nodes){
        QElapsedTimer leafTimer;leafTimer.start();
        heartbeat();checkpointCheck(++nodes<=16384,"Adaptive subdivision limit reached; inspect dense/coincident points or radius");
        QTemporaryDir scratch(QDir(folder).filePath(".adaptive_XXXXXX"));checkpointCheck(scratch.isValid(),"Cannot create adaptive scratch directory");
        QFile src(input),xyz(scratch.filePath("xyz.f64")),queries(scratch.filePath("query_indices.u32"));checkpointCheck(src.open(QIODevice::ReadOnly)&&src.size()%28==0&&xyz.open(QIODevice::WriteOnly)&&queries.open(QIODevice::WriteOnly),"Invalid adaptive coordinate spool");
        quint64 n=0,own=0;QByteArray xyzBuffer,queryBuffer;xyzBuffer.reserve(1048576);queryBuffer.reserve(1048576);const auto allowance=ramBudget();const quint64 cap=capacity(allowance);bool fits=true;
        while(!src.atEnd()){const auto bytes=src.read(28*32768);checkpointCheck(bytes.size()%28==0,"Truncated adaptive coordinate spool");for(int p=0;p<bytes.size();p+=28){double v[3];std::memcpy(v,bytes.constData()+p+4,24);if(!core.context(v[0],v[1],halo))continue;++n;const bool isCore=core.owns(v[0],v[1]);own+=isCore;if(n>cap)fits=false;if(fits){xyzBuffer.append(bytes.constData()+p+4,24);if(isCore){const quint32 index=quint32(n-1);queryBuffer.append(reinterpret_cast<const char*>(&index),4);}if(xyzBuffer.size()>=1048576){checkpointCheck(xyz.write(xyzBuffer)==xyzBuffer.size()&&queries.write(queryBuffer)==queryBuffer.size(),"Adaptive XYZ/query write failed");xyzBuffer.clear();queryBuffer.clear();}}}heartbeat();}
        checkpointCheck(own==expected&&own>0,"Adaptive core membership mismatch");
        if(fits&&n>capacity(ramBudget()))fits=false; // Other applications may have consumed RAM during the scan.
        if(fits){checkpointCheck(xyz.write(xyzBuffer)==xyzBuffer.size()&&queries.write(queryBuffer)==queryBuffer.size(),"Adaptive XYZ/query write failed");xyz.close();queries.close();
            src.seek(0);std::array<double,3> origin{};bool found=false;while(!src.atEnd()&&!found){auto bytes=src.read(28*32768);for(int p=0;p<bytes.size();p+=28){double v[3];std::memcpy(v,bytes.constData()+p+4,24);if(core.owns(v[0],v[1])){origin={{v[0],v[1],v[2]}};found=true;break;}}heartbeat();}
            checkpointCheck(found,"Missing adaptive core origin");
            // A worker-reported memory failure may be retried with smaller spatial cores.
            try{compute(scratch.path(),n,origin,std::min(allowance,ramBudget()),[&](int pc){if(progress)progress(completedCore+quint64(double(own)*pc/100),result.leaves);});}
            catch(const std::exception& e){const QString why=QString::fromUtf8(e.what());if(!why.contains("memory",Qt::CaseInsensitive)&&!why.contains("RAM",Qt::CaseInsensitive)&&!why.contains("bad_alloc",Qt::CaseInsensitive))throw;fits=false;if(log)log("Worker memory pressure: subdividing this core, preserving its full halo.");}
            if(fits){
                QFile values(scratch.filePath("features.f32"));checkpointCheck(values.open(QIODevice::ReadOnly)&&quint64(values.size())==n*keys.size()*4,"Invalid adaptive worker feature matrix");
                FeatureCheckpointWriter out(output,metadata,quint32(own),quint32(keys.size()));src.seek(0);quint64 contextRow=0;
                while(!src.atEnd()){auto bytes=src.read(28*32768);for(int p=0;p<bytes.size();p+=28){quint32 id;double v[3];std::memcpy(&id,bytes.constData()+p,4);std::memcpy(v,bytes.constData()+p+4,24);if(!core.context(v[0],v[1],halo))continue;if(core.owns(v[0],v[1])){checkpointCheck(values.seek(qint64(contextRow)*keys.size()*4),"Cannot seek adaptive feature row");auto row=values.read(keys.size()*4);checkpointCheck(row.size()==keys.size()*4,"Truncated adaptive features");out.append(id,row.constData());}++contextRow;}heartbeat();}
                out.finish();result.corePoints+=own;result.contextPoints+=n;result.leaves++;completedCore+=own;if(leafCompleted)leafCompleted(own,n,(core.right-core.left)*(core.top-core.bottom),leafTimer.elapsed()/1000.);if(progress)progress(completedCore,result.leaves);return;
            }
        }
        xyz.close();queries.close();xyzBuffer.clear();queryBuffer.clear();src.close();
        checkpointCheck(depth<12&&(core.right-core.left>1e-6||core.top-core.bottom>1e-6),"Even the minimum core's full halo exceeds available RAM. Reduce radii or free RAM; no points were discarded.");
        if(log)log(QString("Adaptive split: %1 x %2 m; %3 core / %4 context points (capacity %5).").arg(core.right-core.left,0,'g',5).arg(core.top-core.bottom,0,'g',5).arg(own).arg(n).arg(cap));
        std::array<AdaptiveCore,4> children;std::array<quint64,4> counts{{0,0,0,0}};std::array<std::unique_ptr<QFile>,4> spools;std::array<QByteArray,4> buffers;
        for(int k=0;k<4;++k){children[k]=core.child(k);spools[k].reset(new QFile(scratch.filePath(QString("child%1.xyzid").arg(k))));checkpointCheck(spools[k]->open(QIODevice::WriteOnly),"Cannot spool adaptive child");buffers[k].reserve(1048576);}
        checkpointCheck(src.open(QIODevice::ReadOnly),"Cannot reopen adaptive parent");
        while(!src.atEnd()){auto bytes=src.read(28*32768);for(int p=0;p<bytes.size();p+=28){double v[3];std::memcpy(v,bytes.constData()+p+4,24);for(int k=0;k<4;++k){if(children[k].owns(v[0],v[1]))++counts[k];if(children[k].context(v[0],v[1],halo)){buffers[k].append(bytes.constData()+p,28);if(buffers[k].size()>=1048576){checkpointCheck(spools[k]->write(buffers[k])==buffers[k].size(),"Adaptive child disk write failed");buffers[k].clear();}}}}heartbeat();}src.close();
        quint64 total=0;for(int k=0;k<4;++k){total+=counts[k];checkpointCheck(spools[k]->write(buffers[k])==buffers[k].size(),"Adaptive child disk write failed");spools[k]->close();buffers[k].clear();}checkpointCheck(total==own,"Adaptive split lost or duplicated source points");
        QStringList pieces;for(int k=0;k<4;++k)if(counts[k]){auto part=scratch.filePath(QString("child%1.alsf").arg(k));process(spools[k]->fileName(),children[k],counts[k],part,depth+1,nodes);pieces<<part;}
        mergeFeatureCheckpoints(pieces,output,metadata,quint32(own),quint32(keys.size()),[&](){heartbeat();});
    }
};
}
