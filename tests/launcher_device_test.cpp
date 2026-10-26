// SPDX-License-Identifier: Apache-2.0
// Actual Launcher hooks with fake parameter trees and retained capabilities.
#include <android/surface_control.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <cstddef>
#include <chrono>
#include <condition_variable>
static int alpha_writes=0;
static ASurfaceControl* suppressed=nullptr;
struct AlphaWrite {ASurfaceTransaction* transaction;ASurfaceControl* surface;float value;};
static std::vector<AlphaWrite> alpha_calls;
static void ObserveAlpha(ASurfaceTransaction* transaction,ASurfaceControl* surface,float value) {
    if(value!=0) std::abort();
    ++alpha_writes;suppressed=surface;alpha_calls.push_back({transaction,surface,value});
}
#define ASurfaceTransaction_setBufferAlpha ObserveAlpha
#include "../app/src/main/cpp/genie_native.cpp"
#undef ASurfaceTransaction_setBufferAlpha
namespace {
int assertions=0, prepared=0, retained=0, released=0, queued=0, cancelled=0, local_cancels=0;
int32_t cancel_layer=-1;int64_t cancel_epoch=0;
std::vector<int> stop_prime_order;
int preview_prepared=0,group_prepared=0;
int parent_retained=0,parent_released=0,home_calls=0,set_callback_calls=0;
int owned_retained=0,owned_released=0,publication_issued=0,publication_released=0;
int clone_calls=0,parameters_issued=0,parameters_released=0;
genie::Transition prepared_transition;
bool accept=true,reenter_replace=false,accept_preview=true,accept_group=true,defer_group=false;
bool accept_clone=true,shallow_clone=false;
genie::PublicationKind publication_kind=genie::PublicationKind::NormalGroup;
void Check(bool yes,const char* text) {++assertions;if(!yes){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
struct Control {
    int id;std::string name;
    std::array<unsigned char,0xa8-8-sizeof(std::string)> padding{};
    void* listener=nullptr;
};
static_assert(offsetof(Control,name)==8&&offsetof(Control,listener)==0xa8);
struct Listener {const void* vtable=nullptr;std::array<unsigned char,0x20> padding{};void* callback=nullptr;};
static_assert(offsetof(Listener,callback)==0x28&&sizeof(Listener)==0x30);
Listener icon_listener,app_listener,other_listener;
struct Callback {const void* vtable=nullptr;};
Callback callback_token;
const std::array<uintptr_t,4> listener_table{1,0,0,0},params_table{2,0,0,0};
struct HomePayload {
    alignas(void*) std::array<unsigned char,0x78> bytes{};
    HomePayload() {
        const uintptr_t marker=reinterpret_cast<uintptr_t>(g_params_vtable);
        std::memcpy(bytes.data(),&marker,sizeof(marker));
        for(int i=0;i<28;++i) {
            const float value=i+.25f;
            std::memcpy(bytes.data()+8+i*4,&value,4);
        }
        const float width=352.72f,height=767.2f,alpha=.76f,velocity=-3.25f;
        std::memcpy(bytes.data()+0x18,&width,4);std::memcpy(bytes.data()+0x20,&height,4);
        std::memcpy(bytes.data()+0x30,&alpha,4);std::memcpy(bytes.data()+0x34,&velocity,4);
    }
};
static_assert(sizeof(HomePayload)==0x78&&alignof(HomePayload)>=alignof(void*));
const void* home_pointer=nullptr;
void* home_listener=nullptr;
int32_t home_id=0;
std::array<unsigned char,0x78> home_bytes{};
void* setter_control=nullptr;
const void* setter_callback=nullptr;
Listener* setter_install=nullptr;
Control app{11,"Task=7#11"},icon{22,"FloatingIcon_all#22"},child{23,"[BBQ] FloatingIcon_all#22"},new_child{24,"[BBQ] FloatingIcon_all#22"},desktop{25,"DesktopIcon"};
Control owned_host{41,"Genie.Close#41"},next_owned_host{42,"Genie.Close#42"};
void* publication_host=&owned_host;
std::vector<std::pair<const void*,const void*>> publication_leases;
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
struct AnimationCall {
    char kind;
    void* transaction;
    const void* control;
    const void* parameters;
    int32_t animation_id;
    std::array<float,4> end{};
    const void* parameter_object=nullptr;
    std::array<float,4> alpha{};
    const void* alpha_property=nullptr;
    std::array<unsigned char,0x30> alpha_bytes{};
    std::array<int32_t,2> types{};
};
std::vector<AnimationCall> animation_calls;
std::mutex animation_calls_mutex,owned_start_mutex;
std::condition_variable owned_start_condition;
bool pause_owned_start=false,owned_start_entered=false,release_owned_start=false,auto_host_commit=true;
struct HostCommit {ASurfaceTransaction* transaction;void* context;ASurfaceTransaction_OnCommit callback;};
std::vector<HostCommit> host_commits;
alignas(void*) std::array<unsigned char,sizeof(void*)> host_commit_stats{};
std::vector<std::pair<Param*,const void*>> parameter_leases;
void* StockAnimation(char kind,void* transaction,const void* surface,const void* parameters) {
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"original animation append runs outside the registry mutex");
    if(unlocked)g_mutex.unlock();
    AnimationCall call{kind,transaction,surface?*static_cast<const void* const*>(surface):nullptr,parameters,-1};
    if(parameters) {
        const auto* object=*static_cast<const unsigned char* const*>(parameters);
        call.parameter_object=object;
        std::memcpy(call.types.data(),object+0x40,sizeof(call.types));
        std::memcpy(&call.animation_id,object+0x48,4);
        call.end={Spring(parameters,"centerX").end,Spring(parameters,"centerY").end,
            Spring(parameters,"width").end,Spring(parameters,"height").end};
        const auto alpha=Spring(parameters,"alpha");call.alpha={alpha.start,alpha.end,alpha.velocity,alpha.delay};
        const auto* entry=Find(parameters,0x18,"alpha");
        if(entry) {
            call.alpha_property=*reinterpret_cast<const void* const*>(entry);
            if(call.alpha_property)std::memcpy(call.alpha_bytes.data(),call.alpha_property,call.alpha_bytes.size());
        }
    }
    {std::lock_guard<std::mutex> lock(animation_calls_mutex);animation_calls.push_back(call);}
    if(kind=='S'&&call.control==&owned_host) {
        std::unique_lock<std::mutex> lock(owned_start_mutex);
        if(pause_owned_start) {
            owned_start_entered=true;owned_start_condition.notify_all();
            owned_start_condition.wait(lock,[]{return release_owned_start;});
        }
    }
    return transaction;
}
void* StockStart(void* transaction,const void* surface,const void* parameters) {
    return StockAnimation('S',transaction,surface,parameters);
}
void* StockAdjust(void* transaction,const void* surface,const void* parameters) {
    return StockAnimation('A',transaction,surface,parameters);
}
uint64_t TransactionId(void* transaction){return reinterpret_cast<uintptr_t>(transaction);}
void* StockStop(void* transaction,const void* surface,int32_t id) {
    stop_prime_order.push_back(0);
    StockAnimation('X',transaction,surface,nullptr);animation_calls.back().animation_id=id;return transaction;
}
void ReleasePublication(const void* control,const void* owner) {
    const auto lease=std::find(publication_leases.begin(),publication_leases.end(),std::pair{control,owner});
    Check(lease!=publication_leases.end(),"temporary publication releases the same stable parcel owner exactly once");
    publication_leases.erase(lease);++publication_released;
}
void ReleaseParameters(const void* object,const void* owner) {
    const auto lease=std::find_if(parameter_leases.begin(),parameter_leases.end(),[&](const auto& entry) {
        return entry.first->object.data()==object&&entry.second==owner;
    });
    Check(lease!=parameter_leases.end(),"private animation parameters release their stable owner exactly once");
    delete lease->first;parameter_leases.erase(lease);++parameters_released;
}
void StockHostCommit(ASurfaceTransaction* transaction,void* context,ASurfaceTransaction_OnCommit callback) {
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"host commit registration runs outside the registry mutex");
    if(unlocked)g_mutex.unlock();
    // The production callback ignores stats; NDK still requires a non-null token.
    if(auto_host_commit)callback(context,reinterpret_cast<ASurfaceTransactionStats*>(host_commit_stats.data()));
    else host_commits.push_back({transaction,context,callback});
}
void CommitHost(size_t index) {
    Check(index<host_commits.size(),"commit identifies an outstanding caller transaction");
    const auto commit=host_commits[index];host_commits.erase(host_commits.begin()+index);
    commit.callback(commit.context,reinterpret_cast<ASurfaceTransactionStats*>(host_commit_stats.data()));
}
void* StockBlast(void* queue,const void*,uint32_t,uint32_t,int32_t){return queue;}
void StockHome(void* listener,const void* parameters,int32_t id) {
    ++home_calls;home_pointer=parameters;home_listener=listener;home_id=id;
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"HomeParams does not hold the registry mutex across the original callback");
    if(unlocked)g_mutex.unlock();
    home_bytes={};if(parameters)std::memcpy(home_bytes.data(),parameters,home_bytes.size());
}
void StockSetCallback(void* control,const void* callback) {
    ++set_callback_calls;setter_control=control;setter_callback=callback;
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"SetCallback does not hold the registry mutex across the original setter");
    if(unlocked)g_mutex.unlock();
    if(setter_install) {
        setter_install->callback=const_cast<void*>(callback);
        static_cast<Control*>(control)->listener=setter_install;
    }
}
void Reset() {
    for(const auto& commit:host_commits)
        commit.callback(commit.context,reinterpret_cast<ASurfaceTransactionStats*>(host_commit_stats.data()));
    host_commits.clear();auto_host_commit=true;pause_owned_start=owned_start_entered=release_owned_start=false;
    g_last={};g_icons={};g_apps={};g_anchors={};g_next=0;g_next_icon=0;g_next_app=0;
    g_next_config=Now()+1000000000000LL;g_config={true,true,false};
    g_layer=Layer;g_name=Name;g_stop=StockStop;g_transaction_id=TransactionId;
    g_start=StockStart;g_adjust=StockAdjust;animation_calls.clear();
    g_host_commit=StockHostCommit;
    g_home_params=StockHome;g_set_callback=StockSetCallback;g_blast_update=StockBlast;g_icon_ready=true;
    g_listener_vtable=listener_table.data();g_params_vtable=params_table.data();
    alpha_writes=prepared=retained=released=queued=cancelled=local_cancels=0;
    stop_prime_order.clear();
    cancel_layer=-1;cancel_epoch=0;
    suppressed=nullptr;alpha_calls.clear();accept=true;reenter_replace=false;
    preview_prepared=group_prepared=0;accept_preview=accept_group=true;defer_group=false;
    publication_kind=genie::PublicationKind::NormalGroup;publication_host=&owned_host;
    owned_retained=owned_released=publication_issued=publication_released=0;publication_leases.clear();
    clone_calls=parameters_issued=parameters_released=0;accept_clone=true;shallow_clone=false;parameter_leases.clear();
    prepared_transition={};
    parent_retained=parent_released=home_calls=set_callback_calls=0;
    home_pointer=nullptr;home_listener=nullptr;home_id=0;home_bytes={};
    setter_control=nullptr;setter_callback=nullptr;setter_install=nullptr;
    icon_listener={};app_listener={};other_listener={};
    callback_token.vtable=listener_table.data();
    icon_listener.vtable=app_listener.vtable=other_listener.vtable=g_listener_vtable;
    icon_listener.callback=app_listener.callback=other_listener.callback=&callback_token;
    app.listener=&app_listener;icon.listener=&icon_listener;child.listener=nullptr;new_child.listener=nullptr;
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
void CheckHeld(const Param& parameters) {
    const void* reference=&parameters.ref;
    Check(Spring(reference,"ratio").start==Spring(reference,"ratio").end,"pending close holds its presented aspect instead of the OEM close spring");
    Check(Spring(reference,"width").start==Spring(reference,"width").end&&
          Spring(reference,"width").velocity==0,"pending close holds its presented viewport");
    Check(g_last[0].host_transaction_id==0&&!g_last[0].supported,"held close still publishes no custom viewport");
}
void BindReadback(bool closing=true,bool registered=true,bool callback_ready=true,bool app_registered=true) {
    Reset();if(!registered)icon.listener=nullptr;
    if(!app_registered)app.listener=nullptr;
    if(!callback_ready)icon_listener.callback=nullptr;
    Param source(2);if(closing)Reverse(source);Seed(source);AddChild();
    Param target;if(closing)Reverse(target);const void* control=&icon;
    TuneParent(reinterpret_cast<void*>(0x1111),&control,target.Reference());
    Check(g_icons[0].request_ns==g_last[0].transition.event_ns&&g_icons[0].source_leash==app.id,
          "readback fixture uses a real successful custom icon registration");
}
std::array<unsigned char,0x78> HiddenReadback(const HomePayload& payload,
        const std::array<float,9>& corners={0,.2f},uint16_t present=3) {
    auto expected=payload.bytes;
    const auto set=[&](size_t index,float value) {std::memcpy(expected.data()+8+index*4,&value,4);};
    set(0,165.5f);set(2,885.5f);set(4,171);set(6,171);set(8,1);set(10,0);
    set(12,.2f*std::hypot(171.f,171.f));
    for(size_t index:{1,3,5,7,9,11,13,15})set(index,0);
    if(present&1u)set(14,corners[0]);
    for(size_t i=1;i<corners.size();++i)if(present&(1u<<i))set(15+i,corners[i]);
    return expected;
}
void CheckReadbackPositive() {
    for(auto* listener:{&icon_listener,&app_listener}) {
        BindReadback();HomePayload payload;const auto original=payload.bytes;
        HomeParams(listener,payload.bytes.data(),77);
        Check(home_calls==1&&home_listener==listener&&home_id==77,
              "custom closing App and icon readback forward their listener and callback integer once");
        Check(home_pointer!=payload.bytes.data()&&home_bytes==HiddenReadback(payload),
              "App and icon readback use the square icon endpoint with zero velocity and preserve every other byte");
        Check(payload.bytes==original&&alpha_writes==0&&prepared==1,
              "closing readback leaves the original payload and SurfaceFlinger publication unchanged");
    }
}
void CheckReadbackEndpoints() {
    Reset();Param source(2);Reverse(source);
    const std::array<float,9> corners{.37f,.2f,12.25f,13.25f,14.25f,15.25f,16.25f,17.25f,18.25f};
    constexpr const char* keys[]={"radiusRatio","cornerTLX","cornerTLY","cornerTRX","cornerTRY",
        "cornerBRX","cornerBRY","cornerBLX","cornerBLY"};
    SetSpring(source.Reference(),keys[0],0,corners[0]);
    for(size_t i=2;i<corners.size();++i)source.Add(keys[i],0,corners[i]);
    Seed(source);AddChild();Param target;Reverse(target);const void* icon_control=&icon;
    TuneParent(reinterpret_cast<void*>(0x1111),&icon_control,target.Reference());
    const auto epoch=g_last[0].transition.event_ns;const void* app_control=&app;
    Check(Spring(source.Reference(),"radiusRatio").end==0&&Spring(source.Reference(),"cornerBLY").end==0,
          "the App binding snapshots OEM endpoints before host spring normalization");
    for(int repeat=0;repeat<2;++repeat) {
        for(auto* listener:{&app_listener,&icon_listener}) {
            HomePayload payload;const auto original=payload.bytes;
            HomeParams(listener,payload.bytes.data(),77);
            Check(home_bytes==HiddenReadback(payload,corners,0x1ff)&&payload.bytes==original,
                  "both cache choices preserve every raw OEM corner endpoint without inferring its units");
        }
        TuneParent(reinterpret_cast<void*>(0x2222),&app_control,source.Reference());
        Check(g_last[0].transition.event_ns==epoch&&parent_retained==2&&parent_released==0,
              "repeated same-epoch tuning preserves the first endpoint and both retained capabilities");
    }
    Reset();Param partial(2);Reverse(partial);
    SetSpring(partial.Reference(),"radiusRatio",0,NAN);partial.Add("cornerTLY",0,NAN);
    Seed(partial);AddChild();Param partial_icon;Reverse(partial_icon);
    TuneParent(reinterpret_cast<void*>(0x1111),&icon_control,partial_icon.Reference());
    for(auto* listener:{&app_listener,&icon_listener}) {
        HomePayload payload;HomeParams(listener,payload.bytes.data(),77);
        Check(home_bytes==HiddenReadback(payload,{0,.2f},2),
              "missing or invalid OEM corner endpoints keep the corresponding callback bytes unchanged");
    }
    BindReadback();g_apps={};HomePayload without_app;HomeParams(&icon_listener,without_app.bytes.data(),77);
    Check(home_bytes==HiddenReadback(without_app,{},0),
          "an exact icon binding can normalize geometry without inventing missing App corner endpoints");
}
void CheckReadbackIdentity() {
    for(int rejected=0;rejected<23;++rejected) {
        BindReadback(rejected!=0,true,rejected!=14);HomePayload payload;void* listener=&icon_listener;
        switch(rejected) {
            case 1: listener=&other_listener;break;
            case 2: g_icons[0].source_leash=-1;break;
            case 3: g_last[0].supported=false;break;
            case 4: g_last[0].admitted=false;break;
            case 5: g_last[0].preparing=true;break;
            case 6: g_last[0].retry_pending=true;break;
            case 7: g_last[0].transition.closing=false;break;
            case 8: g_last[0].transition.leash_layer=app.id+1;break;
            case 9: ++g_last[0].transition.event_ns;break;
            case 10: ++g_last[0].animation_id;break;
            case 11: g_icons[0].parent_control=nullptr;break;
            case 12: g_icons[0].listener=&other_listener;break;
            case 13: icon.listener=&other_listener;break;
            case 14: break;
            case 15: g_icons[0].surface=&new_child;break;
            case 16: g_icons[0].parent=desktop.id;break;
            case 17: g_last={};break;
            case 18: g_last[1]=g_last[0];++g_last[1].transition.event_ns;g_last[1].valid=false;break;
            case 19: g_config.enabled=false;break;
            case 20: g_config.content=false;break;
            case 21: icon_listener.vtable=params_table.data();break;
            case 22: {const void* wrong=listener_table.data();std::memcpy(payload.bytes.data(),&wrong,sizeof(wrong));break;}
        }
        const auto original=payload.bytes;
        HomeParams(listener,payload.bytes.data(),77);
        Check(home_calls==1&&home_pointer==payload.bytes.data()&&home_listener==listener&&
              home_id==77&&home_bytes==original&&payload.bytes==original,
              "ordinary, opening, mismatched, replaced, superseded, or disabled readback forwards its original payload");
    }
    Reset();HomePayload ordinary;HomeParams(&icon_listener,ordinary.bytes.data(),31);
    Check(home_pointer==ordinary.bytes.data()&&home_bytes==ordinary.bytes&&home_id==31,
          "an ordinary unbound icon never receives custom readback suppression");
    BindReadback();HomeParams(nullptr,ordinary.bytes.data(),32);
    Check(home_pointer==ordinary.bytes.data()&&home_listener==nullptr&&home_id==32,
          "a null listener still forwards the original call");
    HomeParams(&icon_listener,nullptr,33);
    Check(home_pointer==nullptr&&home_listener==&icon_listener&&home_id==33&&home_calls==2,
          "a null parameter payload still forwards the original call");
    BindReadback();const void* app_control=&app;Stop(nullptr,&app_control,77);
    HomePayload after_stop;HomeParams(&icon_listener,after_stop.bytes.data(),77);
    Check(home_bytes==HiddenReadback(after_stop)&&!g_last[0].valid&&parent_retained==2&&parent_released==0,
          "the same closing generation retains endpoint normalization and both leases after App stop");
    BindReadback();const void* replacement=&new_child;
    BlastUpdate(reinterpret_cast<void*>(0x2222),&replacement,171,171,1);
    HomePayload after_child;HomeParams(&icon_listener,after_child.bytes.data(),77);
    Check(home_pointer==after_child.bytes.data()&&home_bytes==after_child.bytes&&parent_released==1,
          "a replacement child releases its old parent lease and cannot inherit readback suppression");
    BindReadback(true,false);Check(g_icons[0].listener==nullptr,"custom binding can precede the OEM callback setter");
    setter_install=&icon_listener;SetCallback(&icon,&callback_token);
    Check(set_callback_calls==1&&setter_control==&icon&&setter_callback==&callback_token&&
          g_icons[0].listener==&icon_listener,
          "late callback registration runs the original setter before binding its resulting listener");
    HomePayload after_setter;HomeParams(&icon_listener,after_setter.bytes.data(),77);
    Check(home_bytes==HiddenReadback(after_setter),"late listener registration protects the already-bound closing icon");
    BindReadback(true,false);++g_last[0].transition.event_ns;setter_install=&icon_listener;
    SetCallback(&icon,&callback_token);HomePayload stale_setter;HomeParams(&icon_listener,stale_setter.bytes.data(),77);
    Check(set_callback_calls==1&&home_pointer==stale_setter.bytes.data()&&home_bytes==stale_setter.bytes&&
          g_icons[0].listener==nullptr,"late callback registration cannot attach an old icon to a newer source epoch");
}
void CheckAppReadbackIdentity() {
    for(int rejected=0;rejected<24;++rejected) {
        BindReadback(rejected!=0);HomePayload payload;void* listener=&app_listener;
        const int original_layer=app.id;
        switch(rejected) {
            case 1: listener=&other_listener;break;
            case 2: g_apps[0].control=nullptr;break;
            case 3: ++g_apps[0].layer;break;
            case 4: ++g_apps[0].source_leash;break;
            case 5: ++g_apps[0].request_ns;break;
            case 6: ++g_apps[0].animation_id;break;
            case 7: g_apps[0].closing=false;break;
            case 8: g_last[0].supported=false;break;
            case 9: g_last[0].admitted=false;break;
            case 10: g_last[0].preparing=true;break;
            case 11: g_last[0].retry_pending=true;break;
            case 12: g_last[1]=g_last[0];++g_last[1].transition.event_ns;g_last[1].valid=false;break;
            case 13: app.listener=&other_listener;break;
            case 14: g_apps[0].listener=&other_listener;break;
            case 15: app_listener.vtable=params_table.data();break;
            case 16: {const void* wrong=listener_table.data();std::memcpy(payload.bytes.data(),&wrong,sizeof(wrong));break;}
            case 17: g_config.enabled=false;break;
            case 18: g_config.content=false;break;
            case 19: g_last={};break;
            case 20: g_apps={};break;
            case 21: ++app.id;break;
            case 22: g_layer=nullptr;break;
            case 23: g_last[0].transition.closing=false;break;
        }
        const auto original=payload.bytes;HomeParams(listener,payload.bytes.data(),77);
        Check(home_calls==1&&home_pointer==payload.bytes.data()&&home_listener==listener&&
              home_id==77&&home_bytes==original&&payload.bytes==original,
              "App readback requires the exact live capability, listener, epoch, admission and ABI");
        app.id=original_layer;
    }
    BindReadback();const void* app_control=&app;Stop(nullptr,&app_control,77);
    for(auto* listener:{&app_listener,&icon_listener}) {
        HomePayload payload;HomeParams(listener,payload.bytes.data(),77);
        Check(home_bytes==HiddenReadback(payload)&&parent_retained==2&&parent_released==0,
              "queued App and icon callbacks retain the same endpoint after their source stops");
    }
    BindReadback();Control replacement{app.id,app.name};replacement.listener=&other_listener;
    const void* replacement_control=&replacement;Param newer(2,78);Reverse(newer);
    Observe(&replacement_control,newer.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x3333),&replacement_control,newer.Reference());
    for(auto* listener:{&app_listener,&icon_listener}) {
        HomePayload payload;HomeParams(listener,payload.bytes.data(),77);
        Check(home_pointer==payload.bytes.data()&&home_bytes==payload.bytes,
              "a newer App capability on the same layer invalidates both old cache choices");
    }
    HomePayload replacement_payload;HomeParams(&other_listener,replacement_payload.bytes.data(),78);
    Check(home_bytes==HiddenReadback(replacement_payload)&&parent_retained==3&&parent_released==0,
          "the replacement capability receives only its own admitted endpoint");
    BindReadback(true,true,true,false);
    Check(g_apps[0].listener==nullptr,"custom App admission can precede OEM callback registration");
    setter_install=&app_listener;SetCallback(&app,&callback_token);
    HomePayload late;HomeParams(&app_listener,late.bytes.data(),77);
    Check(set_callback_calls==1&&setter_control==&app&&setter_callback==&callback_token&&
          g_apps[0].listener==&app_listener&&home_bytes==HiddenReadback(late),
          "late App registration binds the resulting listener after the original callback setter");
    BindReadback(true,true,true,false);++g_last[0].transition.event_ns;setter_install=&app_listener;
    SetCallback(&app,&callback_token);HomePayload stale;HomeParams(&app_listener,stale.bytes.data(),77);
    Check(g_apps[0].listener==nullptr&&home_pointer==stale.bytes.data()&&home_bytes==stale.bytes,
          "late App registration cannot attach an obsolete epoch to a newer source");
    Reset();app_listener.callback=nullptr;Param invalid_callback(2);Reverse(invalid_callback);Seed(invalid_callback);
    HomePayload unbound;HomeParams(&app_listener,unbound.bytes.data(),77);
    Check(g_apps[0].listener==nullptr&&home_pointer==unbound.bytes.data()&&home_bytes==unbound.bytes,
          "an App listener without a validated callback stays unbound");
    Reset();g_icon_ready=false;Param cold(2);Reverse(cold);Seed(cold);
    HomePayload before_warm;HomeParams(&app_listener,before_warm.bytes.data(),77);
    Check(std::all_of(g_apps.begin(),g_apps.end(),[](const auto& entry){return !entry.control;})&&
          parent_retained==0&&g_last[0].admitted&&home_pointer==before_warm.bytes.data()&&home_bytes==before_warm.bytes,
          "App admission before ownership helpers are ready retains no raw capability and leaves readback untouched");
    BindReadback();std::vector<std::unique_ptr<Control>> controls;
    for(size_t i=0;i<g_apps.size();++i) {
        auto control=std::make_unique<Control>();control->id=100+static_cast<int>(i);control->name="Task=8#100";
        control->listener=&other_listener;const void* reference=control.get();
        Param source(2);Reverse(source);Observe(&reference,source.Reference(),false);
        TuneParent(reinterpret_cast<void*>(0x4444),&reference,source.Reference());
        controls.push_back(std::move(control));
    }
    HomePayload evicted;HomeParams(&app_listener,evicted.bytes.data(),77);
    Check(parent_retained==34&&parent_released==1&&
          std::none_of(g_apps.begin(),g_apps.end(),[](const auto& entry){return entry.control==&app;})&&
          home_pointer==evicted.bytes.data()&&home_bytes==evicted.bytes,
          "bounded App registry eviction releases the old capability and removes its readback authority");
}
void PrepareOwnedFixture() {
    Reset();g_anchors[0]={app.id,app.id,7};publication_kind=genie::PublicationKind::OwnedClosingGroup;
}
void CheckOwnedHostPositive() {
    PrepareOwnedFixture();Param source(2);Reverse(source);const void* control=&app;
    SetSpring(source.Reference(),"alpha",1,0);SetVelocity(source.Reference(),"alpha",.25f);
    std::array<unsigned char,0x30> original_alpha{};
    auto* alpha_property=*reinterpret_cast<unsigned char* const*>(Find(source.Reference(),0x18,"alpha"));
    const std::array<float,4> alpha_tail{.85f,.25f,.5f,.75f};
    std::memcpy(alpha_property+0x20,alpha_tail.data(),sizeof(alpha_tail));
    std::memcpy(original_alpha.data(),alpha_property,original_alpha.size());
    void* transaction=reinterpret_cast<void*>(0x5100);
    Check(Start(transaction,&control,source.Reference())==transaction,"Start preserves the original transaction return value");
    Check(animation_calls.size()==2&&animation_calls[0].control==&owned_host,
          "an owned closing group receives its own host animation before the original OEM animation");
    Check(animation_calls[0].kind=='S'&&animation_calls[1].kind=='S'&&animation_calls[1].control==&app&&
          animation_calls[0].transaction==transaction&&animation_calls[1].transaction==transaction&&
          animation_calls[0].parameter_object!=source.ref&&animation_calls[1].parameter_object==source.ref&&
          animation_calls[0].end==animation_calls[1].end&&animation_calls[0].animation_id==77,
          "the first owned host uses independent parameters with the same viewport in the caller's transaction");
    Check(animation_calls[0].alpha==std::array<float,4>{1,0,.25f,.7f}&&
          animation_calls[1].alpha==std::array<float,4>{1,0,.25f,.7f}&&
          animation_calls[0].alpha_property!=alpha_property&&animation_calls[1].alpha_property==alpha_property&&
          animation_calls[0].alpha_bytes==original_alpha&&animation_calls[1].alpha_bytes==original_alpha&&
          std::memcmp(original_alpha.data(),alpha_property,original_alpha.size())==0&&
          parameters_issued==1&&parameters_released==1&&parameter_leases.empty(),
          "owned content keeps an independent exact copy of every OEM alpha byte including fade, velocity, delay, damping and response");
    Check(group_prepared==1&&publication_issued==1&&publication_released==1&&publication_leases.empty()&&
          owned_retained==owned_released+1,
          "owned admission transfers one retained host beyond the stable temporary parcel lease");
    for(int i=0;i<3;++i) {
        HomePayload payload;HomeParams(&app_listener,payload.bytes.data(),77);
        Check(home_bytes==HiddenReadback(payload),"owned-host routing preserves logical App readback at the icon endpoint");
    }
    Check(group_prepared==1&&clone_calls==1&&animation_calls.size()==2,
          "ordinary readback frames neither clone parameters, call Binder preparation nor append another host animation");
    transaction=reinterpret_cast<void*>(0x5200);
    Start(transaction,&control,source.Reference());
    Check(group_prepared==1&&publication_issued==1&&animation_calls.size()==4&&
          animation_calls[2].kind=='A'&&animation_calls[2].control==&owned_host&&
          animation_calls[3].kind=='S'&&animation_calls[3].control==&app&&
          animation_calls[2].transaction==transaction&&animation_calls[3].transaction==transaction,
          "a repeated same-epoch Start adjusts the existing owned host without another Binder preparation");
    Check(animation_calls[2].alpha_property!=alpha_property&&animation_calls[3].alpha_property==alpha_property&&
          animation_calls[2].alpha_bytes==original_alpha&&animation_calls[3].alpha_bytes==original_alpha,
          "same-epoch owned adjustment preserves the independent complete OEM fade curve");
}
void CheckOwnedHostEpochs() {
    PrepareOwnedFixture();Param source(2);Reverse(source);const void* control=&app;
    Start(reinterpret_cast<void*>(0x5100),&control,source.Reference());
    const auto first_epoch=g_last[0].transition.event_ns;
    Param same(-1,77);Reverse(same);void* transaction=reinterpret_cast<void*>(0x5200);
    Adjust(transaction,&control,same.Reference());
    Check(group_prepared==1&&animation_calls.size()==4&&animation_calls[2].kind=='A'&&
          animation_calls[2].control==&owned_host&&animation_calls[3].kind=='A'&&animation_calls[3].control==&app&&
          animation_calls[2].transaction==transaction&&animation_calls[3].transaction==transaction,
          "same-epoch Adjust reuses the exact owned capability and caller transaction");
    publication_host=&next_owned_host;Param newer(-1,78);Reverse(newer);
    transaction=reinterpret_cast<void*>(0x5300);Adjust(transaction,&control,newer.Reference());
    Check(group_prepared==2&&g_last[0].transition.event_ns>first_epoch&&publication_issued==2&&
          publication_released==2&&publication_leases.empty()&&owned_retained==owned_released+1,
          "a new Adjust epoch must prepare and retain its own host while releasing the preceding capability");
    Check(animation_calls.size()==6&&animation_calls[4].kind=='S'&&animation_calls[4].control==&next_owned_host&&
          animation_calls[5].kind=='A'&&animation_calls[5].control==&app&&
          animation_calls[4].transaction==transaction&&animation_calls[5].transaction==transaction&&
          animation_calls[4].animation_id==78&&animation_calls[4].end==animation_calls[5].end&&
          animation_calls[4].types==std::array<int32_t,2>{2,0}&&
          animation_calls[5].types==std::array<int32_t,2>{-1,1},
          "a new owned host starts once even when its originating OEM operation is Adjust");
    transaction=reinterpret_cast<void*>(0x5400);Stop(transaction,&control,78);
    Check(animation_calls.size()==7&&animation_calls.back().kind=='X'&&animation_calls.back().control==&app&&
          animation_calls.back().transaction==transaction&&animation_calls.back().animation_id==78&&cancelled==1,
          "the original Stop addresses only the OEM host and leaves owned-host retirement to the central owner");

    PrepareOwnedFixture();Param initial(2);Reverse(initial);Start(reinterpret_cast<void*>(0x5100),&control,initial.Reference());
    accept_group=false;defer_group=true;Param deferred(-1,78);Reverse(deferred);
    Adjust(reinterpret_cast<void*>(0x5200),&control,deferred.Reference());
    Check(group_prepared==2&&animation_calls.size()==3&&animation_calls.back().kind=='A'&&
          animation_calls.back().control==&app&&!g_last[0].admitted&&g_last[0].retry_pending,
          "a deferred new epoch cannot reuse the previous epoch's host admission");
    accept_group=true;publication_host=&next_owned_host;transaction=reinterpret_cast<void*>(0x5300);
    Adjust(transaction,&control,deferred.Reference());
    Check(group_prepared==3&&animation_calls.size()==5&&animation_calls[3].kind=='S'&&
          animation_calls[3].control==&next_owned_host&&animation_calls[4].kind=='A'&&
          animation_calls[4].control==&app&&animation_calls[3].transaction==transaction&&
          animation_calls[4].transaction==transaction&&g_last[0].admitted&&!g_last[0].retry_pending,
          "first-buffer admission on the same deferred epoch starts its newly prepared host in that retry transaction");
}
void CheckOwnedHostDomains() {
    using Kind=genie::PublicationKind;
    const Kind kinds[]={Kind::NormalGroup,Kind::RegisteredPreview,Kind::None,static_cast<Kind>(99),
        Kind::OwnedClosingGroup,Kind::OwnedClosingGroup};
    for(size_t rejected=0;rejected<std::size(kinds);++rejected) {
        PrepareOwnedFixture();publication_kind=kinds[rejected];if(rejected==5)publication_host=nullptr;
        Param source(2);if(rejected!=4)Reverse(source);const void* control=&app;
        Start(reinterpret_cast<void*>(0x5100),&control,source.Reference());
        Check(animation_calls.size()==1&&animation_calls[0].kind=='S'&&animation_calls[0].control==&app&&
              owned_retained==owned_released&&publication_issued==publication_released&&publication_leases.empty(),
              "normal, registered-preview, invalid, opening, and missing-capability replies never route an owned host");
    }
    Reset();AnchorPrestart();Param preview(2);const void* preview_control=&prestart;
    Start(reinterpret_cast<void*>(0x5100),&preview_control,preview.Reference());
    Check(preview_prepared==1&&group_prepared==0&&animation_calls.size()==1&&
          animation_calls[0].control==&prestart&&owned_retained==0,
          "the existing registered opening-preview path keeps its original OEM animation target");
    PrepareOwnedFixture();Param closing(2);Reverse(closing);const void* control=&app;
    Start(reinterpret_cast<void*>(0x5100),&control,closing.Reference());
    publication_kind=Kind::NormalGroup;Param opening(2,78);
    Start(reinterpret_cast<void*>(0x5200),&control,opening.Reference());
    Check(group_prepared==2&&animation_calls.size()==3&&animation_calls.back().control==&app&&
          owned_retained==owned_released&&!g_last[0].transition.closing,
          "a newer opening drops its old closing-host capability and remains on the OEM target");
    PrepareOwnedFixture();Param original(2);Reverse(original);Start(reinterpret_cast<void*>(0x5100),&control,original.Reference());
    Control other{12,"Task=8#12"};const void* other_control=&other;g_anchors[1]={other.id,other.id,8};
    publication_kind=Kind::RegisteredPreview;Param independent(2);Reverse(independent);
    Start(reinterpret_cast<void*>(0x5200),&other_control,independent.Reference());
    Check(group_prepared==2&&animation_calls.size()==3&&animation_calls.back().control==&other&&
          owned_retained==owned_released+1,
          "another controller reusing the animation ID cannot borrow the existing owned host");
    g_config.enabled=false;Start(reinterpret_cast<void*>(0x5300),&control,original.Reference());
    Check(animation_calls.size()==4&&animation_calls.back().control==&app&&animation_calls.back().kind=='S',
          "disabling the feature stops owned-host routing even for an already admitted epoch");
    for(bool alias:{false,true}) {
        PrepareOwnedFixture();accept_clone=alias;shallow_clone=alias;
        Param source(2);Reverse(source);
        const auto original_alpha=Spring(source.Reference(),"alpha");
        Start(reinterpret_cast<void*>(0x5100),&control,source.Reference());
        Check(clone_calls==1&&group_prepared==0&&animation_calls.size()==1&&animation_calls[0].control==&app&&
              owned_retained==0&&parameters_issued==parameters_released&&parameter_leases.empty(),
              "failed or aliased parameter clones reject owned preparation and never enter a custom host");
        Check(Spring(source.Reference(),"alpha").start==original_alpha.start&&
              Spring(source.Reference(),"alpha").end==original_alpha.end,
              "failed closing clone leaves non-viewport OEM properties untouched");
        for(const char* key:{"centerX","width","ratio","cornerTLX"}) {
            const auto spring=Spring(source.Reference(),key);
            Check(spring.valid&&spring.start==spring.end&&spring.velocity==0&&spring.delay==0,
                  "failed closing clone holds the presented viewport/crop instead of the bare OEM close spring");
        }
    }
}
void CheckOwnedHostCloneFallback() {
    for(bool alias:{false,true}) for(bool bound:{false,true}) {
        PrepareOwnedFixture();publication_kind=genie::PublicationKind::NormalGroup;
        const void* app_control=&app;const void* icon_control=&icon;
        Param opening(2);Start(reinterpret_cast<void*>(0x5500),&app_control,opening.Reference());
        AddChild();
        if(bound) {
            Param opening_icon;
            TuneParent(reinterpret_cast<void*>(0x5510),&icon_control,opening_icon.Reference());
        }
        const int registrations=prepared;
        const int64_t opening_epoch=g_last[0].transition.event_ns;
        Check(g_last[0].admitted&&(!bound||g_icons[0].request_ns==opening_epoch),
              "clone fallback starts with an admitted opening and an optional registered icon");
        publication_kind=genie::PublicationKind::OwnedClosingGroup;accept_clone=alias;shallow_clone=alias;
        Param closing(-1,78);Reverse(closing);const auto original_alpha=Spring(closing.Reference(),"alpha");
        Adjust(reinterpret_cast<void*>(0x5520),&app_control,closing.Reference());
        Check(clone_calls==1&&group_prepared==1&&queued==1&&!g_last[0].admitted&&
              g_last[0].transition.closing&&g_last[0].transition.event_ns>opening_epoch&&
              owned_retained==0&&parameter_leases.empty()&&
              parameters_issued==parameters_released,
              "failed or aliased closing clone never prepares a new group or enters a custom host");
        Check(Spring(closing.Reference(),"alpha").start==original_alpha.start&&
              Spring(closing.Reference(),"alpha").end==original_alpha.end,
              "failed closing clone leaves non-viewport App properties untouched");
        for(const char* key:{"centerX","width","ratio","cornerTLX"}) {
            const auto spring=Spring(closing.Reference(),key);
            Check(spring.valid&&spring.start==spring.end&&spring.velocity==0&&spring.delay==0,
                  "failed closing clone holds the App presented viewport/crop");
        }
        Param closing_icon(1,78),expected_icon(1,78);Reverse(closing_icon);Reverse(expected_icon);
        void* transaction=reinterpret_cast<void*>(0x5530);
        Adjust(transaction,&icon_control,closing_icon.Reference());
        Check(prepared==registrations+(bound?1:0),
              "the failed closing clone re-primes only the established icon mask and never registers through the icon callback");
        Check(!g_last[0].supported&&!g_last[0].retry_pending&&
              g_icons[0].request_ns==(bound?opening_epoch:0)&&alpha_writes==0&&!suppressed,
              "failed clone generation never hides its established custom icon");
        if(bound) {
            for(const char* key:{"centerX","width","ratio","cornerTLX"}) {
                const auto spring=Spring(closing_icon.Reference(),key);
                Check(spring.valid&&spring.start==spring.end&&spring.velocity==0&&spring.delay==0,
                      "failed clone generation holds the established icon's presented viewport/crop");
            }
        } else Check(Properties(closing_icon)==Properties(expected_icon),
              "clone failure preserves stock icon geometry when no custom buffer exists");
        accept_clone=true;shallow_clone=false;
        Param recovered(-1,79);Reverse(recovered);
        Adjust(reinterpret_cast<void*>(0x5540),&app_control,recovered.Reference());
        Param recovered_icon(1,79);Reverse(recovered_icon);
        TuneParent(reinterpret_cast<void*>(0x5550),&icon_control,recovered_icon.Reference());
        Check(group_prepared==2&&queued==2&&prepared==registrations+(bound?2:1)&&g_last[0].admitted&&g_last[0].supported&&
              g_icons[0].request_ns==g_last[0].transition.event_ns&&g_icons[0].animation_id==79,
              "a newer valid closing epoch recovers group and icon admission after parameter clone failure");
    }
}
void CheckOwnedHostCommit() {
    PrepareOwnedFixture();auto_host_commit=false;pause_owned_start=true;
    Param first(2);Reverse(first);const void* control=&app;
    void* first_transaction=reinterpret_cast<void*>(0x6100);
    std::thread initial([&]{Start(first_transaction,&control,first.Reference());});
    {
        std::unique_lock<std::mutex> lock(owned_start_mutex);
        Check(owned_start_condition.wait_for(lock,std::chrono::seconds(3),[]{return owned_start_entered;}),
              "first owned start reaches its original callback without holding the registry mutex");
    }
    Param concurrent(-1,77);Reverse(concurrent);
    std::thread repeated([&]{Adjust(reinterpret_cast<void*>(0x6200),&control,concurrent.Reference());});
    repeated.join();
    Check(group_prepared==1&&animation_calls.size()==2&&animation_calls[0].kind=='S'&&
          animation_calls[0].control==&owned_host&&animation_calls[1].kind=='A'&&animation_calls[1].control==&app,
          "a concurrent same-epoch Adjust cannot precede the still-appending first owned start");
    {
        std::lock_guard<std::mutex> lock(owned_start_mutex);release_owned_start=true;
    }
    owned_start_condition.notify_all();initial.join();
    Check(host_commits.size()==1&&host_commits[0].transaction==first_transaction,
          "the first owned start waits for its exact caller transaction's commit");
    Param pending(-1,77);Reverse(pending);
    Adjust(reinterpret_cast<void*>(0x6300),&control,pending.Reference());
    Check(group_prepared==1&&animation_calls.size()==4&&animation_calls.back().kind=='A'&&
          animation_calls.back().control==&app,
          "returning from start does not authorize owned adjustments before transaction commit");
    CommitHost(0);Param committed(-1,77);Reverse(committed);
    Adjust(reinterpret_cast<void*>(0x6400),&control,committed.Reference());
    Check(group_prepared==1&&animation_calls.size()==6&&animation_calls[4].kind=='A'&&
          animation_calls[4].control==&owned_host&&animation_calls[5].control==&app&&host_commits.empty(),
          "the committed epoch can then adjust its owned host without another preparation");

    PrepareOwnedFixture();auto_host_commit=false;Param old(2);Reverse(old);
    Start(reinterpret_cast<void*>(0x7100),&control,old.Reference());
    publication_host=&next_owned_host;Param replacement(-1,78);Reverse(replacement);
    void* replacement_transaction=reinterpret_cast<void*>(0x7200);
    Adjust(replacement_transaction,&control,replacement.Reference());
    Check(host_commits.size()==2&&host_commits[0].context&&host_commits[1].context&&
          host_commits[0].context!=host_commits[1].context&&animation_calls.size()==4&&animation_calls[2].kind=='S'&&
          animation_calls[2].control==&next_owned_host,"a replacement epoch owns an independent initial-start commit");
    CommitHost(0);Param still_pending(-1,78);Reverse(still_pending);
    Adjust(reinterpret_cast<void*>(0x7300),&control,still_pending.Reference());
    Check(group_prepared==2&&animation_calls.size()==5&&animation_calls.back().control==&app&&
          host_commits.size()==1&&host_commits[0].transaction==replacement_transaction,
          "an older epoch's late commit cannot unlock the replacement host");
    CommitHost(0);Param now_committed(-1,78);Reverse(now_committed);
    Adjust(reinterpret_cast<void*>(0x7400),&control,now_committed.Reference());
    Check(group_prepared==2&&animation_calls.size()==7&&animation_calls[5].kind=='A'&&
          animation_calls[5].control==&next_owned_host&&animation_calls[6].control==&app&&host_commits.empty(),
          "only the replacement epoch's own commit authorizes its later host adjustments");

    PrepareOwnedFixture();auto_host_commit=false;Param discarded(2);Reverse(discarded);
    Start(reinterpret_cast<void*>(0x8100),&control,discarded.Reference());
    Check(host_commits.size()==1,"a discarded transaction has exactly one outstanding commit token");
    const auto abandoned_token=host_commits[0].context;host_commits.clear();
    publication_host=&next_owned_host;Param recovered(-1,78);Reverse(recovered);
    Adjust(reinterpret_cast<void*>(0x8200),&control,recovered.Reference());
    Check(host_commits.size()==1&&host_commits[0].context!=abandoned_token&&animation_calls.size()==4&&
          animation_calls[2].kind=='S'&&animation_calls[2].control==&next_owned_host,
          "abandoning an unapplied transaction does not reuse its commit token or block the next epoch's start");
    Param waiting(-1,78);Reverse(waiting);Adjust(reinterpret_cast<void*>(0x8300),&control,waiting.Reference());
    Check(animation_calls.size()==5&&animation_calls.back().control==&app,
          "the replacement after an abandoned transaction still waits for its own publication commit");
    CommitHost(0);Param ready(-1,78);Reverse(ready);Adjust(reinterpret_cast<void*>(0x8400),&control,ready.Reference());
    Check(animation_calls.size()==7&&animation_calls[5].control==&next_owned_host&&animation_calls[5].kind=='A'&&
          group_prepared==2&&host_commits.empty(),"the replacement proceeds after its own commit without retrying Binder");
}
void ClearStopObservations() {
    alpha_writes=0;suppressed=nullptr;alpha_calls.clear();animation_calls.clear();cancelled=local_cancels=0;
}
void BindStoppedIcon(bool closing=true,bool committed=true,bool registered=true) {
    PrepareOwnedFixture();auto_host_commit=committed;accept=registered;
    if(!closing)publication_kind=genie::PublicationKind::NormalGroup;
    Param source(2);if(closing)Reverse(source);const void* app_control=&app;
    Start(reinterpret_cast<void*>(0x9100),&app_control,source.Reference());
    AddChild();Param target;if(closing)Reverse(target);const void* icon_control=&icon;
    TuneParent(reinterpret_cast<void*>(0x9200),&icon_control,target.Reference());
    Check(prepared==1,"stop fixture attempts actual custom icon registration against its App source");
    if(registered)Check(g_icons[0].source_leash==app.id&&
        g_icons[0].request_ns==g_last[0].transition.event_ns&&g_icons[0].parent_control==&icon&&
        g_icons[0].readback_child==&child,"stop fixture retains the exact successful icon publication");
    ClearStopObservations();
}
void CheckStoppedPair(void* transaction,size_t offset=0) {
    Check(alpha_calls.size()==offset+2&&alpha_calls[offset].transaction==transaction&&
        alpha_calls[offset+1].transaction==transaction&&
        alpha_calls[offset].surface==reinterpret_cast<ASurfaceControl*>(&icon)&&
        alpha_calls[offset+1].surface==reinterpret_cast<ASurfaceControl*>(&child)&&
        alpha_calls[offset].value==0&&alpha_calls[offset+1].value==0,
        "committed owned closing Stop hides exactly its retained icon parent and current buffer in the original transaction");
}
void CheckStopSuppression() {
    for(bool app_stop:{false,true}) {
        BindStoppedIcon();const void* control=app_stop?&app:&icon;void* transaction=reinterpret_cast<void*>(0x9300);
        const int preparations=group_prepared,clones=clone_calls;
        Check(Stop(transaction,&control,77)==transaction,"Stop preserves its original return value");
        CheckStoppedPair(transaction);
        Check(animation_calls.size()==1&&animation_calls[0].kind=='X'&&animation_calls[0].control==control&&
            animation_calls[0].transaction==transaction&&animation_calls[0].animation_id==77&&
            group_prepared==preparations&&clone_calls==clones,
            "icon and App Stop each forward once to their exact original target without another owned animation or preparation");
        Check(cancelled==(app_stop?1:0)&&g_last[0].valid==!app_stop&&local_cancels==0,
            "App Stop preserves its existing cancellation while icon Stop never cancels the shared owner");
    }
    BindStoppedIcon();const void* app_control=&app;const void* icon_control=&icon;
    Stop(reinterpret_cast<void*>(0x9400),&app_control,77);
    Stop(reinterpret_cast<void*>(0x9500),&icon_control,77);CheckStoppedPair(reinterpret_cast<void*>(0x9500),2);
    Check(cancelled==1&&animation_calls.size()==2&&animation_calls[0].control==&app&&
        animation_calls[1].control==&icon&&!g_last[0].valid,
        "an exact closing icon tail remains suppressible after its App Stop without cancelling the owner twice");

    BindStoppedIcon(true,false);void* transaction=reinterpret_cast<void*>(0x9600);
    Check(host_commits.size()==1,"uncommitted close fixture retains its first publication callback");
    Stop(transaction,&icon_control,77);
    Check(alpha_calls.empty()&&cancelled==0&&animation_calls.size()==1&&animation_calls[0].control==&icon,
        "an owned close that has not committed cannot hide its original icon at Stop");
    CommitHost(0);ClearStopObservations();Stop(transaction,&icon_control,77);CheckStoppedPair(transaction);

    // Each rejected capability starts from a real successful binding except
    // the explicit failed-registration and opening cases.
    for(int rejected=0;rejected<38;++rejected) {
        BindStoppedIcon(rejected!=0,true,rejected!=1);
        const void* control=&icon;const void* surface=&control;int32_t id=77;
        transaction=reinterpret_cast<void*>(0x9700);
        Control same_icon{icon.id,icon.name},same_app{app.id,app.name};
        Control malformed_child{child.id,child.name};
        switch(rejected) {
            case 0:break;
            case 1:break;
            case 2:{const void* replacement=&new_child;
                BlastUpdate(reinterpret_cast<void*>(0x2222),&replacement,171,171,1);break;}
            case 3:control=&same_icon;break;
            case 4:control=&same_app;break;
            case 5:id=78;break;
            case 6:{publication_kind=genie::PublicationKind::NormalGroup;Param opening(2,78);
                Start(reinterpret_cast<void*>(0x9800),&app_control,opening.Reference());break;}
            case 7:++g_last[0].transition.event_ns;break;
            case 8:++g_apps[0].request_ns;break;
            case 9:g_icons[0].parent_control=nullptr;break;
            case 10:g_icons[0].surface=nullptr;break;
            case 11:g_icons[0].readback_child=&new_child;break;
            case 12:++g_icons[0].parent;break;
            case 13:g_icons[0].width=0;break;
            case 14:g_icons[0].height=0;break;
            case 15:g_apps[0].kind=genie::PublicationKind::RegisteredPreview;break;
            case 16:g_apps[0].owned_host=nullptr;break;
            case 17:++g_apps[0].owned_layer;break;
            case 18:g_apps[0].host_started=false;break;
            case 19:g_apps[0].host_commit_token=0;break;
            case 20:++g_apps[0].layer;break;
            case 21:g_last[0].admitted=false;break;
            case 22:g_last[0].supported=false;break;
            case 23:g_last[0].preparing=true;break;
            case 24:g_last[0].retry_pending=true;break;
            case 25:g_config.enabled=false;break;
            case 26:g_config.content=false;break;
            case 27:g_icon_ready=false;break;
            case 28:transaction=nullptr;break;
            case 29:surface=nullptr;break;
            case 30:control=nullptr;break;
            case 31:g_layer=nullptr;break;
            case 32:g_name=nullptr;break;
            case 33:malformed_child.name="Unrelated buffer";
                // A buffer replacement resets request_ns; a forged pointer swap
                // must model the same binding invalidation to stay faithful.
                g_icons[0].surface=g_icons[0].readback_child=&malformed_child;
                g_icons[0].request_ns=0;break;
            case 34:g_icons[0].surface=g_icons[0].readback_child=&app;
                g_icons[0].request_ns=0;break;
            case 35:malformed_child.id=icon.id;
                g_icons[0].surface=g_icons[0].readback_child=&malformed_child;
                g_icons[0].request_ns=0;break;
            case 36:malformed_child.id=app.id;
                g_icons[0].surface=g_icons[0].readback_child=&malformed_child;
                g_icons[0].request_ns=0;break;
            case 37:malformed_child.id=owned_host.id;
                g_icons[0].surface=g_icons[0].readback_child=&malformed_child;
                g_icons[0].request_ns=0;break;
        }
        ClearStopObservations();Stop(transaction,surface,id);
        Check(alpha_calls.empty()&&alpha_writes==0&&animation_calls.size()==1&&
            animation_calls[0].kind=='X'&&animation_calls[0].control==(surface?control:nullptr)&&
            animation_calls[0].transaction==transaction&&animation_calls[0].animation_id==id&&local_cancels==0,
            "opening, failed, replaced, stale, mismatched, uncommitted, or disabled Stop keeps the original call without hiding a layer");
    }
}
}
namespace genie {
bool CloneAnimationParameters(const void* reference,AnimationParameters* output) {
    ++clone_calls;if(!output)return false;output->Reset();if(!accept_clone||!reference)return false;
    const auto* original=*static_cast<const unsigned char* const*>(reference);if(!original)return false;
    auto copy=std::make_unique<Param>();copy->nodes.clear();std::memcpy(copy->object.data(),original,copy->object.size());
    if(!shallow_clone) {
        const unsigned char* root=nullptr;std::memcpy(&root,original+0x18,sizeof(root));
        std::memset(copy->object.data()+0x18,0,sizeof(root));
        std::vector<const unsigned char*> pending;if(root)pending.push_back(root);
        while(!pending.empty()) {
            const auto* node=pending.back();pending.pop_back();
            for(size_t offset:{0,8}) {
                const unsigned char* child_node=nullptr;std::memcpy(&child_node,node+offset,sizeof(child_node));
                if(child_node)pending.push_back(child_node);
            }
            const std::string key(reinterpret_cast<const char*>(node+0x21),node[0x20]>>1);
            const unsigned char* property=nullptr;std::memcpy(&property,node+0x38,sizeof(property));
            copy->Add(key.c_str(),0,0);std::memcpy(copy->nodes.back()->property.data(),property,0x30);
        }
    }
    output->object=copy->object.data();output->release=ReleaseParameters;
    parameter_leases.emplace_back(copy.release(),&output->object);++parameters_issued;return true;
}
void QueueTransition(const Transition&){++queued;}
void QueueCancel(int32_t layer,int64_t event_ns){++cancelled;cancel_layer=layer;cancel_epoch=event_ns;}
bool WarmIconRenderer(){return true;}
void RetainIconSurface(void* surface,const void*) {
    if(surface==&child||surface==&new_child)++retained;
    else if(surface==&owned_host||surface==&next_owned_host)++owned_retained;
    else if(surface)++parent_retained;
}
void ReleaseIconSurface(void* surface,const void*) {
    if(surface==&child||surface==&new_child)++released;
    else if(surface==&owned_host||surface==&next_owned_host)++owned_released;
    else if(surface)++parent_released;
}
void CancelIconRenderer(){++local_cancels;}
bool PrepareOpeningPreview(void*,void*,int,int,const Transition& transition) {
    ++preview_prepared;
    return accept_preview&&!transition.closing;
}
GroupPreparation PrepareContentGroupStatus(void*,void*,genie::model::Rect*,const Transition& transition,
                                          GroupPublication* publication) {
    ++group_prepared;
    if(transition.task_id<0) return GroupPreparation::Rejected;
    if(accept_group) {
        if(publication) {
            publication->Reset();publication->kind=publication_kind;
            if(publication_kind==PublicationKind::OwnedClosingGroup&&publication_host) {
                publication->host=publication_host;publication->release=ReleasePublication;
                publication_leases.emplace_back(publication->host,&publication->host);++publication_issued;
            }
        }
        return GroupPreparation::Ready;
    }
    return defer_group ? GroupPreparation::Deferred : GroupPreparation::Rejected;
}
bool PrepareIcon(void*,void* surface,void* parent,int tw,int th,int ww,int wh,const Transition& transition) {
    prepared_transition=transition;
    stop_prime_order.push_back(1);
    ++prepared;Check(std::any_of(g_icons.begin(),g_icons.end(),
        [&](const IconBuffer& entry){return entry.surface==surface&&g_layer&&parent&&
            entry.parent==g_layer(parent);}),
        "central registration uses exact retained capabilities");
    Check(tw==171&&th==171&&ww==1120&&wh==2436,"central registration preserves texture/window domains");
    const bool unlocked=g_mutex.try_lock();Check(unlocked,"no launcher registry lock crosses Binder");
    if(unlocked)g_mutex.unlock();Check(retained==released+1,"independent lease held during RPC");
    if(reenter_replace){std::lock_guard<std::mutex> lock(g_mutex);g_icons[0].surface=&new_child;g_icons[0].request_ns=INT64_MAX;}
    return accept;
}
}
int main(){
    CheckStopSuppression();
    CheckOwnedHostPositive();CheckOwnedHostEpochs();CheckOwnedHostDomains();CheckOwnedHostCloneFallback();CheckOwnedHostCommit();
    CheckReadbackPositive();CheckReadbackEndpoints();CheckReadbackIdentity();CheckAppReadbackIdentity();
    // An established icon must keep its App identity when another App reuses
    // the same OEM animation ID, including callbacks after its App has stopped.
    Reset();Param closing_source(2);Reverse(closing_source);Seed(closing_source);AddChild();
    const void* bound_icon_control=&icon;const void* bound_app_control=&app;
    Param initial_closing_icon;Reverse(initial_closing_icon);
    TuneParent(reinterpret_cast<void*>(0x1010),&bound_icon_control,initial_closing_icon.Reference());
    const auto closing_epoch=g_icons[0].request_ns;
    Control other_app{12,"Task=8#12"};const void* other_app_control=&other_app;
    Param other_open(2);
    Observe(&other_app_control,other_open.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x2020),&other_app_control,other_open.Reference());
    Check(g_last[1].valid&&g_last[1].transition.event_ns>closing_epoch,
          "another App has a newer opening with the same animation ID");
    Param late_closing_icon;Reverse(late_closing_icon);
    TuneParent(reinterpret_cast<void*>(0x3030),&bound_icon_control,late_closing_icon.Reference());
    Check(prepared_transition.leash_layer==app.id&&prepared_transition.event_ns==closing_epoch&&
          prepared_transition.closing&&g_icons[0].request_ns==closing_epoch,
          "late closing icon keeps its own App instead of borrowing a newer independent opening");
    Param same_app_reversal(2,78);
    Observe(&bound_app_control,same_app_reversal.Reference(),true);
    TuneParent(reinterpret_cast<void*>(0x4040),&bound_app_control,same_app_reversal.Reference());
    Param reversed_icon(1,78);
    TuneParent(reinterpret_cast<void*>(0x5050),&bound_icon_control,reversed_icon.Reference());
    Check(prepared_transition.leash_layer==app.id&&!prepared_transition.closing&&
          g_icons[0].request_ns>closing_epoch&&g_icons[0].animation_id==78,
          "same-App reversal advances the bound icon to its newer epoch");
    Stop(nullptr,&bound_app_control,78);
    const auto before_stale=prepared;
    Param orphaned_icon(1,77);Reverse(orphaned_icon);
    Param hidden_orphan(1,77);Reverse(hidden_orphan);
    SetSpring(hidden_orphan.Reference(),"alpha",0,0);
    SetVelocity(hidden_orphan.Reference(),"alpha",0);SetDelay(hidden_orphan.Reference(),"alpha",0);
    TuneParent(reinterpret_cast<void*>(0x6060),&bound_icon_control,orphaned_icon.Reference());
    Check(prepared==before_stale&&alpha_writes==1&&Properties(orphaned_icon)==Properties(hidden_orphan),
          "stopped bound App suppresses its old custom clone while preserving stock geometry");
    // A new child has no previous App binding and still uses the current ID.
    g_icons[0]={};AddChild();Param fresh_icon(1,77);
    TuneParent(reinterpret_cast<void*>(0x7070),&bound_icon_control,fresh_icon.Reference());
    Check(prepared==before_stale+1&&prepared_transition.leash_layer==other_app.id&&
          g_icons[0].request_ns==g_last[1].transition.event_ns,
          "first publication of a new icon still pairs with the current App");
    Check(Spring(fresh_icon.Reference(),"alpha").start==1&&Spring(reversed_icon.Reference(),"alpha").start==1,
          "fresh publication and valid same-App reversal keep their visible alpha");
    // Every fallback keeps ordinary stock icons unchanged, but a previously
    // customized buffer cannot safely reuse its retained SELF with stock crop.
    for(int fallback=0;fallback<5;++fallback) for(bool customized:{false,true}) {
        Reset();Param fallback_source(2);Seed(fallback_source);AddChild();
        if(customized) {
            Param accepted_icon;
            TuneParent(reinterpret_cast<void*>(0x8080),&bound_icon_control,accepted_icon.Reference());
        }
        Param fallback_icon,expected_icon;
        if(fallback==0) g_last[0].valid=false;
        if(fallback==1) g_last[0].preparing=true;
        if(fallback==2) g_last[0].supported=false;
        if(fallback==3) {
            g_last[0].retry_pending=true;
            g_last[0].transition.event_ns=Now()-1600000000LL;
            if(customized) g_icons[0].request_ns=g_last[0].transition.event_ns;
        }
        if(fallback==4) for(Param* parameters:{&fallback_icon,&expected_icon}) {
            SetSpring(parameters->Reference(),"width",171,1120);
            SetSpring(parameters->Reference(),"height",171,1120);
            SetSpring(parameters->Reference(),"ratio",1,1);
        }
        if(customized) {
            SetSpring(expected_icon.Reference(),"alpha",0,0);
            SetVelocity(expected_icon.Reference(),"alpha",0);SetDelay(expected_icon.Reference(),"alpha",0);
        }
        const int prior_prepared=prepared;
        TuneParent(reinterpret_cast<void*>(0x9090),&bound_icon_control,fallback_icon.Reference());
        if(fallback==1) {
            // The synchronous prepare gap holds the presented viewport/crop;
            // it never hides the icon and never republishes its mesh.
            Check(prepared==prior_prepared&&alpha_writes==0&&!suppressed,
                  "the prepare gap never hides parent/icon and never republishes its mesh");
            for(const char* key:{"centerX","width","ratio","cornerTLX"}) {
                const auto spring=Spring(fallback_icon.Reference(),key);
                Check(spring.valid&&spring.start==spring.end&&spring.velocity==0&&spring.delay==0,
                      "the prepare gap holds presented viewport/crop instead of the bare OEM spring");
            }
            continue;
        }
        Check(prepared==prior_prepared&&Properties(fallback_icon)==Properties(expected_icon),
              "fallback changes only alpha of an established custom icon and never republishes its mesh");
        Check(alpha_writes==(customized?1:0),
              "only established custom fallback clones are hidden in the submitted transaction");
    }
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
    accept_preview=false;accept_group=true;publication_kind=genie::PublicationKind::OwnedClosingGroup;
    Param reused_close(2);Reverse(reused_close);
    Observe(&prestart_control,reused_close.Reference(),false);
    Check(g_last[0].transition.closing&&queued==1,"reused close waits for grouped first-frame preparation");
    TuneParent(reinterpret_cast<void*>(0x6789),&prestart_control,reused_close.Reference());
    Check(preview_prepared==1&&group_prepared==1,"reused close bypasses opening preview and prepares live group");
    Check(queued==2&&g_last[0].supported&&g_last[0].host_transaction_id==0x6789,"reused close publishes its own custom host epoch");
    Param closing_icon;Reverse(closing_icon);
    TuneParent(reinterpret_cast<void*>(0x789a),&icon_control,closing_icon.Reference());
    Check(prepared==2&&alpha_writes==0,"reused close keeps paired icon on the custom route");
    {
        const auto& geometry=g_last[0].transition.geometry;
        const auto rect=[](Param& p) {
            const float w=Spring(p.Reference(),"width").start,h=Spring(p.Reference(),"height").start;
            return std::array<float,4>{Spring(p.Reference(),"centerX").start-w/2,
                Spring(p.Reference(),"centerY").start-h/2,w,h};
        };
        const auto app_rect=rect(reused_close),icon_rect=rect(closing_icon);
        Check(app_rect[0]==geometry[0]&&app_rect[1]==geometry[1]&&
              app_rect[2]==geometry[2]&&app_rect[3]==geometry[3],
              "first closing host transform starts at the donor-presented request rect");
        Check(icon_rect==app_rect,
              "icon and App host transforms share one screen-space start at closing publication");
    }

    Reset();AnchorPrestart();accept_preview=false;Param rejected_preview(2);
    Observe(&prestart_control,rejected_preview.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,rejected_preview.Reference());
    Check(preview_prepared==1&&group_prepared==0&&queued==0,"failed preview cannot fall through to another publication route");
    CheckStock(rejected_preview);

    Reset();AnchorPrestart();accept_group=false;Param rejected_group(2);Reverse(rejected_group);
    Observe(&prestart_control,rejected_group.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,rejected_group.Reference());
    Check(preview_prepared==0&&group_prepared==1&&queued==0,"failed closing group never attempts opening preview or publishes geometry");
    CheckHeld(rejected_group);

    Reset();Param unanchored_close(2);Reverse(unanchored_close);
    Observe(&prestart_control,unanchored_close.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,unanchored_close.Reference());
    Check(preview_prepared==0&&group_prepared==1&&queued==0,"unanchored pre-start close requires a verified group instead of publishing unprepared geometry");
    CheckHeld(unanchored_close);
    Check(cancelled==1&&cancel_layer==prestart.id&&cancel_epoch==g_last[0].transition.event_ns,
          "unanchored pre-start close cancels the stale opening session with its exact epoch and no render dispatch");
    Reset();Param unanchored_open(2);
    Observe(&prestart_control,unanchored_open.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,unanchored_open.Reference());
    Check(queued==1&&cancelled==0&&g_last[0].supported,"an unanchored opening admission never cancels its own publication");
    AddChild();Param established_custom_icon;
    TuneParent(reinterpret_cast<void*>(0x5678),&icon_control,established_custom_icon.Reference());
    Check(prepared==1&&alpha_writes==0,"the unanchored opening establishes its custom icon before the failed close");
    Param clone_unready(2);Reverse(clone_unready);accept_clone=false;
    Observe(&prestart_control,clone_unready.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x2345),&prestart_control,clone_unready.Reference());
    Check(prepared==2,
          "the close observation re-primes the established icon mask when no closing registration can arrive");
    Check(prepared_transition.event_ns==g_last[0].transition.event_ns,
          "the mask re-prime carries the presented closing contour basis");
    Check(g_icons[0].request_ns<g_last[0].transition.event_ns,
          "the mask re-prime preserves the existing binding until a real registration");
    Param late_custom_icon;Reverse(late_custom_icon);
    TuneParent(reinterpret_cast<void*>(0x3456),&icon_control,late_custom_icon.Reference());
    Check(alpha_writes==0&&!suppressed,
          "an established custom icon in a failed close generation never takes the alpha0 hide");
    CheckHeld(late_custom_icon);
    CheckHeld(clone_unready);
    Param second_app_callback(2);Reverse(second_app_callback);
    TuneParent(reinterpret_cast<void*>(0x3567),&prestart_control,second_app_callback.Reference());
    CheckHeld(second_app_callback);
    Check(alpha_writes==0&&!suppressed,
          "second and later App host callbacks in a failed close generation hold, never run bare springs");
    Check(cancelled==1&&cancel_layer==prestart.id&&cancel_epoch==g_last[0].transition.event_ns&&
          group_prepared==0&&queued==1&&!g_last[0].supported,
          "a failed closing parameter clone still supersedes the live opening ordering on an unanchored pre-start leash");
    const auto close_epoch=g_last[0].transition.event_ns;
    const int before_land=prepared;
    Stop(nullptr,&prestart_control,77);
    Check(prepared==before_land+1,
          "landing re-primes the retained child inside the same completion");
    Check(g_icons[0].request_ns==close_epoch&&g_icons[0].closing&&g_icons[0].landed,
          "the retained child basis targets the landed closing epoch immediately");
    HomePayload landed_payload;HomeParams(&icon_listener,landed_payload.bytes.data(),88);
    Check(home_pointer!=landed_payload.bytes.data()&&home_bytes==HiddenReadback(landed_payload,{},0),
          "post-landing readback is the true landed square, not deferred to a later registration");
    Check(prepared==before_land+1,
          "no later unrelated registration participates in the landing convergence");
    Reset();Param stop_seed(2);Seed(stop_seed);AddChild();
    Param stop_icon;TuneParent(reinterpret_cast<void*>(0x5678),&icon_control,stop_icon.Reference());
    Check(prepared==1,"established icon is registered before the opening stop");
    const int before_stop=prepared;
    Stop(nullptr,&app_control,77);
    Check(prepared==before_stop+1,
          "the opening stop re-primes the established icon mask in its own transaction");
    Reset();Param batch_open(2);Seed(batch_open);AddChild();
    Param batch_icon;TuneParent(reinterpret_cast<void*>(0x5678),&icon_control,batch_icon.Reference());
    Check(prepared==1,"established icon before the OEM stop batch");
    const int before_batch=prepared;stop_prime_order.clear();
    Stop(nullptr,&icon_control,77);
    Check(prepared==before_batch+1,
          "the icon-parent stop re-primes the established icon mask before any close generation arrives");
    Check(stop_prime_order==std::vector<int>({0,1}),
          "the icon mask re-prime lands after the OEM stop in the same transaction");
    Reset();AnchorPrestart();accept_group=false;defer_group=true;Param pending_close(2);Reverse(pending_close);
    Observe(&prestart_control,pending_close.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,pending_close.Reference());
    Check(g_last[0].retry_pending&&!g_last[0].admitted&&queued==0&&
          Spring(pending_close.Reference(),"width").start==Spring(pending_close.Reference(),"width").end&&
          Spring(pending_close.Reference(),"width").velocity==0&&
          Spring(pending_close.Reference(),"ratio").start==Spring(pending_close.Reference(),"ratio").end&&
          Spring(pending_close.Reference(),"cornerTLX").start==Spring(pending_close.Reference(),"cornerTLX").end,
          "a deferred pending close holds its presented viewport and crop until its publication lands");
    Reset();Param gap_source(2);Seed(gap_source);AddChild();
    Param gap_app;Reverse(gap_app);Param gap_icon;Reverse(gap_icon);
    { std::lock_guard<std::mutex> lock(g_mutex); g_last[0].preparing=true; }
    const void* gap_app_control=&app;
    TuneParent(reinterpret_cast<void*>(0x1234),&gap_app_control,gap_app.Reference());
    const void* gap_icon_control=&icon;
    TuneParent(reinterpret_cast<void*>(0x2345),&gap_icon_control,gap_icon.Reference());
    Check(alpha_writes==0&&!suppressed,
          "the synchronous prepare gap never hides the presented parent or icon");
    for(auto* held:{&gap_app,&gap_icon}) for(const char* key:{"centerX","width","ratio","cornerTLX"}) {
        const auto spring=Spring(held->Reference(),key);
        Check(spring.valid&&spring.start==spring.end&&spring.velocity==0&&spring.delay==0,
              "the prepare gap holds parent and icon presented viewport instead of the bare OEM close spring");
    }

    Reset();Param fresh_preview(2);
    Observe(&prestart_control,fresh_preview.Reference(),false);
    TuneParent(reinterpret_cast<void*>(0x1234),&prestart_control,fresh_preview.Reference());
    Check(preview_prepared==1&&group_prepared==0&&queued==1,"unanchored opening retains preview-only publication");
    Check(g_last[0].supported&&g_last[0].host_transaction_id==0x1234,"fresh preview publishes its custom host");

    // A higher-ID reversal retains its presentation dynamics but must acquire
    // a publication and host for the newly accepted closing epoch.
    AnchorPrestart();accept_preview=accept_group=false;Param reversal(2);Reverse(reversal);
    const int next_animation=78;std::memcpy(reversal.object.data()+0x48,&next_animation,4);
    Observe(&prestart_control,reversal.Reference(),true);
    TuneParent(reinterpret_cast<void*>(0x9abc),&prestart_control,reversal.Reference());
    Check(g_last[0].adjusting&&g_last[0].transition.closing,"higher-ID reverse callback retains continuation dynamics");
    Check(preview_prepared==1&&group_prepared==1&&queued==1&&!g_last[0].admitted,
          "a reversal cannot borrow the opening preview's admission when its own group is unavailable");
    CheckHeld(reversal);
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
    Check(g_last[0].adjusting&&group_prepared==4&&queued==1&&!g_last[0].admitted,
          "a newer continuation keeps its dynamics but cannot inherit the preceding publication proof");
    CheckStock(continuing);

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
    Check(!genie::FindIconSource(g_last,77,Now()),"pending group cannot lend custom geometry to the icon");
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
