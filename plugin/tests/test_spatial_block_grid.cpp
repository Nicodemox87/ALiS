// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpatialBlockGrid.h"
#include <iostream>
#include <limits>
int spatialGridRegression(){int n=0;auto check=[&](bool b){++n;if(!b)throw std::runtime_error("Spatial block grid regression");};
 alis::SpatialBlockGrid g(440000,4135000,440250,4135250,50);check(g.size()==25);check(g.cell(440000,4135000)==0);check(g.cell(440050,4135000)==1);check(g.cell(440250,4135250)==24);check(g.cell(439999,4135000)==-1);check(g.cell(440251,4135250)==-1);check(g.cell(std::numeric_limits<double>::quiet_NaN(),4135000)==-1);
 check(g.context(0,440050.4,4135020,.5));check(g.cell(440050.4,4135020)==1);check(!g.context(0,440050.6,4135020,.5));
 alis::SpatialBlockGrid partial(0,0,125,99,50);check(partial.size()==6);check(partial.right(2)==125);check(partial.top(5)==99);check(partial.cell(125,99)==5);
 for(int y=0;y<=99;++y)for(int x=0;x<=125;++x){int core=partial.cell(x,y);check(core>=0&&core<partial.size());check(partial.context(core,x,y,0));}
 bool rejected=false;try{alis::SpatialBlockGrid huge(0,0,1000,1000,1);}catch(...){rejected=true;}check(rejected);
 rejected=false;try{alis::SpatialBlockGrid invalid(0,0,10,10,0);}catch(...){rejected=true;}check(rejected);
 std::cout<<n<<" grid / edge / halo checks passed"<<std::endl;
 return 0;
}
