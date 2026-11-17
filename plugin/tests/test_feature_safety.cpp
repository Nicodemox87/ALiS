// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureEngine.h"
#include "WindowsUpdates.h"
#include "QuantileSelection.h"
#include <ccPointCloud.h>
#include <GenericProgressCallback.h>
#include <QApplication>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <random>

struct Cancel final : CCCoreLib::GenericProgressCallback {
	void update(float) override {}
	void setMethodTitle(const char*) override {}
	void setInfo(const char*) override {}
	void start() override {}
	void stop() override {}
	bool isCancelRequested() override {return true;}
};
int main(int argc,char** argv) {
	QApplication app(argc,argv);
	int checks=0;
	auto check=[&](bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);};
	try {
		ccPointCloud cloud("Feature safety synthetic cloud");
		check(cloud.reserve(1000),"reserve fixture");
		for(int z=0;z<10;++z)for(int y=0;y<10;++y)for(int x=0;x<10;++x)
			cloud.addPoint(CCVector3(float(x)*.01f,float(y)*.01f,float(z)*.01f));
		alis::FeatureEngine engine(&cloud);alis::ComputeReport report;std::string error;
		std::vector<alis::FeatureRequest> requests={{alis::FeatureId::NeighborCount,.015,""},{alis::FeatureId::Planarity,.015,""},{alis::FeatureId::ZMedian,.015,""}};
		alis::ComputeOptions tinyBudget;tinyBudget.additionalMemoryBudget=1;
		check(!engine.compute(requests,report,error,nullptr,tinyBudget) && error.find("budget")!=std::string::npos,"memory guard fails before allocations");
		check(!cloud.getOctree() && engine.cacheBytes()==0,"budget failure builds no octree and publishes no cache");
		alis::ComputeOptions serial;serial.multiThread=false;
		check(engine.compute(requests,report,error,nullptr,serial),"normal exact computation succeeds");
		const auto expected=*engine.cachedValues({alis::FeatureId::Planarity,.015,""});
		const auto bytes=engine.cacheBytes();
		alis::ComputeOptions dense;dense.maxNeighbourhoodCandidates=100;
		check(!engine.compute({{alis::FeatureId::NeighborCount,2,""}},report,error,nullptr,dense) && error.find("neighbourhood safety")!=std::string::npos,"dense large-radius search rejected before candidate allocation");
		check(engine.cacheBytes()==bytes && !engine.hasCached({alis::FeatureId::NeighborCount,2,""}),"dense failure preserves previous cache and publishes no partial output");
		Cancel cancel;
		check(!engine.compute({{alis::FeatureId::NeighborCount,.02,""}},report,error,&cancel),"cancellation honored before first search");
		check(engine.cacheBytes()==bytes,"cancel preserves previous completed cache");
		engine.clearCache();
		alis::ComputeOptions guarded;guarded.maxNeighbourhoodCandidates=999;
		check(engine.compute(requests,report,error,nullptr,guarded),"bounded parallel searches succeed");
		const auto* actual=engine.cachedValues({alis::FeatureId::Planarity,.015,""});
		bool equivalent=actual && actual->size()==expected.size();
		if(equivalent)for(std::size_t i=0;i<expected.size();++i){
			const double a=(*actual)[i],b=expected[i];
			if(!(std::isnan(a)&&std::isnan(b)) && (!std::isfinite(a)||!std::isfinite(b)||std::abs(a-b)>2e-5*std::max(1.,std::abs(b))))equivalent=false;
		}
		check(equivalent,"bounded parallel output agrees with serial within floating-point tolerance");
		std::vector<unsigned char> mask(cloud.size(),0);for(unsigned i=0;i<cloud.size();++i)mask[i]=(i%3)==0;
		alis::ComputeOptions coreOnly=serial;coreOnly.queryMask=&mask;
		check(engine.compute(requests,report,error,nullptr,coreOnly),"core queries use complete support cloud");
		check(report.queryPointCount==334&&report.reusedFields==0,"partial query scope counted and never reuses complete cache");
		actual=engine.cachedValues({alis::FeatureId::Planarity,.015,""});
		for(unsigned i=0;i<cloud.size();++i){const bool ok=mask[i]?((std::isnan((*actual)[i])&&std::isnan(expected[i]))||std::abs((*actual)[i]-expected[i])<2e-5):std::isnan((*actual)[i]);if(!ok)std::cerr<<"query-mask mismatch row "<<i<<" selected "<<int(mask[i])<<" value "<<(*actual)[i]<<" baseline "<<expected[i]<<" neighbours "<<(*engine.cachedValues({alis::FeatureId::NeighborCount,.015,""}))[i]<<'\n';check(ok,"masked rows retain full-support values; halo rows are NaN");}
		check(engine.compute(requests,report,error,nullptr,serial)&&report.reusedFields==0,"complete request cannot reuse masked NaN cache");
		actual=engine.cachedValues({alis::FeatureId::Planarity,.015,""});for(unsigned i=0;i<cloud.size();++i)check((std::isnan((*actual)[i])&&std::isnan(expected[i]))||std::abs((*actual)[i]-expected[i])<2e-5,"full values restored after partial query scope");
		std::vector<unsigned char> invalidMask(2,1);coreOnly.queryMask=&invalidMask;const auto priorBytes=engine.cacheBytes();
		check(!engine.compute(requests,report,error,nullptr,coreOnly)&&engine.cacheBytes()==priorBytes,"bad query-mask size rejected without cache loss");
		std::mt19937 random(20261107);
		for(unsigned count:{0u,1u,2u,3u,127u,128u,513u,16001u}){
			std::vector<double> values(count);for(auto& value:values)value=double(int(random()%2001)-1000)/13;
			auto sorted=values;std::sort(sorted.begin(),sorted.end());alis::selectQuantileRanks(values);
			if(count)for(double p:{.1,.25,.5,.75,.9}){double pos=p*(count-1);auto lo=std::size_t(std::floor(pos)),hi=std::size_t(std::ceil(pos));check(values[lo]==sorted[lo]&&values[hi]==sorted[hi],"selected interpolated quantile ranks exactly match sorted values");}
		}
		check(alis::newerWindowsVersion("0.1.0-alpha.5.8","0.1.0-alpha.5.7"),"update compares final alpha component");
		check(alis::newerWindowsVersion("0.1.0-alpha.5.10","0.1.0-alpha.5.9"),"alpha comparison is numeric");
		check(!alis::newerWindowsVersion("0.1.0-alpha.5.7","0.1.0-alpha.5.8"),"older package not offered");
		check(alis::windowsInstallerVersion("ALiS-0.1.0-alpha.5.8-Windows-x64-Complete-Setup-r1.exe")=="0.1.0-alpha.5.8","complete installer recognized");
		check(alis::windowsInstallerVersion("ALiS-0.1.0-alpha.5.8-CloudCompare-2.13.2-Windows-x64-Setup.exe")=="0.1.0-alpha.5.8","plugin-only installer recognized");
		auto release=[](const QString& v)->QJsonObject {
			const QString tag="v"+v,name="ALiS-"+v+"-Windows-x64-Complete-Setup-r1.exe";
			QJsonObject asset;asset["name"]=name;asset["size"]=1024;asset["browser_download_url"]=QString("https://github.com/Nicodemox87/ALiS/releases/download/"+tag+"/"+name);
			QJsonObject result;result["tag_name"]=tag;result["html_url"]=QString("https://github.com/Nicodemox87/ALiS/releases/tag/"+tag);result["assets"]=QJsonArray{asset};return result;
		};
		auto next=release("0.1.0-alpha.5.10");
		check(alis::newestWindowsRelease({release("0.1.0-alpha.5.8"),next},"0.1.0-alpha.5.9")==next,"startup updater selects numeric newest platform installer");
		check(alis::newestWindowsRelease({next},"0.1.0-alpha.5.10").isEmpty(),"startup updater ignores current version");
		auto draft=next;draft["draft"]=true;check(alis::newestWindowsRelease({draft},"0.1.0-alpha.5.9").isEmpty(),"draft ignored");
		auto missing=next;missing["assets"]=QJsonArray();check(alis::newestWindowsRelease({missing},"0.1.0-alpha.5.9").isEmpty(),"no installer no update");
		auto foreign=next;foreign["html_url"]="https://example.com/update";check(alis::newestWindowsRelease({foreign},"0.1.0-alpha.5.9").isEmpty(),"foreign release page rejected");
		foreign=next;foreign["assets"]=QJsonArray{QJsonObject{{"name","ALiS-0.1.0-alpha.5.10-Windows-x64-Complete-Setup-r1.exe"},{"size",100},{"browser_download_url","https://example.com/a.exe"}}};
		check(alis::newestWindowsRelease({foreign},"0.1.0-alpha.5.9").isEmpty(),"foreign installer rejected");
		check(alis::newestWindowsRelease({release("invalid")},"0.1.0-alpha.5.9").isEmpty(),"invalid version rejected");
		std::cout<<"PASS: "<<checks<<" feature safety checks\n";return 0;
	} catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
