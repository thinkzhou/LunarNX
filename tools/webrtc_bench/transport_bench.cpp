// Isolated synthetic RTP transport benchmark; no codec or application integration.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#ifdef BENCH_LIBPEER
extern "C" {
#include "peer.h"
}
#else
#include "rtc/rtc.hpp"
#endif
using Clock = std::chrono::steady_clock;
static uint64_t ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
static double cpu() { rusage r{}; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6; }
static void writeFile(const std::string& path, const std::string& data) {
    { std::ofstream out(path+".tmp"); if (!(out << data)) throw std::runtime_error("write failed: "+path); }
    std::filesystem::rename(path+".tmp",path);
}
static std::string readFile(const std::string& path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in),{}}; }
static void waitFile(const std::string& path) {
    auto end=Clock::now()+std::chrono::seconds(25);
    while (!std::filesystem::exists(path)) {
        if (Clock::now()>end) throw std::runtime_error("file timeout: "+path);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
struct Samples {
    std::mutex lock;
    std::vector<double> latency;
    std::vector<uint8_t> seen = std::vector<uint8_t>(2000000);
    uint64_t first=0, bytes=0, duplicates=0, invalid=0;
    void packet(const uint8_t* p,size_t size) {
        auto now=ns();
        std::lock_guard<std::mutex> guard(lock);
        if (size!=1200 || p[12]!=0x61) { ++invalid; return; }
        uint64_t sent, index; memcpy(&sent,p+16,8); memcpy(&index,p+24,8);
        if (index>=seen.size() || sent>now) { ++invalid; return; }
        if (!first) first=now;
        if (!p[13]) return; // warm-up traffic
        if (seen[index]++) {++duplicates; return;}
        latency.push_back((now-sent)/1e6); bytes+=size;
    }
    void result(const std::string& path,uint64_t start,uint64_t applied,double cpuDelta,double wall) {
        std::lock_guard<std::mutex> guard(lock);
        std::sort(latency.begin(),latency.end());
        auto pct=[&](double q){return latency.empty()? -1.:latency[std::min(latency.size()-1,size_t(q*(latency.size()-1)))];};
        std::ofstream out(path);
        out << "{\"received\":" << latency.size() << ",\"bytes\":" << bytes
            << ",\"duplicates\":" << duplicates << ",\"invalid\":" << invalid
            << ",\"p50_ms\":" << pct(.5) << ",\"p95_ms\":" << pct(.95) << ",\"p99_ms\":" << pct(.99)
            << ",\"first_packet_from_start_ms\":" << (first?double(first-start)/1e6:-1)
            << ",\"first_packet_from_remote_sdp_ms\":" << (first?double(first-applied)/1e6:-1)
            << ",\"cpu_seconds\":" << cpuDelta << ",\"measurement_wall_seconds\":" << wall
            << ",\"cpu_percent_one_core\":" << cpuDelta/wall*100 << "}\n";
    }
};
int main(int argc,char** argv) try {
    if (argc!=6) throw std::runtime_error("usage: binary sender|receiver trial-dir Mbps seconds idle-us");
    const bool sender=std::string(argv[1])=="sender";
    const std::string dir=argv[2];
    const double mbps=std::stod(argv[3]), seconds=std::stod(argv[4]);
    const int idleUs=std::stoi(argv[5]);
    if(mbps<=0 || seconds<=0 || idleUs<0) throw std::runtime_error("invalid arguments");
    std::filesystem::create_directories(dir);
    Samples samples;
    auto start=ns(); uint64_t applied=0;
#ifdef BENCH_LIBPEER
    if(sender) throw std::runtime_error("sender must use shared libdatachannel binary");
    if(peer_init()!=0) throw std::runtime_error("peer_init failed");
    PeerConfiguration config{};
    config.video_codec=CODEC_H264; config.datachannel=DATA_CHANNEL_NONE;
    config.raw_video_rtp=1; config.user_data=&samples;
    config.onvideotrack=[](uint8_t* p,size_t n,uint16_t,uint32_t,void* user){static_cast<Samples*>(user)->packet(p,n);};
    auto pc=peer_connection_create(&config);
    if(!pc) throw std::runtime_error("peer create failed");
    peer_connection_oniceconnectionstatechange(pc,[](PeerConnectionState state,void*){
        std::cerr << "libpeer state " << peer_connection_state_to_string(state) << '\n';
    });
    waitFile(dir+"/offer.sdp");
    const auto offer=readFile(dir+"/offer.sdp"); applied=ns();
    peer_connection_set_remote_description(pc,offer.c_str(),SDP_TYPE_OFFER);
    const char* answer=peer_connection_create_answer(pc);
    if(!answer) throw std::runtime_error("answer failed");
    writeFile(dir+"/answer.sdp",answer);
    auto pump=[&](){
        auto end=Clock::now()+std::chrono::milliseconds(3);
        for(int i=0;i<8;++i) if(peer_connection_loop(pc)<=0 || Clock::now()>=end) break;
        if(idleUs) std::this_thread::sleep_for(std::chrono::microseconds(idleUs));
    };
#else
    rtc::InitLogger(std::getenv("BENCH_DEBUG")?rtc::LogLevel::Debug:rtc::LogLevel::Error);
    rtc::Configuration config; config.disableAutoNegotiation=true;
    if (const char* address=std::getenv("BENCH_BIND_ADDRESS")) config.bindAddress=address;
    std::atomic<bool> gathered{false};
    rtc::PeerConnection pc(config);
    pc.onGatheringStateChange([&](rtc::PeerConnection::GatheringState state){ if(state==rtc::PeerConnection::GatheringState::Complete) gathered=true; });
    std::shared_ptr<rtc::Track> track;
    if(sender) {
        rtc::Description::Video video("0",rtc::Description::Direction::SendOnly);
        video.addH264Codec(102); video.addSSRC(0x1234,"bench");
        track=pc.addTrack(video);
        pc.setLocalDescription(rtc::Description::Type::Offer);
    } else {
        rtc::Description::Video video("0",rtc::Description::Direction::RecvOnly);
        video.addH264Codec(102);
        track=pc.addTrack(video);
        track->onMessage([&](rtc::binary packet){samples.packet(reinterpret_cast<const uint8_t*>(packet.data()),packet.size());},nullptr);
        waitFile(dir+"/offer.sdp"); applied=ns();
        pc.setRemoteDescription(rtc::Description(readFile(dir+"/offer.sdp"),"offer"));
        pc.setLocalDescription(rtc::Description::Type::Answer);
    }
    auto timeout=Clock::now()+std::chrono::seconds(25);
    while(!gathered) {
        if(Clock::now()>timeout) throw std::runtime_error("gather timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    writeFile(dir+(sender?"/offer.sdp":"/answer.sdp"),std::string(*pc.localDescription()));
    if(sender) {
        waitFile(dir+"/answer.sdp"); applied=ns();
        pc.setRemoteDescription(rtc::Description(readFile(dir+"/answer.sdp"),"answer"));
    }
    if(sender) {
        timeout=Clock::now()+std::chrono::seconds(25);
        while(!track || !track->isOpen()) {
            if(Clock::now()>timeout) throw std::runtime_error("track open timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::vector<uint8_t> packet(1200,0x55);
        packet[0]=0x80; packet[1]=102; packet[8]=0; packet[9]=0;packet[10]=0x12;packet[11]=0x34;packet[12]=0x61;
        uint64_t index=0, sentMeasured=0, failed=0;
        auto origin=Clock::now();
        const double ppf=mbps*1e6/(8*packet.size()*60);
        const int frames=int((seconds+1)*60);
        for(int frame=0;frame<frames;++frame) {
            std::this_thread::sleep_until(origin+std::chrono::nanoseconds(int64_t(frame*(1e9/60))));
            if(frame==60) writeFile(dir+"/measurement.start",std::to_string(ns()));
            const int count=int((frame+1)*ppf)-int(frame*ppf);
            for(int i=0;i<count;++i) {
                uint16_t seq=index;packet[2]=seq>>8;packet[3]=seq&255;
                uint32_t ts=frame*1500;packet[4]=ts>>24;packet[5]=ts>>16;packet[6]=ts>>8;packet[7]=ts;
                packet[1]=102|(i==count-1?128:0);packet[13]=frame>=60;
                auto sent=ns();memcpy(packet.data()+16,&sent,8);memcpy(packet.data()+24,&index,8);
                if(!track->send(reinterpret_cast<const std::byte*>(packet.data()),packet.size())) ++failed;
                if(frame>=60)++sentMeasured;
                ++index;
            }
        }
        std::this_thread::sleep_until(origin+std::chrono::nanoseconds(int64_t((seconds+1)*1e9)));
        const double actual=std::chrono::duration<double>(Clock::now()-origin).count()-1;
        writeFile(dir+"/sender.json","{\"sent\":"+std::to_string(sentMeasured)+",\"failed_sends\":"+std::to_string(failed)+",\"actual_seconds\":"+std::to_string(actual)+",\"actual_mbps\":"+std::to_string(sentMeasured*1200*8/actual/1e6)+"}\n");
        writeFile(dir+"/measurement.stop",std::to_string(ns()));
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        writeFile(dir+"/sender.done","done");
        pc.close();
        return 0;
    }
    auto pump=[&](){if(idleUs) std::this_thread::sleep_for(std::chrono::microseconds(idleUs));};
#endif
    auto deadline=Clock::now()+std::chrono::seconds(int(seconds)+35);
    double cpuStart=0,cpuStop=0,wall=0;
    bool measuring=false,stopped=false;
    while(!std::filesystem::exists(dir+"/sender.done")) {
        if(Clock::now()>deadline) throw std::runtime_error("receiver timeout");
        if(!measuring && std::filesystem::exists(dir+"/measurement.start")) {cpuStart=cpu();wall=double(ns())/1e9;measuring=true;}
        pump();
        if(measuring && !stopped && std::filesystem::exists(dir+"/measurement.stop")) {cpuStop=cpu();wall=double(ns())/1e9-wall;stopped=true;}
    }
    if(!stopped) throw std::runtime_error("no measurement interval");
    samples.result(dir+"/receiver.json",start,applied,cpuStop-cpuStart,wall);
#ifdef BENCH_LIBPEER
    peer_connection_close(pc);peer_connection_destroy(pc);peer_deinit();
#else
    pc.close();
#endif
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
