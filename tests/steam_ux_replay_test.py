#!/usr/bin/env python3
"""Replay production UI handlers with controlled worker scheduling and save failures."""
from pathlib import Path
import subprocess,tempfile,sys
r=Path(__file__).resolve().parents[1]
def extract(text,sig):
 a=text.index(sig); start=text.index('{',a); n=1;i=start+1
 while n:
  if text[i]=='{':n+=1
  if text[i]=='}':n-=1
  i+=1
 return text[a:i]
source=(r/'src/ui/stream_view.cpp').read_text()
method=extract(source,'void StreamView::handleWindowFocusChanged(bool focused)')
method += extract((r/'src/ui/button_mapping_activity.cpp').read_text(), 'void ButtonMappingActivity::finishCapture()')
method += extract((r/'src/ui/button_mapping_activity.cpp').read_text(), 'void ButtonMappingActivity::cancelCapture()')
method += extract((r/'src/ui/button_mapping_activity.cpp').read_text(), 'void ButtonMappingActivity::pollCaptureInput()')
pre=r'''
#include <atomic>
#include <array>
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <iostream>
#include <cassert>
#include "common/operation_generation.h"
#include "steamlink/steam_pointer.h"
std::vector<std::function<void()>> workers;
namespace lunar {
template<class... T> void diagnosticLog(T...) {}
namespace platform { bool startNetworkWorker(const char*,std::function<void()> f) { workers.push_back(f);return true;} }
}
namespace brls {
const char* getStr(const char* s) { return s; }
void sync(std::function<void()> f) { f(); }
enum class Visibility { VISIBLE, GONE };
struct Application {
 static inline int notifications=0;
 static void notify(const char*) { ++notifications; }
 static void giveFocus(void*) {}
};
}
constexpr uint64_t HidNpadButton_Minus=1, HidNpadButton_Plus=2;
constexpr int kCaptureReleaseFrames=6;
constexpr uint64_t kSupportedButtons=255;
uint64_t physical_buttons=0;
void padUpdate(int*) {}
uint64_t padGetButtons(int*) { return physical_buttons; }
namespace input {
 constexpr uint64_t kButtonMappingCapture=128;
 bool isCaptureButtonPressed() {return false;}
 const char* formatHidButtonMask(uint64_t) {return "buttons";}
 bool writes_succeed=false;
 bool saveButtonMapping(int,const std::array<uint64_t,2>&) { return writes_succeed; }
}
struct Element {
 void setText(const char*) {}
 void setVisibility(brls::Visibility) {}
};
struct ButtonMappingActivity {
 std::array<uint64_t,2> mapping_{4,8};
 int capture_pad_=0, profile_=0, release_frames_=0, refreshed=0;
 bool waiting_for_release_=false;
 size_t capture_index_=0;
 uint64_t peak_buttons_=16;
 bool saw_button_=true, capturing_=true;
 Element element;
 Element *capture_status_=&element, *capture_content_=&element, *mapping_content_=&element;
 std::vector<Element*> rows_{&element};
 void refreshRows() {++refreshed;}
 void finishCapture();
 void cancelCapture();
 void pollCaptureInput();
};
struct Runtime {
 bool suspended=false;
 void setVideoPresentationSuspended(bool b) {suspended=b;}
 bool resumeAfterForeground(std::function<bool()> cancelled) {return !cancelled();}
};
struct StreamView {
 std::shared_ptr<Runtime> runtime_=std::make_shared<Runtime>();
 std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
 std::shared_ptr<std::atomic<bool>> terminal_stop_=std::make_shared<std::atomic<bool>>(false);
 std::shared_ptr<lunar::common::OperationGeneration> lifecycle_generation_=std::make_shared<lunar::common::OperationGeneration>();
 std::atomic<bool> backgrounded_{false},stop_started_{false},foreground_recovery_running_{false};
 bool child_activity_visible_=false;
 bool ui_owns_input=false;
 void updateInputOwnership() { ui_owns_input=backgrounded_||foreground_recovery_running_; }
 void stopAndReturn() {stop_started_=true;}
 void handleWindowFocusChanged(bool focused);
};
'''
main=r'''
int main() {
 ButtonMappingActivity mapping;
 const int notices=brls::Application::notifications;
 mapping.finishCapture();
 assert(mapping.mapping_[0]==4 && !mapping.capturing_ && mapping.refreshed==1);
 assert(brls::Application::notifications==notices+1);
 input::writes_succeed=true; mapping.capturing_=true; mapping.finishCapture();
 assert(mapping.mapping_[0]==16 && !mapping.capturing_);
 std::cout << "PASS: mapping save failure preserves old mapping, remains navigable and retries successfully\n";

 mapping.capturing_=true;
 const auto saved=mapping.mapping_;
 physical_buttons=HidNpadButton_Minus | HidNpadButton_Plus;
 mapping.pollCaptureInput();
 assert(!mapping.capturing_ && mapping.mapping_==saved && mapping.peak_buttons_==0);
 std::cout << "PASS: cancel preserves saved mapping\n";
 mapping.capturing_=true; mapping.waiting_for_release_=true;
 physical_buttons=0; mapping.pollCaptureInput();
 physical_buttons=4; mapping.pollCaptureInput(); // B remains assignable.
 physical_buttons=0;
 for(int i=0;i<kCaptureReleaseFrames;++i) mapping.pollCaptureInput();
 assert(!mapping.capturing_ && mapping.mapping_[0]==4);
 StreamView view;
 view.handleWindowFocusChanged(false);
 view.handleWindowFocusChanged(true);
 view.handleWindowFocusChanged(false);
 view.handleWindowFocusChanged(true);
 workers.at(0)();
 std::cout << "focus replay: backgrounded=" << view.backgrounded_ << " ui_owns_input=" << view.ui_owns_input << " suspended=" << view.runtime_->suspended << '\n';
 assert(!view.backgrounded_ && !view.ui_owns_input && !view.runtime_->suspended);
 StreamView hidden;
 hidden.handleWindowFocusChanged(false); hidden.handleWindowFocusChanged(true);
 hidden.handleWindowFocusChanged(false); workers.at(1)();
 assert(hidden.backgrounded_ && hidden.ui_owns_input && hidden.runtime_->suspended);
 StreamView settings;
 settings.child_activity_visible_=true;
 settings.handleWindowFocusChanged(false); settings.handleWindowFocusChanged(true); workers.at(2)();
 assert(settings.runtime_->suspended);
 using namespace lunar::steamlink;
 for (int x: {640,1240}) {
  SteamPointer p; p.touch_mode=TouchMode::Absolute;
  TouchSample t; p.update(t,{},true,false,1);
  t.count=1;t.id[0]=1;t.x[0]=x;t.y[0]=20;
  auto down=p.update(t,{},true,false,20);
  t.count=0;
  auto up=p.update(t,{},true,false,70);
  std::cout << "absolute tap x=" << x << " position=" << down.absolute << " click=" << up.left << '\n';
  assert(x==640 ? down.absolute && up.left : !down.absolute && up.absolute && up.left);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='lunar-ux-review-') as d:
 p=Path(d);(p/'replay.cpp').write_text(pre+method+main)
 flags=[]
 if sys.platform == 'darwin':
  sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
  flags=['-isysroot',sdk,'-isystem',sdk+'/usr/include/c++/v1']
 subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',*flags,'-I'+str(r/'src'),str(p/'replay.cpp'),'-o',str(p/'replay')],check=True)
 subprocess.run([str(p/'replay')],check=True)
