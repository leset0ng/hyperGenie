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
int preview_prepared=0,group_prepared=0;
bool accept=true,reenter_replace=false,accept_preview=true,accept_group=true,defer_group=false;
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
    explicit Param(int type=1,int id=77) {
        const int adjust_type=type==-1?1:0;
        std::memcpy(object.data()+0x40,&type,4);std::memcpy(object.data()+0x44,&adjust_type,4);
        std::memcpy(object.data()+0x48,&id,4);
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
    preview_prepared=group_prepared=0;accept_preview=accept_group=true;defer_group=false;
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
Control prestart{31,"Pre startup opening leash#31"};
void AnchorPrestart(){g_anchors[0]={prestart.id,app.id,7};}
void Reverse(Param& parameters) {
    for(const char* key:{"centerX","centerY","width","height","ratio","radius","cornerTLX"}) {
        const auto spring=Spring(parameters.Reference(),key);
        SetSpring(parameters.Reference(),key,spring.end,spring.start);
    }
}
std::vector<std::array<unsigned char,0x30>> Properties(const Param& parameters) {
    std::vector<std::array<unsigned char,0x30>> result;
    for(const auto& node:parameters.nodes) result.push_back(node->property);
    return result;
}
void CheckStock(const Param& parameters) {
    const void* reference=&parameters.ref;
    Check(Spring(reference,"ratio").start!=Spring(reference,"ratio").end,"rejected preparation preserves OEM aspect animation");
    Check(g_last[0].host_transaction_id==0&&!g_last[0].supported,"rejected source cannot publish a custom viewport");
}
}
namespace hha {
void QueueTransition(const Transition&){++queued;}
void QueueCancel(int32_t,int64_t){++cancelled;}
bool WarmIconRenderer(){return true;}
void RetainIconSurface(void*,const void*){++retained;}
void ReleaseIconSurface(void* surface,const void*){if(surface)++released;}
void CancelIconRenderer(){++local_cancels;}
bool PrepareOpeningPreview(void*,void*,int,int,const Transition& transition) {
    ++preview_prepared;
    return accept_preview&&!transition.closing;
}
GroupPreparation PrepareContentGroupStatus(void*,void*,genie::Rect*,const Transition& transition) {
    ++group_prepared;
    if(transition.task_id<0) return GroupPreparation::Rejected;
    if(accept_group) return GroupPreparation::Ready;
    return defer_group ? GroupPreparation::Deferred : GroupPreparation::Rejected;
}
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
    // Reused pre-start leashes have a cached Task even though their name still
    // identifies an opening preview. Successful preview publication is enough.
    Reset();AnchorPrestart();accept_group=false;Param anchored_open(2);
    const void* prestart_control=&prestart;
    Observe(&prestart_control,anchored_open.Reference(),false);
    Check(g_last[0].transition.task_id==7&&queued==0,"anchored opening waits for its host transaction");
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,anchored_open.Reference());
    Check(preview_prepared==1&&group_prepared==0,"anchored opening selects only preview preparation");
    Check(queued==1&&g_last[0].supported&&g_last[0].host_transaction_id==0x1234,"accepted preview publishes custom host despite unavailable group");
    AddChild();Param anchored_icon;
    TuneParent(reinterpret_cast<void*>(0x5678),&icon_control,anchored_icon.Reference());
    Check(prepared==1&&alpha_writes==0,"accepted preview remains eligible for shared icon publication");

    // A close on that exact reused leash must use grouped live content, even
    // when the opening-only preview capability is unavailable.
    accept_preview=false;accept_group=true;Param reused_close(2);Reverse(reused_close);
    Observe(&prestart_control,reused_close.Reference(),false);
    Check(g_last[0].transition.closing&&queued==1,"reused close waits for grouped first-frame preparation");
    TuneParent(reinterpret_cast<void*>(0x6789),&prestart_control,reused_close.Reference());
    Check(preview_prepared==1&&group_prepared==1,"reused close bypasses opening preview and prepares live group");
    Check(queued==2&&g_last[0].supported&&g_last[0].host_transaction_id==0x6789,"reused close publishes its own custom host epoch");
    Param closing_icon;Reverse(closing_icon);
    TuneParent(reinterpret_cast<void*>(0x789a),&icon_control,closing_icon.Reference());
    Check(prepared==2&&alpha_writes==0,"reused close keeps paired icon on the custom route");

    Reset();AnchorPrestart();accept_preview=false;Param rejected_preview(2);
    Observe(&prestart_control,rejected_preview.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,rejected_preview.Reference());
    Check(preview_prepared==1&&group_prepared==0&&queued==0,"failed preview cannot fall through to another publication route");
    CheckStock(rejected_preview);

    Reset();AnchorPrestart();accept_group=false;Param rejected_group(2);Reverse(rejected_group);
    Observe(&prestart_control,rejected_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,rejected_group.Reference());
    Check(preview_prepared==0&&group_prepared==1&&queued==0,"failed closing group never attempts opening preview or publishes geometry");
    CheckStock(rejected_group);

    Reset();Param unanchored_close(2);Reverse(unanchored_close);
    Observe(&prestart_control,unanchored_close.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,unanchored_close.Reference());
    Check(preview_prepared==0&&group_prepared==1&&queued==0,"unanchored pre-start close requires a verified group instead of publishing unprepared geometry");
    CheckStock(unanchored_close);

    Reset();Param fresh_preview(2);
    Observe(&prestart_control,fresh_preview.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,fresh_preview.Reference());
    Check(preview_prepared==1&&group_prepared==0&&queued==1,"unanchored opening retains preview-only publication");
    Check(g_last[0].supported&&g_last[0].host_transaction_id==0x1234,"fresh preview publishes its custom host");

    // Adjust already owns a prepared stream; it must not acquire new first-frame
    // prerequisites when a rapid reversal changes direction.
    AnchorPrestart();accept_preview=accept_group=false;Param reversal(2);Reverse(reversal);
    const int next_animation=78;std::memcpy(reversal.object.data()+0x48,&next_animation,4);
    Observe(&prestart_control,reversal.Reference(),true);
    TuneParent(reinterpret_cast<void*>(0x9abc),&prestart_control,reversal.Reference());
    Check(g_last[0].adjusting&&g_last[0].transition.closing,"higher-ID reverse callback retains prepared stream");
    Check(preview_prepared==1&&group_prepared==0&&queued==2,"adjust bypasses both initial preparation routes");
    Check(g_last[0].supported&&g_last[0].host_transaction_id==0x9abc,"adjust still publishes the retargeted viewport");
    Control shell{41,"Surface(name=Task=7#11)/@test_transition-leash#41"};
    const void* shell_control=&shell;
    Reset();accept_group=false;Param not_ready(2);
    Observe(&shell_control,not_ready.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&shell_control,not_ready.Reference());
    Check(group_prepared==1&&queued==0,"unavailable initial group preserves stock host");
    CheckStock(not_ready);
    Param still_not_ready(-1,78);
    Observe(&shell_control,still_not_ready.Reference(),true);
    TuneParent(reinterpret_cast<void*>(0x2345),&shell_control,still_not_ready.Reference());
    Check(group_prepared==2&&queued==0,"unsupported adjust retries preparation without asynchronous publication");
    CheckStock(still_not_ready);
    const auto rejected_epoch=g_last[0].transition.event_ns;
    Param stale(-1,77);Observe(&shell_control,stale.Reference(),true);
    Check(g_last[0].transition.event_ns==rejected_epoch,"stale callback cannot retry or replace a rejected epoch");
    accept_group=true;Param recovered(-1,79);
    Observe(&shell_control,recovered.Reference(),true);
    Check(!g_last[0].adjusting&&queued==0,"recovery adjust requires a fresh first-frame proof");
    TuneParent(reinterpret_cast<void*>(0x3456),&shell_control,recovered.Reference());
    Check(group_prepared==3&&queued==1&&g_last[0].supported&&g_last[0].host_transaction_id==0x3456,
          "ready group recovers the same stream on a newer adjust");
    accept_group=false;Param continuing(-1,80);
    Observe(&shell_control,continuing.Reference(),true);
    TuneParent(reinterpret_cast<void*>(0x4567),&shell_control,continuing.Reference());
    Check(g_last[0].adjusting&&group_prepared==3&&queued==2&&g_last[0].host_transaction_id==0x4567,
          "already prepared adjust keeps its fast continuation path");

    // A later host transaction can repeat the same SF generation without a
    // newer adjust. First-buffer admission must retry that exact request.
    Reset();accept_group=false;defer_group=true;Param pending_group(2);
    Observe(&shell_control,pending_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1111),&shell_control,pending_group.Reference());
    const auto pending_epoch=g_last[0].transition.event_ns;
    const auto pending_geometry=g_last[0].transition.geometry;
    const auto pending_properties=Properties(pending_group);
    Check(!g_last[0].supported&&g_last[0].retry_pending&&!g_last[0].admitted,
          "deferred group remains unadmitted and icon-ineligible");
    Check(!hha::FindIconSource(g_last,77,Now()),"pending group cannot lend custom geometry to the icon");
    Param stale_host(2,76);const auto stale_properties=Properties(stale_host);
    TuneParent(reinterpret_cast<void*>(0x1212),&shell_control,stale_host.Reference());
    Check(group_prepared==1&&Properties(stale_host)==stale_properties,
          "older host callback cannot retry or mutate the pending generation");
    Observe(&shell_control,pending_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1313),&shell_control,pending_group.Reference());
    Check(group_prepared==2&&queued==0&&g_last[0].transition.event_ns==pending_epoch,
          "still-pending same-ID host retry keeps its epoch without dispatch");
    Check(Properties(pending_group)==pending_properties&&g_last[0].transition.geometry==pending_geometry,
          "deferred retries preserve every stock spring and original content geometry");
    accept_group=true;
    TuneParent(reinterpret_cast<void*>(0x2222),&shell_control,pending_group.Reference());
    Check(group_prepared==3&&queued==1&&g_last[0].host_transaction_id==0x2222,
          "same-generation host callback retries ready first-buffer admission");
    Check(g_last[0].transition.event_ns==pending_epoch&&g_last[0].transition.geometry==pending_geometry,
          "same-generation admission preserves original epoch and square icon domain");
    Check(g_last[0].supported&&g_last[0].admitted&&!g_last[0].retry_pending&&!g_last[0].preparing,
          "successful host merge atomically completes pending admission");
    Observe(&shell_control,pending_group.Reference(),false);
    Param repeated_host(2);Observe(&shell_control,repeated_host.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x3333),&shell_control,repeated_host.Reference());
    Check(group_prepared==3&&queued==1&&g_last[0].host_transaction_id==0x3333,
          "admitted same-generation callbacks never duplicate preparation or dispatch");
    Check(g_last[0].transition.event_ns==pending_epoch&&g_last[0].transition.geometry==pending_geometry,
          "rewritten host springs cannot contaminate the retained content request");

    Reset();accept_group=false;Param terminal_group(2);
    Observe(&shell_control,terminal_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1111),&shell_control,terminal_group.Reference());
    accept_group=true;
    TuneParent(reinterpret_cast<void*>(0x2222),&shell_control,terminal_group.Reference());
    Check(group_prepared==1&&queued==0&&!g_last[0].retry_pending,
          "terminal domain rejection cannot become a same-generation retry");
    CheckStock(terminal_group);

    Reset();accept_group=false;defer_group=true;Param expired_group(2);
    Observe(&shell_control,expired_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1111),&shell_control,expired_group.Reference());
    g_last[0].transition.event_ns=Now()-1600000000LL;accept_group=true;
    TuneParent(reinterpret_cast<void*>(0x2222),&shell_control,expired_group.Reference());
    Check(group_prepared==1&&queued==0,"deferred request cannot revive after the render deadline");
    CheckStock(expired_group);
    std::printf("PASS %d Launcher integration checks\n",assertions);
}
