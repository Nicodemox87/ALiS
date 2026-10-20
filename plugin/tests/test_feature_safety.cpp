// SPDX-License-Identifier: GPL-2.0-or-later
#include "FeatureEngine.h"
#include "WindowsUpdates.h"
#include <ccPointCloud.h>
#include <GenericProgressCallback.h>
#include <QApplication>
#include <iostream>
#include <stdexcept>
#include <cmath>

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
		check(alis::newerWindowsVersion("0.1.0-alpha.5.8","0.1.0-alpha.5.7"),"update compares final alpha component");
		check(alis::newerWindowsVersion("0.1.0-alpha.5.10","0.1.0-alpha.5.9"),"alpha comparison is numeric");
		check(!alis::newerWindowsVersion("0.1.0-alpha.5.7","0.1.0-alpha.5.8"),"older package not offered");
		check(alis::windowsInstallerVersion("ALiS-0.1.0-alpha.5.8-Windows-x64-Complete-Setup-r1.exe")=="0.1.0-alpha.5.8","complete installer recognized");
		check(alis::windowsInstallerVersion("ALiS-0.1.0-alpha.5.8-CloudCompare-2.13.2-Windows-x64-Setup.exe")=="0.1.0-alpha.5.8","plugin-only installer recognized");
		std::cout<<"PASS: "<<checks<<" feature safety checks\n";return 0;
	} catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
