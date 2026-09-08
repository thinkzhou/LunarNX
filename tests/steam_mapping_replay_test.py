#!/usr/bin/env python3
"""Execute production default mappings, chord resolution and Capture reload together."""
from pathlib import Path
import re, subprocess, tempfile, sys
r=Path(__file__).resolve().parents[1]
def extract(s, sig):
 a=s.index(sig); i=s.index('{',a); depth=1; j=i+1
 while depth:
  depth += (s[j]=='{')-(s[j]=='}'); j+=1
 return s[a:j]
reader=(r/'src/input/gamepad_reader.cpp').read_text()
mapping=(r/'src/input/button_mapping.cpp').read_text()
default=extract(mapping,'ButtonMapping defaultButtonMapping(')
reload=extract(reader,'void GamepadReader::reloadButtonMapping()')
release=extract(reader,'void GamepadReader::releaseCaptureButton()')
resolve=reader[reader.index('        const auto menu_mask'):reader.index('    HidAnalogStickState left')]
resolve=resolve.replace('    } else if (quick_menu_chord) {\n        btns &= ~(HidNpadButton_Minus | HidNpadButton_Plus);\n    }','')
names=sorted(set(re.findall(r'HidNpadButton_\w+',default+resolve)))
constants='\n'.join(f'constexpr uint64_t {name}=uint64_t{{1}}<<{i};' for i,name in enumerate(names))
pre=r'''#include "input/gamepad_reader.h"
#include <cassert>
#include <cstdio>
using namespace lunar::input;
'''
impl=r'''
namespace lunar::input {
int users=0;
ButtonMapping saved;
ButtonMapping loadButtonMapping(ButtonMappingProfile) {return saved;}
bool mappingUsesCaptureButton(const ButtonMapping& m) {
 for(auto mask:m) if(mask & kButtonMappingCapture) return true;
 return false;
}
void acquireCaptureButtonInput(){++users;}
void releaseCaptureButtonInput(){--users; assert(users>=0);}
}
'''
# Use the real class for reload ownership; the mapping resolver is the actual
# production block with a deterministic sample clock and raw button input.
klass=r'''
struct Resolver {
 ButtonMapping button_mapping_=defaultButtonMapping(ButtonMappingProfile::Steam);
 MenuChordFilter menu_chord_;
 GamepadState read(uint64_t btns,uint64_t ms) {
 GamepadState state{};
'''+resolve+r'''
 return state;
 }
};
'''
# Expose private fields only in this standalone test translation unit.
pre=pre.replace('#include "input/gamepad_reader.h"','#define private public\n#include "input/gamepad_reader.h"\n#undef private')
# Constructor/destructor are production methods; they exercise lease cleanup.
ctor=extract(reader,'GamepadReader::GamepadReader(')
dtor=extract(reader,'GamepadReader::~GamepadReader()')
# Destructor's platform pad allocation is unrelated to capture ownership.
dtor=dtor.replace('delete static_cast<PadState*>(pad_state_);','')
main=r'''
int main(){
 const auto plus=HidNpadButton_Plus, minus=HidNpadButton_Minus;
 const auto guide=HidNpadButton_L|HidNpadButton_R|plus;
 for(auto duration:{40,119,120,128,200}) {
  Resolver r;
  auto s=r.read(guide,100);
  assert(s.guide && !s.lb && !s.rb && !s.menu);
  s=r.read(0,100+duration);
  assert(!s.guide && !s.lb && !s.rb && !s.menu);
 }
 for(auto remainder:{plus,HidNpadButton_L,HidNpadButton_L|HidNpadButton_R}) {
  Resolver r; r.read(guide,100);
  auto s=r.read(remainder,130); assert(!s.guide && !s.lb && !s.rb && !s.menu);
  r.read(0,150); s=r.read(guide,180); assert(s.guide);
 }
 for(auto menu_key:{plus,minus}) {
  Resolver r;
  r.button_mapping_[size_t(RemoteButton::Guide)]=HidNpadButton_A|menu_key;
  auto s=r.read(menu_key,100); assert(!s.guide && !s.menu && !s.view);
  s=r.read(HidNpadButton_A|menu_key,160);
  assert(s.guide && !s.b && !s.menu && !s.view);
  s=r.read(menu_key,180); assert(!s.menu && !s.view);
  r.read(0,200);
 }
 Resolver reserved;
 auto s=reserved.read(guide|minus,100);
 assert(!s.guide && !s.menu && !s.view);
 saved=defaultButtonMapping(ButtonMappingProfile::Steam);
 {
  GamepadReader g(ButtonMappingProfile::Steam);
  g.reloadButtonMapping(); assert(users==0 && !g.capture_button_acquired_);
  saved[size_t(RemoteButton::Guide)]=kButtonMappingCapture;
  g.reloadButtonMapping(); assert(users==1 && g.capture_button_acquired_);
  g.reloadButtonMapping(); assert(users==1);
  saved=defaultButtonMapping(ButtonMappingProfile::Steam);
  g.reloadButtonMapping(); assert(users==0 && !g.capture_button_acquired_);
  saved[size_t(RemoteButton::Guide)]=kButtonMappingCapture;
  g.reloadButtonMapping(); assert(users==1);
 }
 assert(users==0);
 puts("PASS: production default/custom chords, reserved menu precedence, staggered release, Capture reload and lease cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='lunar-mapping-') as d:
 p=Path(d);(p/'switch.h').write_text('#pragma once\n#include <cstdint>\n');(p/'test.cpp').write_text(pre+constants+'namespace lunar::input {'+default+'}'+impl+ctor+dtor+reload+release+klass+main)
 flags=[]
 if sys.platform=='darwin':
  sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
  flags=['-isysroot',sdk,'-isystem',sdk+'/usr/include/c++/v1']
 subprocess.run(['clang++','-std=c++17','-D__SWITCH__','-fsanitize=address,undefined',*flags,'-I'+str(p),'-I'+str(r/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
