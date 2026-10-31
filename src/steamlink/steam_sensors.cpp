#include "steam_sensors.h"
#include "../diagnostics.h"
#include <chrono>
#ifdef __SWITCH__
namespace lunar::steamlink {
SteamSensors::SteamSensors(bool motion_enabled) {
    padInitializeDefault(&pad_);
    if (!motion_enabled) return;
    Result rc[] = {
        hidGetSixAxisSensorHandles(&handles_[0],1,HidNpadIdType_Handheld,HidNpadStyleTag_NpadHandheld),
        hidGetSixAxisSensorHandles(&handles_[1],1,HidNpadIdType_No1,HidNpadStyleTag_NpadFullKey),
        hidGetSixAxisSensorHandles(&handles_[2],2,HidNpadIdType_No1,HidNpadStyleTag_NpadJoyDual)};
    for (int i=0;i<4;++i) if (R_SUCCEEDED(rc[std::min(i,2)]))
        started_[i] = R_SUCCEEDED(hidStartSixAxisSensor(handles_[i]));
    lunar::diagnosticLog("steam-motion","sensors started=%d%d%d%d; keep still for calibration",
        int(started_[0]),int(started_[1]),int(started_[2]),int(started_[3]));
}
SteamSensors::~SteamSensors() {
    for (int i=0;i<4;++i) if (started_[i]) hidStopSixAxisSensor(handles_[i]);
}
MotionSample SteamSensors::read() {
    padUpdate(&pad_);
    if (!padIsConnected(&pad_)) { last_source_=-1; return {}; }
    auto style=padGetStyleSet(&pad_); int index=-1;
    if (style&HidNpadStyleTag_NpadHandheld) index=0;
    else if (style&HidNpadStyleTag_NpadFullKey) index=1;
    else if (style&HidNpadStyleTag_NpadJoyDual)
        index=(padGetAttributes(&pad_)&HidNpadAttribute_IsLeftConnected)?2:3;
    if (index<0 || !started_[index]) return {};
    if (index!=last_source_) {
        bias_={}; sum_={}; calibration_=0; last_sample_=0; last_fresh_ms_=0; last_source_=index;
        lunar::diagnosticLog("steam-motion","source=%d calibration pending",index);
    }
    HidSixAxisSensorState raw{};
    if (!hidGetSixAxisSensorStates(handles_[index],&raw,1)) return {};
    const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool fresh=raw.sampling_number!=last_sample_;
    if(fresh) { last_sample_=raw.sampling_number; last_fresh_ms_=now; }
    if (!last_fresh_ms_ || now-last_fresh_ms_>100) return {};
    auto m=convertMotion({raw.angular_velocity.x,raw.angular_velocity.y,raw.angular_velocity.z},
                         {raw.acceleration.x,raw.acceleration.y,raw.acceleration.z});
    if (!m.valid) return {};
    if (calibration_<125 && fresh) {
        float speed=0,gravity=0;
        for(int i=0;i<3;++i) { speed+=m.gyro[i]*m.gyro[i]; gravity+=m.accel[i]*m.accel[i]; }
        if (speed<0.01f && std::abs(std::sqrt(gravity)-9.80665f)<0.5f) {
            for(int i=0;i<3;++i) sum_[i]+=m.gyro[i];
            if (++calibration_==125) {
                for(int i=0;i<3;++i) bias_[i]=sum_[i]/125.f;
                lunar::diagnosticLog("steam-motion","calibration complete");
            }
        } else { sum_={}; calibration_=0; }
    }
    for(int i=0;i<3;++i) m.gyro[i]-=bias_[i];
    return m;
}
TouchSample SteamSensors::touch() {
    HidTouchScreenState raw{}; TouchSample t;
    t.valid=hidGetTouchScreenStates(&raw,1)>0;
    if (!t.valid) return t;
    t.count=std::min(int(raw.count),2);
    for(int i=0;i<t.count;++i) {
        t.id[i]=raw.touches[i].finger_id; t.x[i]=raw.touches[i].x; t.y[i]=raw.touches[i].y;
    }
    if(t.count==2 && t.id[0]>t.id[1]) { std::swap(t.id[0],t.id[1]); std::swap(t.x[0],t.x[1]); std::swap(t.y[0],t.y[1]); }
    return t;
}
}
#endif
