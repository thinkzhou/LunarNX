#include "steamlink/steam_pointer.h"
#include <cassert>
#include <iostream>
using namespace lunar::steamlink;
TouchSample finger(int x,int y) { return {true,1,{1,0},{x,0},{y,0}}; }
int main() {
    auto m=convertMotion({1,2,3},{1,2,3});
    assert(m.valid && std::abs(m.gyro[1]-18.849556f)<0.001f);
    assert(m.gyro[2]<0 && m.accel[0]<0 && m.accel[1]<0 && m.accel[2]>0);
    assert(!convertMotion({NAN,0,0},{}).valid);
    SteamPointer p;
    auto step=[&](TouchSample t,uint64_t ms,bool game=true) { return p.update(t,{},game,false,ms); };
    step(finger(100,100),1000);
    auto r=step({},1100); assert(r.left && !r.right);
    assert(!step({},1150).left);
    step(finger(100,100),1200);
    r=step(finger(130,140),1210); assert(r.dx==30 && r.dy==40);
    assert(!step({},1250).left); // pan is not a tap
    step(finger(100,100),1300);
    assert(step(finger(100,100),1710).left);
    r=step(finger(160,100),1720); assert(r.left && r.dx==60);
    assert(!step({},1730).left);
    TouchSample two{true,2,{1,2},{100,200},{100,100}};
    step(two,1800); assert(step({},1850).right);
    step({},1900); step(two,2000);
    two.y={164,164}; r=step(two,2010); assert(r.wheel==2 && !r.left);
    assert(!step({},2020).right);
    step(finger(200,200),2100);
    assert(!step(finger(200,200),2550,false).left);
    r=step(finger(260,200),2560); assert(!r.left && r.dx==0);
    step({},2570); step(finger(100,100),2600);
    assert(step({},2650).left); // fresh touch accepted after menu release
    p=SteamPointer{}; p.touch_mode=TouchMode::Absolute;
    step(finger(100,100),2990);
    r=step(finger(1279,719),3000); assert(r.absolute && r.x==1 && r.y==1);
    p=SteamPointer{}; p.touch_mode=TouchMode::Absolute;
    p.setVideoSize(960,720);
    r=step(finger(320,360),3010);
    assert(r.absolute && std::abs(r.x-160.f/959.f)<0.0001f);
    step({},3060);
    p=SteamPointer{}; p.touch_mode=TouchMode::Absolute; p.setVideoSize(960,720);
    r=step(finger(100,300),3070); assert(!r.absolute && !r.left);
    assert(!step({},3090).left); // black bar touch is not a game click
    step({},3100); p=SteamPointer{}; p.gyro_mode=GyroMode::Mouse;
    m={true,{1,1,0},{}};
    p.update({},m,true,false,4000);
    r=p.update({},m,true,true,4016); assert(r.dx==-10 && r.dy==-10);
    r=p.update({},m,true,false,4032); assert(r.dx==0 && r.dy==0);
    r=p.update({},m,false,true,4048); assert(r.dx==0 && r.dy==0);
    p=SteamPointer{};
    step(finger(100,100),5000); assert(step(finger(100,100),5450).left);
    TouchSample missing; missing.valid=false;
    assert(!step(missing,5600).left); // no stuck drag on missing input
    p=SteamPointer{};
    step(finger(1250,200),6000);
    r=step(finger(1000,200),6020); assert(r.dx==0 && !r.left);
    assert(!step({},6050).left); // edge swipe never becomes game input
    p=SteamPointer{}; p.fenceTouches();
    step(finger(640,600),6100);
    assert(!step({},6150).left); // held Start-button touch cannot leak into Steam
    step(finger(640,600),6200);
    assert(step({},6250).left);
    std::cout<<"PASS: motion units/axes, tap, pan, drag, right tap, wheel, UI fence, absolute pointer, aim gating, missing samples\n";
}
