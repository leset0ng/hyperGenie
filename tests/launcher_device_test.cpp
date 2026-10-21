// SPDX-License-Identifier: Apache-2.0
// Actual Launcher hooks with fake parameter trees and retained capabilities.
#include <android/surface_control.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <memory>
static int alpha_writes=0;
static ASurfaceControl* suppressed=nullptr;
static void ObserveAlpha(ASurfaceTransaction*,ASurfaceControl* surface,float value) {
    if(value!=0) std::abort();
    ++alpha_writes;suppressed=surface;
}
#define ASurfaceTransaction_setBufferAlpha ObserveAlpha
#include "../app/src/main/cpp/hha_native.cpp"
#undef ASurfaceTransaction_setBufferAlpha
namespace {
int assertions=0, prepared=0, retained=0, released=0, queued=0, cancelled=0, local_cancels=0;
bool accept=true,reenter_replace=false;
void Check(bool yes,const char* text) {++assertions;if(!yes){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
struct Control {int id;std::string name;};
Control app{11,"Task=7#11"},icon{22,"FloatingIcon_all#22"},child{23,"[BBQ] FloatingIcon_all#22"},new_child{24,"[BBQ] FloatingIcon_all#22"},desktop{25,"DesktopIcon"};
int32_t Layer(const void* pointer){return static_cast<const Control*>(pointer)->id;}
const std::string& Name(const void* pointer){return static_cast<const Control*>(pointer)->name;}
struct Param {
    struct Node {std::array<unsigned char,0x48> bytes{};std::array<unsigned char,0x30> property{};};
    std::array<unsigned char,0x60> object{};
    std::vector<std::unique_ptr<Node>> nodes;
    const unsigned char* ref=object.data();
    void Add(const char* key,float start,float end,float velocity=0,float delay=0) {
        auto storage=std::make_unique<Node>();auto* node=storage->bytes.data();
        node[0x20]=std::strlen(key)*2;std::memcpy(node+0x21,key,std::strlen(key));
        auto* prop=storage->property.data();std::memcpy(node+0x38,&prop,sizeof(prop));
        std::memcpy(prop+0x10,&start,4);std::memcpy(prop+0x14,&end,4);
        std::memcpy(prop+0x18,&delay,4);std::memcpy(prop+0x1c,&velocity,4);
        unsigned char** link=reinterpret_cast<unsigned char**>(object.data()+0x18);
        while(*link) {
            const auto* text=reinterpret_cast<const char*>(*link+0x21);
            const size_t size=(*link)[0x20]>>1;
            const std::string current(text,size);
            link=reinterpret_cast<unsigned char**>(*link+(std::string(key)>current?8:0));
        }
        *link=node;nodes.push_back(std::move(storage));
    }
    explicit Param(int type=1) {
        int id=77;std::memcpy(object.data()+0x40,&type,4);std::memcpy(object.data()+0x48,&id,4);
        Add("centerX",165.5f,560,21,.02f);Add("centerY",885.5f,1218,37,.03f);
        Add("width",171,1120,55,.04f);Add("height",171,2436,999,.9f);
        Add("ratio",1,2436.f/1120,9,.8f);Add("alpha",1,1,9,.7f);
        Add("radius",40,100,5,.6f);Add("radiusRatio",0,0);
        Add("cornerTLX",.2f,.1f,4,.5f);
    }
    const void* Reference() {return &ref;}
};
uint64_t TransactionId(void* transaction){return reinterpret_cast<uintptr_t>(transaction);}
void* StockStop(void* transaction,const void*,int32_t){return transaction;}
void Reset() {
    g_last={};g_icons={};g_anchors={};g_next=0;g_next_icon=0;
    g_next_config=Now()+1000000000000LL;g_config={true,true,false};
    g_layer=Layer;g_name=Name;g_stop=StockStop;g_transaction_id=TransactionId;
    alpha_writes=prepared=retained=released=queued=cancelled=local_cancels=0;
    suppressed=nullptr;accept=true;reenter_replace=false;
}
void Seed(Param& source) {
    const void* control=&app;Observe(&control,source.Reference(),false);
    Check(g_last[0].valid,"source observed");
    Check(g_last[0].host_velocity==std::array<float,3>{21,37,55},"source velocities preserved");
    Check(g_last[0].host_delay==std::array<float,3>{.02f,.03f,.04f},"source delays preserved");
    Check(g_last[0].host_transaction_id==0,"new source resets previous host transaction proof");
    TuneParent(reinterpret_cast<void*>(0x1234),&control,source.Reference());
    Check(g_last[0].host_transaction_id==0x1234,"successful App tune records exact host transaction before stock start");
}
void AddChild(){g_icons[0].surface=&child;g_icons[0].parent=icon.id;g_icons[0].width=171;g_icons[0].height=171;}
}
namespace hha {
void QueueTransition(const Transition&){++queued;}
void QueueCancel(int32_t,int64_t){++cancelled;}
bool WarmIconRenderer(){return true;}
void RetainIconSurface(void*,const void*){++retained;}
void ReleaseIconSurface(void* surface,const void*){if(surface)++released;}
void CancelIconRenderer(){++local_cancels;}
bool PrepareOpeningPreview(void*,void*,int,int,const Transition&){return true;}
bool PrepareContentGroup(void*,void*,genie::Rect*,const Transition&){return true;}
bool PrepareIcon(void*,void* surface,void* parent,int tw,int th,int ww,int wh,const Transition&) {
    ++prepared;Check(surface==&child&&parent==&icon,"central registration uses exact retained capabilities");
    Check(tw==171&&th==171&&ww==1120&&wh==2436,"central registration preserves texture/window domains");
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"no launcher registry lock crosses Binder");
    if(unlocked)g_mutex.unlock();Check(retained==released+1,"independent lease held during RPC");
    if(reenter_replace){std::lock_guard<std::mutex> lock(g_mutex);g_icons[0].surface=&new_child;g_icons[0].request_ns=INT64_MAX;}
    return accept;
}
}
int main(){
    Reset();Param source(2);Seed(source);AddChild();
    const void* app_control=&app;
    Param target;const void* icon_control=&icon;TuneParent(reinterpret_cast<void*>(0x1234),&icon_control,target.Reference());
    Check(prepared==1&&retained==released,"registration owns a balanced lease");
    for(const char* key:{"centerX","centerY","width","height","ratio","radius","cornerTLX"}) {
        const auto a=Spring(source.Reference(),key),b=Spring(target.Reference(),key);
        Check(a.start==b.start&&a.end==b.end&&a.velocity==b.velocity&&a.delay==b.delay,"App/icon host springs match");
    }
    Check(g_icons[0].animation_id==77&&g_icons[0].request_ns==g_last[0].transition.event_ns,"registered icon retains exact epoch");
    const int64_t epoch=g_icons[0].request_ns;Stop(nullptr,&icon_control,77);
    Check(cancelled==0&&local_cancels==0&&g_icons[0].request_ns==epoch,"icon stock completion leaves central owner alive");
    Stop(nullptr,&app_control,77);Check(cancelled==1&&!g_last[0].valid,"App stop invalidates source and queues central cancel");
    Reset();Param rejected_source(2);Seed(rejected_source);AddChild();accept=false;
    Param rejected;TuneParent(reinterpret_cast<void*>(0x1234),&icon_control,rejected.Reference());
    const auto alpha=Spring(rejected.Reference(),"alpha");
    Check(alpha.start==0&&alpha.end==0&&alpha.velocity==0&&alpha.delay==0,"rejected clone cannot regain stock alpha");
    Check(alpha_writes==1&&suppressed==reinterpret_cast<ASurfaceControl*>(&icon),"initial host transaction also suppresses clone");
    Check(Spring(rejected.Reference(),"width").start==171,"suppression preserves OEM geometry lifecycle");
    Check(retained==released,"rejected RPC releases lease");
    Reset();Param missing_source(2);Seed(missing_source);Param missing;
    TuneParent(reinterpret_cast<void*>(0x1234),&icon_control,missing.Reference());
    Check(prepared==0&&alpha_writes==1,"missing child suppresses temporary clone");
    Param actual_desktop;const void* desktop_control=&desktop;
    TuneParent(reinterpret_cast<void*>(0x1234),&desktop_control,actual_desktop.Reference());
    Check(alpha_writes==1&&Spring(actual_desktop.Reference(),"alpha").start==1,"desktop source icon is never suppressed");
    Reset();Param race_source(2);Seed(race_source);AddChild();reenter_replace=true;Param race;
    TuneParent(reinterpret_cast<void*>(0x1234),&icon_control,race.Reference());
    Check(g_icons[0].surface==&new_child&&g_icons[0].request_ns==INT64_MAX,"nested replacement cannot be retagged by old RPC");
    Check(retained==released,"replacement preserves lease balance");
    Reset();Param late_source(2);Seed(late_source);AddChild();Param late;
    TuneParent(reinterpret_cast<void*>(0x5678),&icon_control,late.Reference());
    Check(prepared==1&&alpha_writes==0&&retained==released,"separate OEM construction transactions still register the shared icon motion");
    Reset();Param unavailable_source(2);Seed(unavailable_source);AddChild();g_transaction_id=nullptr;Param unavailable;
    TuneParent(reinterpret_cast<void*>(0x1234),&icon_control,unavailable.Reference());
    Check(prepared==1&&alpha_writes==0&&retained==released,"optional transaction diagnostics never disable the shared icon animation");
    std::printf("PASS %d Launcher central-icon integration checks\n",assertions);
}
