from pathlib import Path
import sys,struct,bisect,subprocess,json,re
D=Path(__file__).resolve().parent;W=D.parents[1];ROOT=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else W
src=(ROOT/'app/src/main/cpp/targets/home/home_layout_hooks.cpp').read_text(encoding='utf8')
if 'bool bind_folder_layout()' not in src:
 print('FOLDER_LAYOUT=legacy baseline; title/width/padding native transport absent; NEW_FEATURE_TEST=FAIL');sys.exit(1)
sys.path.insert(0,str(W/'tests/home-layout-native'));from dart_dump import Elf,Symbols
R=Path('C:/Users/XiaoYe/.codex/visualizations/2026/09/29/01a0eb2b-4de7-7442-83d8-db3197b7610f');e=Elf(R/'launcher-7722-libapp.so');s=Symbols(R/'launcher-7722-mini.elf');m=Elf(R/'launcher-7722-mini.elf');sec=m.sections[m.section_names['.symtab']];vas=sorted(set(v for _,t,_,_,v,_ in (struct.unpack_from('<IBBHQQ',m.data,a) for a in range(sec[4],sec[4]+sec[5],24)) if t&15==2 and v))
def fun(source,name):
 a=source.index(name);a=source.rfind('\n',0,a)+1;b=source.index('{',a);dep=1;j=b+1
 while dep:dep+=(source[j]=='{')-(source[j]=='}');j+=1
 return source[a:j]
names=['FolderGridViewGetxController.calGridWidth','FolderGridViewGetxController.folderCellWidth','FolderGridViewGetxController.folderGridOuterHorizontalPadding','FolderGridViewGetxController.folderGridPaddingLeft','FolderHeaderWidget._buildText','FolderHeaderWidget._buildEditor','FolderClingWidget.getFolderClingWidth','FolderClingGetxController._calcFolderPaddingTop','_FlutterTextViewState._resolveEffectiveTextAlign','GridController.currentConfig','RxObjectMixin.value','AndroidAttributeUtils.convertGravity','Stack.updateRenderObject','_encodeParagraphStyle','AndroidAttributeUtils.convertTextAlignment','FolderHeaderWidget.build','CrossAxisAlignment._getChildCrossAxisOffset','Flex.updateRenderObject']
pre=r'''
#include <vector>
#include <map>
#include <string>
#include <span>
#include <array>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <optional>
#include <cmath>
#include "home_layout_config.h"
#include "home_dart_fields.h"
#include "home_widget_move.h"
#include "home_gadget_bridge.h"
std::map<uint32_t,std::vector<uint32_t>> images;std::map<std::string,std::pair<uint32_t,uint32_t>> symbols;
bool dart_function_words(uint32_t va,uint32_t n,std::vector<uint32_t>&out){if(!images.count(va)||images[va].size()*4!=n)return false;out=images[va];return true;}
bool production_accept(const char*,uint32_t,uint32_t);
namespace hometweaks{bool HomeTweaksFindSymbol(const char*n,uint32_t*v,uint32_t*z){if(!symbols.count(n))return false;auto p=symbols[n];if(!production_accept(n,p.first,p.second))return false;*v=p.first;*z=p.second;return true;}}
int bl_imm_words(uint32_t w){int n=int(w&0x3ffffff);if(n&0x2000000)n-=0x4000000;return n;}
'''
registry_source=(ROOT/'app/src/main/cpp/targets/home/tweaks/symtab.cpp').read_text(encoding='utf8')
pre+='struct Segment{uintptr_t begin,end;uint32_t flags;};struct Image{uintptr_t base;Segment segments[16];size_t segmentCount;};constexpr uint32_t kMaxFunctionBytes=0x4000;\n'
for name in ['bool NameEquals(', 'bool VaInExecSegment(', 'bool LooksLikeDartFunction(']:pre+=fun(registry_source,name)+'\n'
pre+='bool production_accept(const char*n,uint32_t va,uint32_t size){if(!images.count(va))return false;auto&b=images[va];uintptr_t begin=(uintptr_t)b.data();Image im{};im.base=begin-va;im.segmentCount=1;im.segments[0]={begin,begin+b.size()*4,1};return LooksLikeDartFunction(im,va,n,size); }\n'
for n in ['bool is_ldur_double(', 'bool is_ldur_word(', 'int imm9(', 'uint32_t rn(', 'uint32_t rt(', 'bool bl_target(uint32_t word, uint32_t pc, uint32_t *target) {']:pre+=fun(src,n)+'\n'
pre+='namespace dartscan { bool function_span(uint32_t va,uint32_t*out){if(!images.count(va))return false;*out=images[va].size()*4;return true;}\n'
for n in ['bool unique_sequence(', 'bool body(uint32_t va', 'bool site(uint32_t va, std::span', 'bool site(uint32_t va, std::initializer_list']:pre+=fun(src,n)+'\n'
pre+='}\nconstexpr uint32_t kDartPrologue=0xa9bf79fd;constexpr int ANDROID_LOG_INFO=4;const char*kTag="test";int __android_log_print(int,const char*,const char*,...){return 0;}\n'
pre+='namespace nhk{struct CodeSource{};}using Words=std::array<uint32_t,4>;struct Slot{uintptr_t address=0;void*replacement=nullptr;void**original=nullptr;nhk::CodeSource source;Words original_words{};bool registered=false;};\nstruct Dart{uintptr_t load_base=0x100000000;};Dart data;Dart*g_dart=&data;Slot g_slots[8];constexpr int kFolderLayoutSlotBase=0;\n'
pre+='void*hc_folder_layout_original[8]{};uint64_t hc_folder_layout_requested=3;uint32_t hc_folder_layout_ready=0;bool g_folder_layout_bound=false,g_folder_layout_checked=false;int g_folder_cell_width_field=-1,g_folder_gap_field=-1,g_folder_screen_width_field=-1,g_folder_screen_height_field=-1,g_folder_cling_width_field=-1,g_folder_controller_config_field=-1,g_folder_rx_value_field=-1,g_folder_enum_index_field=-1;uint32_t g_folder_center_pool=0,g_folder_cross_center_pool=0,g_folder_alignment_start_pool=0;\n'
for i in range(8):pre+=f'void hc_folder_layout_{i}_entry(){{}}\n'
pre+='bool bind_dart_target(uint32_t va,uintptr_t&a,nhk::CodeSource&,Words&w){for(auto&[v,b]:images)if(va>=v&&!((va-v)&3)&&(va-v)/4+4<=b.size()){a=data.load_base+va;std::copy_n(b.begin()+(va-v)/4,4,w.begin());return true;}return false;}\n'
pre+=fun(src,'bool folder_layout_anchors(')+'\n'+fun(src,'bool bind_folder_layout()')+'\n'
# Test the actual callbacks, not a separately reimplemented width/alignment formula.
a=src.index('static thread_local double folder_layout_gap');b=src.index('extern "C" uint64_t hc_title_custom_label(',a);pre+=src[a:b]
cs=(ROOT/'app/src/main/cpp/targets/home/home_layout_config.cpp').read_text(encoding='utf8');a=cs.index("constexpr char kCacheMagic");b=cs.index('void write_config_cache(',a);pre+='namespace home_layout {\n'+cs[a:b]+'\n}\n'
pre+='int checks=0,failed=0;void ok(bool v,const char*n){++checks;if(!v){++failed;std::cout<<"FAIL "<<n<<"\\n";}}\nint main(){\n'
for n in names:
 v,z=s.by_name[n];span=vas[bisect.bisect_right(vas,v)]-v;words=struct.unpack('<'+'I'*(span//4),e.read(v,span));pre+=f'symbols["{n}"]={{{v},{z}}};images[{v}]={{'+','.join(hex(w) for w in words)+'};\n'
registry=(ROOT/'app/src/main/cpp/targets/home/tweaks/symtab.cpp').read_text(encoding='utf8')
pairs=set(re.findall(r'\{\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"',registry))
registered=all((n,n) in pairs for n in names)
pre+='ok('+('true' if registered else 'false')+',"required semantic roots registered in production kTargets");\n'
pre+=r'''
using namespace home_layout;
ok(bind_folder_layout(),"actual 7722 complete bank admitted");
ok(g_folder_cell_width_field==15&&g_folder_gap_field==67&&g_folder_screen_width_field==11&&g_folder_screen_height_field==19&&g_folder_cling_width_field==175,"owning width/spacing fields decoded");
ok(g_folder_controller_config_field==43&&g_folder_rx_value_field==19,"controller and Rx payload not confused with GridConfig");
ok(g_folder_center_pool==0x5eb0&&g_folder_cross_center_pool==0x8180&&g_folder_alignment_start_pool==0x25088,"TextAlign center and outer Stack Alignment center independently decoded");
for(auto &slot:g_slots)ok(slot.address&&slot.original,"eight replay windows bound");
ok(g_slots[7].address==data.load_base+0x146eac0,"slot7 belongs to outer FolderHeader.build alignment store, not nested Stack clip");
ok(g_slots[7].original_words[0]==0xb800f001&&g_slots[7].original_words[3]==0xb8017001,"alignment replay preserves StackFit and Clip fields");
auto old=images;
for(auto&[va,b]:images){
 for(size_t i=4;i<b.size();++i)if((b[i]&0xfc000000)==0x94000000)b[i]=(b[i]&0xfc000000)|((b[i]-4)&0x3ffffff);
 b.insert(b.begin()+4,4,0xd503201f);
}
g_folder_layout_bound=false;g_folder_layout_checked=false;ok(bind_folder_layout(),"shifted code resolves without old offsets");
images=old;
// Include the actual production leaf/prologue filter, not just a synthetic symbol map.
auto leafva=symbols["AndroidAttributeUtils.convertTextAlignment"].first;
auto leafsize=symbols["AndroidAttributeUtils.convertTextAlignment"].second;
ok(production_accept("AndroidAttributeUtils.convertTextAlignment",leafva,leafsize),"production resolver admits actual read-only leaf ABI");
ok(!production_accept("Unrelated.leaf",leafva,leafsize),"no arbitrary leaf admission");
ok(!production_accept("AndroidAttributeUtils.convertTextAlignment",leafva,8),"short leaf rejected before third instruction");
for(size_t k=0;k<3;++k){images[leafva][k]^=1;ok(!production_accept("AndroidAttributeUtils.convertTextAlignment",leafva,leafsize),"foreign leaf dispatch opcode rejected");images=old;}
// An independent named converter must identify the same TextAlign.center root.
uint32_t converter=symbols["AndroidAttributeUtils.convertTextAlignment"].first;
auto &cb=images[converter];
size_t center_load=0;
for(size_t j=0;j+3<cb.size();++j)if(cb[j]==0xf100107f&&cb[j+2]==0xf96f5b60)center_load=j+2;
ok(center_load>0,"independent center branch found");
cb[center_load]+=0x400; // Change only its pool displacement.
g_folder_layout_bound=g_folder_layout_checked=false;
ok(!bind_folder_layout(),"disagreeing converter center declines whole bank");images=old;
uint32_t resolver=symbols["_FlutterTextViewState._resolveEffectiveTextAlign"].first;
auto &rb=images[resolver];
for(size_t j=0;j+3<rb.size();++j)if(rb[j]==0x7100043f)rb[j]=0x7100143f;
g_folder_layout_bound=g_folder_layout_checked=false;
ok(!bind_folder_layout(),"RIGHT-only branch never admitted as center");images=old;
// Change both owning center pool loads: the current symbol is followed, not a fixed offset.
for(auto v:{resolver,converter}){
 auto &body=images[v];
 for(size_t j=0;j+3<body.size();++j)
  if(body[j]==(v==resolver?0x7100043fu:0xf100107fu))body[j+2]+=0x800;
}
g_folder_layout_bound=g_folder_layout_checked=false;
ok(bind_folder_layout()&&g_folder_center_pool==0x5ec0,"center pool displacement relocates with both semantic branches");images=old;
// Admission must also reject duplicate semantic branches and a foreign common join.
images[converter].insert(images[converter].end(),old[converter].begin(),old[converter].end());
g_folder_layout_bound=g_folder_layout_checked=false;
ok(!bind_folder_layout(),"duplicate converter center branch rejects ambiguity");images=old;
for(size_t j=0;j+3<images[resolver].size();++j)if(images[resolver][j]==0x7100043f){images[resolver][j+3]=0x14000001;break;}
g_folder_layout_bound=g_folder_layout_checked=false;
ok(!bind_folder_layout(),"foreign gravity join does not resolve center");images=old;
for(size_t j=0;j+3<images[converter].size();++j)if(images[converter][j]==0xf100107f){images[converter][j+1]^=1;break;}
g_folder_layout_bound=g_folder_layout_checked=false;
ok(!bind_folder_layout(),"inverted converter selection rejected");images=old;
uint32_t rp=0;ok(folder_center_pool(images[resolver],rp)&&rp==0x5eb0,"gravity1 selects center rather than gravity5");
g_folder_layout_bound=g_folder_layout_checked=false;ok(bind_folder_layout(),"original semantic root restored");
auto va=symbols["FolderGridViewGetxController.folderGridOuterHorizontalPadding"].first;
auto original=images[va];images[va].insert(images[va].end(),original.begin()+0x90/4,original.begin()+0xa0/4);
g_folder_layout_bound=false;g_folder_layout_checked=false;ok(!bind_folder_layout(),"ambiguous return store rejected atomically");images=old;
va=symbols["FolderGridViewGetxController.folderCellWidth"].first;
images[va][0x58/4]^=1;g_folder_layout_bound=false;g_folder_layout_checked=false;ok(!bind_folder_layout(),"foreign currentConfig producer rejected");images=old;
va=symbols["FolderGridViewGetxController.folderGridPaddingLeft"].first;
images[va][0xac/4]^=1;g_folder_layout_bound=false;g_folder_layout_checked=false;ok(!bind_folder_layout(),"GC rejoin into wrong instruction rejected");images=old;
va=symbols["RxObjectMixin.value"].first;
images[va][0x40/4]^=1;g_folder_layout_bound=false;g_folder_layout_checked=false;ok(!bind_folder_layout(),"RX payload change rejected");images=old;
g_folder_layout_bound=false;g_folder_layout_checked=false;ok(bind_folder_layout(),"bank restored after fault injection");
FolderLayoutConfig f{1,1,1,20,120,60,0};auto packed=pack_folder_layout(f,4);
ok(unpack_folder_layout(packed)==f,"one atomic snapshot roundtrip");
ok(std::abs(folder_cell_width(90,400,900,4,packed)-87)<1e-9,"phone side padding and four columns");
f.padding_enabled=0;ok(std::abs(folder_cell_width(90,400,900,4,pack_folder_layout(f,4))-97)<1e-9,"width gate independent of padding");
f.full_width=0;ok(folder_cell_width(90,400,900,4,pack_folder_layout(f,4))==90,"switch off exact original width");
f={0,1,1,20,120,60,1};
ok(std::abs(folder_cell_width(90,1000,700,4,pack_folder_layout(f,4))-187)<1e-9,"tablet landscape uses h slider");
ok(std::abs(folder_cell_width(90,700,1000,4,pack_folder_layout(f,4))-142)<1e-9,"tablet portrait uses v slider");
f.landscape_padding=450;ok(folder_cell_width(90,700,600,4,pack_folder_layout(f,6))>=24,"extreme padding retains positive cells");
f.phone_padding=51;ok(!valid_folder_layout(f)&&!pack_folder_layout(f,4),"out of range values refused");
f={1,1,1,20,120,60,0};std::vector<int32_t> packet={kFolderLayoutMagic,1,1,1,20,120,60,0};size_t at=0;FolderLayoutConfig output;
auto read=[&](int32_t&n){if(at>=packet.size())return false;n=packet[at++];return true;};
ok(read_folder_layout(read,output)&&output==f,"Binder extension roundtrip");
for(size_t size=1;size<8;++size){packet.resize(size);at=0;output=f;ok(!read_folder_layout(read,output)&&output==f,"partial extension never overwrites snapshot");}
packet.clear();at=0;output=f;ok(read_folder_layout(read,output)&&output==FolderLayoutConfig{},"legacy endpoint uses stock defaults");
Config config;config.cell_x=4;config.cell_y=6;config.folder=f;config.widget_allow_move=true;
std::vector<uint8_t> cache;serialize_config(config,cache);Config restored;
ok(parse_config(cache,restored)&&restored.folder==f&&restored.widget_allow_move,"version10 cache roundtrip");
auto legacy=cache;legacy.resize(legacy.size()-32);legacy[4]=9;restored.folder=f;
ok(parse_config(legacy,restored)&&restored.folder==FolderLayoutConfig{}&&restored.widget_allow_move,"version9 cache compatibility preserves existing widget setting");
cache.pop_back();ok(!parse_config(cache,restored),"truncated v10 rejected");
alignas(8) unsigned char buffer[2048]{};uintptr_t saved=(uintptr_t)buffer, controller=(uintptr_t)buffer+800+1, root=(uintptr_t)buffer+1000+1, child=(uintptr_t)buffer+1200+1;
auto set64=[&](uintptr_t addr,uint64_t v){std::memcpy((void*)addr,&v,8);};
auto setd=[&](uintptr_t addr,double v){std::memcpy((void*)addr,&v,8);};
auto getd=[&](uintptr_t addr){double v=0;std::memcpy(&v,(void*)addr,8);return v;};
hc_folder_layout_requested=pack_folder_layout(f,4);
set64(saved+8,controller);setd(controller+g_folder_gap_field,4);hc_folder_layout_body(saved,0,0,0);
set64(saved,root);set64(saved+8,child);setd(root+g_folder_screen_width_field,400);setd(root+g_folder_screen_height_field,900);setd(child+g_folder_cell_width_field,90);
hc_folder_layout_body(saved,0,1,0);ok(getd(saved+160)==87,"production callback computes fresh cell size");
hc_folder_layout_body(saved,0,1,0);ok(getd(saved+160)==87,"repeat does not compound original config");
ok(getd(child+g_folder_cell_width_field)==90&&getd(root+g_folder_screen_width_field)==400,"shared GridConfig never modified");
setd(saved+160,12);hc_folder_layout_body(saved,0,2,0);ok(getd(saved+160)==0,"no extra outer padding on wide grid");
uint64_t heap=controller&~uint64_t(0xffffffff);uintptr_t rx=(uintptr_t)buffer+1400+1;
uint32_t crx=uint32_t(rx),croot=uint32_t(root);std::memcpy((void*)(controller+g_folder_controller_config_field),&crx,4);std::memcpy((void*)(rx+g_folder_rx_value_field),&croot,4);
set64(saved,controller);setd(controller+g_folder_cling_width_field,300);hc_folder_layout_body(saved,0,6,heap);ok(getd(saved+160)==400,"tablet popup expands through original controller RX chain");
hc_folder_layout_requested=0;set64(saved,root);set64(saved+8,child);hc_folder_layout_body(saved,0,1,0);ok(getd(saved+160)==90,"production callback switch off original scalar");
hc_folder_layout_requested=pack_folder_layout(f,4);
uintptr_t start=(uintptr_t)buffer+1504+1,center=(uintptr_t)buffer+1600+1,cross_start=(uintptr_t)buffer+1696+1,cross_center=(uintptr_t)buffer+1792+1;
std::vector<uint64_t> pool((0x67580+8)/8+1);
set64(start-1,uint64_t(1234)<<12);set64(center-1,uint64_t(1234)<<12);set64(start+7,4);set64(center+7,2);
pool[g_folder_center_pool/8]=center;set64(saved+8,start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),4,0);
ok(*(uint64_t*)(saved+8)==center,"Text uses original center enum");
set64(saved+8,start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),5,0);
ok(*(uint64_t*)(saved+8)==center,"rename TextField uses same alignment");
set64(cross_start-1,uint64_t(2345)<<12);set64(cross_center-1,uint64_t(3456)<<12);setd(cross_start+7,-1);setd(cross_start+15,-1);setd(cross_center+7,0);setd(cross_center+15,0);pool[g_folder_alignment_start_pool/8]=cross_start;
pool[g_folder_cross_center_pool/8]=cross_center;set64(saved+8,cross_start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),7,0);
ok(*(uint64_t*)(saved+8)==cross_center,"outer Stack alignment centers intrinsic text without changing Clip");
setd(cross_center+15,1);set64(saved+8,cross_start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),7,0);
ok(*(uint64_t*)(saved+8)==cross_start,"wrong alignment center coordinates preserve original");setd(cross_center+15,0);
set64(saved+8,start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),7,0);
ok(*(uint64_t*)(saved+8)==start,"foreign enum never treated as Stack alignment");
hc_folder_layout_requested=0;set64(saved+8,cross_start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),7,0);
ok(*(uint64_t*)(saved+8)==cross_start,"disabled Stack centering exact original");hc_folder_layout_requested=pack_folder_layout(f,4);
ok(*(uint64_t*)(start+7)==4&&getd(cross_start+7)==-1,"pooled original enums are immutable");
set64(center+7,3);set64(saved+8,start);hc_folder_layout_body(saved,(uintptr_t)pool.data(),4,0);
ok(*(uint64_t*)(saved+8)==start,"wrong enum index retains stock text");
set64(center+7,2);set64(center-1,uint64_t(4321)<<12);hc_folder_layout_body(saved,(uintptr_t)pool.data(),4,0);
ok(*(uint64_t*)(saved+8)==start,"wrong enum class retained");
hc_folder_layout_requested=0;hc_folder_layout_body(saved,(uintptr_t)pool.data(),4,0);ok(*(uint64_t*)(saved+8)==start,"title default exact original");
g_folder_layout_bound=g_folder_layout_checked=false;ok(!bind_folder_layout()&&!g_folder_layout_checked,"disabled folder features never scan APK");
std::cout<<"FOLDER_CENTER_NATIVE="<<checks<<" checks; failed="<<failed<<"\n";return failed?1:0;}
'''
out=D/('run-'+ROOT.name);out.mkdir(exist_ok=True);(out/'test.cpp').write_text(pre,encoding='utf8')
zig=W/'tests/dynamic-address-audit/runtime/zig-windows-x86_64-0.13.0/zig.exe'
r=subprocess.run([str(zig),'c++','-std=c++20','-O0','-I'+str(ROOT/'app/src/main/cpp/targets/home'),str(out/'test.cpp'),'-o',str(out/'test.exe')],capture_output=True,text=True,encoding='utf8');print(r.stdout+r.stderr,end='')
if r.returncode:sys.exit(r.returncode)
r=subprocess.run([str(out/'test.exe')],capture_output=True,text=True,encoding='utf8');print(r.stdout+r.stderr,end='');sys.exit(r.returncode)
