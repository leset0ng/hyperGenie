// SPDX-License-Identifier: Apache-2.0
// Runs the actual renderer with fake capabilities and real, empty SDK transactions.
#include <android/surface_control.h>
#include <atomic>
static std::atomic<int> renderer_applies{0};
static void ObserveRendererApply(ASurfaceTransaction* transaction) {
    ++renderer_applies;
    ASurfaceTransaction_apply(transaction);
}
#define ASurfaceTransaction_apply ObserveRendererApply
#include "../app/src/main/cpp/genie_content.cpp"
#undef ASurfaceTransaction_apply
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <signal.h>

namespace {
using namespace std::chrono_literals;
struct Event {
    int layer;
    bool clear;
    void* transaction;
    std::array<float,44> uniforms{};
};
struct CallbackCall { void* context; ASurfaceTransaction_OnComplete callback; };
std::mutex test_mutex;
std::condition_variable test_cv;
std::vector<Event> events;
std::vector<CallbackCall> commits;
std::vector<CallbackCall> completions;
std::map<std::pair<const void*, const void*>, int> references;
std::map<int, float> border_widths;
std::map<int, std::array<float,44>> layer_uniforms;
bool pause_post=false, post_paused=false, release_post=false, producer_ready=false;
int producer_parent=-1;
bool pause_layer=false, layer_paused=false, release_layer=false;
int paused_layer=-1;
int checks=0;

void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
void Wait(std::unique_lock<std::mutex>& lock,bool& condition,const char* message) {
    Check(test_cv.wait_for(lock,2s,[&] {return condition;}),message);
}
int Id(const void* pointer) { return static_cast<int>(reinterpret_cast<uintptr_t>(pointer)); }
void Strong(const void* surface,const void* owner,int delta) {
    std::lock_guard<std::mutex> lock(test_mutex);
    references[{surface,owner}]+=delta;
}
void Inc(const void* surface,const void* owner) {Strong(surface,owner,1);}
void Dec(const void* surface,const void* owner) {Strong(surface,owner,-1);}
int32_t Layer(const void* pointer) {
    std::unique_lock<std::mutex> lock(test_mutex);
    if(pause_layer && Id(pointer)==paused_layer) {
        pause_layer=false;layer_paused=true;test_cv.notify_all();
        Wait(lock,release_layer,"icon seed construction released within timeout");
    }
    if(Id(pointer)==producer_parent) {producer_ready=true;test_cv.notify_all();}
    return Id(pointer);
}
void* Post(void* transaction,const void* surface,const void* token,const void* payload,int32_t) {
    std::unique_lock<std::mutex> lock(test_mutex);
    const bool clear=*static_cast<void* const*>(token)==nullptr;
    const int layer=Id(*static_cast<void* const*>(surface));
    if(!clear) {
        const auto& bytes=*static_cast<const std::shared_ptr<std::vector<unsigned char>>*>(payload);
        Check(bytes && bytes->size()==sizeof(std::array<float,44>),"shader payload is complete");
        std::memcpy(layer_uniforms[layer].data(),bytes->data(),bytes->size());
    }
    events.push_back({layer,clear,transaction,{}});
    if(!clear) events.back().uniforms=layer_uniforms[layer];
    if(pause_post && !clear) {
        pause_post=false;post_paused=true;test_cv.notify_all();
        Wait(lock,release_post,"paused Frame released within timeout");
    }
    return transaction; // Neither fake capability reaches an Android SDK function.
}
void* Border(void* transaction,const void* surface,const BorderSettingsAbi* settings) {
    std::lock_guard<std::mutex> lock(test_mutex);
    const int layer=Id(*static_cast<void* const*>(surface));
    Check(!events.empty() && events.back().layer==layer && events.back().transaction==transaction,
          "CLIENT guard and SELF attach/detach share the same surface transaction");
    Check(settings->width==(events.back().clear?0.0f:kClientBorderWidth),
          "every SELF attach has positive CLIENT guard and every detach clears it");
    Check(settings->color==0,"CLIENT guard always has zero RGBA, including retirement");
    border_widths[layer]=settings->width;
    return transaction;
}
void OnCommit(ASurfaceTransaction*,void* context,ASurfaceTransaction_OnCommit callback) {
    std::lock_guard<std::mutex> lock(test_mutex);
    commits.push_back({context,callback});test_cv.notify_all();
}
void OnComplete(ASurfaceTransaction*,void* context,ASurfaceTransaction_OnComplete callback) {
    std::lock_guard<std::mutex> lock(test_mutex);
    completions.push_back({context,callback});
}
void Invoke(std::vector<CallbackCall>& calls,size_t index) {
    CallbackCall call;
    {std::lock_guard<std::mutex> lock(test_mutex);
     Check(index<calls.size() && calls[index].callback,"callback exists and has not fired");
     call=calls[index];calls[index].callback=nullptr;}
    uint64_t opaque_stats=0;
    // Production callbacks never dereference stats or forward it to the SDK.
    call.callback(call.context,reinterpret_cast<ASurfaceTransactionStats*>(&opaque_stats));
}
void DrainCompletions() {
    for(size_t index=0;;++index) {
        bool pending;
        {std::lock_guard<std::mutex> lock(test_mutex);
         if(index>=completions.size()) break;
         pending=completions[index].callback!=nullptr;}
        if(pending) Invoke(completions,index);
    }
}
Controller& Fresh() {
    DrainCompletions();
    // Mask-only OEM previews intentionally outlive their renderer Session.
    // Model platform removal between fixtures with the authenticated hidden-
    // surface cleanup API, which must reject any capability still actively owned.
    if(g_controller.load()) {
        auto* removal=ASurfaceTransaction_create();Check(removal,"create fixture preview-removal transaction");
        for(const auto& entry:border_widths) if(entry.second>0.0f)
            Check(Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(nullptr,nullptr,
                reinterpret_cast<jlong>(removal),entry.first),"fixture removal cannot clear a live effect owner");
        ASurfaceTransaction_apply(removal);ASurfaceTransaction_delete(removal);
    }
    // Controllers intentionally live until executable exit, as in production.
    auto* controller=new Controller;
    controller->shader=reinterpret_cast<void*>(9000);
    g_controller.store(controller);
    std::lock_guard<std::mutex> lock(test_mutex);
    for(const auto& commit:commits) Check(!commit.callback,"previous fixture consumed every fake commit");
    for(const auto& completion:completions) Check(!completion.callback,"previous fixture drained every completion");
    for(const auto& entry:border_widths) Check(entry.second==0.0f,"previous fixture cleared every CLIENT guard");
    border_widths.clear();
    events.clear();commits.clear();completions.clear();
    pause_post=post_paused=release_post=producer_ready=false;producer_parent=-1;
    pause_layer=layer_paused=release_layer=false;paused_layer=-1;
    return *controller;
}
std::array<float,12> Geometry(int width,int height) {
    return {0,0,static_cast<float>(width),static_cast<float>(height),
            80,800,171,171,0,0,100,40};
}
bool Queue(int layer,int parent,int width=1120,int height=2436,
           const std::vector<jlong>& companions={},void* initial=nullptr) {
    return QueueSession(layer,parent,width,height,Geometry(width,height),true,NowNs(),
                        companions,false,0,0,initial);
}
void Pump(Controller& controller) {
    std::lock_guard<std::mutex> lock(controller.mutex);
    ConsumeCommand(controller);RetireCommitted(controller);
}
size_t Prime(int layer,int parent,const std::vector<jlong>& companions={}) {
    size_t index;
    {std::lock_guard<std::mutex> lock(test_mutex);index=commits.size();}
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create empty transaction");
    Check(Queue(layer,parent,1120,2436,companions,transaction),"queue primed session");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    {std::lock_guard<std::mutex> lock(test_mutex);
     Check(commits.size()==index+1,"initial submission records one commit callback");}
    return index;
}
void Commit(size_t index) {Invoke(commits,index);}
std::vector<int> Cleared() {
    std::lock_guard<std::mutex> lock(test_mutex);
    std::vector<int> result;
    for(const auto& event:events) if(event.clear) result.push_back(event.layer);
    return result;
}
void ResetEvents() {std::lock_guard<std::mutex> lock(test_mutex);events.clear();}
void Stop(Controller& controller) {
    Java_ng_lst_genie_ContentAnimator_nativeStop(nullptr,nullptr);Pump(controller);
}
void FramePrimeOrdering() {
    auto& controller=Fresh();Check(Queue(101,1001),"seed old session");Pump(controller);
    controller.active->started_ns=NowNs()-2000000000LL; // Old Frame must retire at deadline.
    ResetEvents();
    {std::lock_guard<std::mutex> lock(test_mutex);pause_post=true;producer_parent=1002;}
    std::thread frame([&] {Frame(NowNs(),&controller);});
    {std::unique_lock<std::mutex> lock(test_mutex);
     Wait(lock,post_paused,"old Frame reached fake post");}
    const bool acquired=controller.mutex.try_lock();
    if(acquired) controller.mutex.unlock();
    Check(!acquired,"actual Frame holds controller lock across SF writes and retirement");
    std::atomic<bool> accepted{false};
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create producer transaction");
    std::thread producer([&] {accepted=Queue(101,1002,1120,2436,{},transaction);});
    {std::unique_lock<std::mutex> lock(test_mutex);
     Wait(lock,producer_ready,"producer built successor before attempting publication");
     Check(events.size()==1,"new prime has not overtaken paused old Frame");
     release_post=true;test_cv.notify_all();}
    frame.join();producer.join();Check(accepted,"producer accepted after old Frame retired");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    {std::lock_guard<std::mutex> lock(test_mutex);
     Check(events.size()==3 && !events[0].clear && events[1].clear && !events[2].clear,
           "old write then old clear precede successor prime on same layer");}
    Pump(controller);Check(!controller.active && controller.pending,"commit gate defers adoption");
    Commit(0);Pump(controller);Check(controller.active && controller.active->layer==101,
                                   "successor adopted after commit");
    Stop(controller);std::puts("PASS Frame/prime serialization");
}
void ReplacementPreservesSharedLayers() {
    auto& controller=Fresh();Check(Queue(201,2001,1120,2436,{202,203}),"seed shared layers");
    Pump(controller);ResetEvents();
    Check(Queue(201,2001,1080,2340,{202,204}),"queue actual extent-changing replacement");
    Pump(controller);
    Check(controller.active && controller.active->width==1080,"replacement extent adopted");
    Check(Cleared().empty(),"extent replacement preserves lost starting preview's final mask");
    Check(border_widths[201]>0 && border_widths[202]>0 && border_widths[203]>0 && border_widths[204]>0,
          "replacement keeps shared guards and the still-visible lost starting preview mask");
    Stop(controller);std::puts("PASS extent replacement retains shared layers");
}
void ScopedCancellation() {
    auto& controller=Fresh();Check(Queue(301,3001),"seed current B");Pump(controller);
    genie::RenderIdentity b{controller.active->parent_layer,controller.active->request_ns};
    genie::CancelIconRenderer({3000,b.request_ns-1});
    genie::CancelIconRenderer({b.parent,b.request_ns-1});
    std::this_thread::sleep_for(60ms);Frame(NowNs(),&controller);
    Check(controller.active && controller.active->layer==301,"old parent and epoch cannot cancel B");
    const int64_t requested=NowNs();
    genie::CancelIconRenderer(b);
    {std::lock_guard<std::mutex> lock(controller.mutex);
     Check(controller.active && controller.active->cancel_ns>=requested+47000000LL,
           "matching B records a scoped cancellation without changing its watchdog");}
    const auto first_cancel=controller.active->cancel_ns;
    genie::CancelIconRenderer(b);
    Check(controller.active->cancel_ns==first_cancel,
          "repeated exact cancellation keeps the first grace deadline before it has elapsed");
    std::this_thread::sleep_for(60ms);ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active && Cleared().empty() && border_widths[301]>0,
          "closing B remains protected after stop because SF stop does not hide it");
    controller.active->started_ns=NowNs()-2000000000LL;
    Frame(NowNs(),&controller);
    Check(!controller.active && Cleared()==std::vector<int>{301},
          "stopped closing B retires at its existing deadline");
    // Pending replacement also wins over an old active stop.
    Check(Queue(302,3002),"seed another active session");Pump(controller);
    genie::RenderIdentity old{controller.active->parent_layer,controller.active->request_ns};
    auto next=Prime(303,3003);genie::CancelIconRenderer(old);Commit(next);Pump(controller);
    std::this_thread::sleep_for(60ms);Frame(NowNs(),&controller);
    Check(controller.active && controller.active->layer==303,"pending successor survives old active stop");
    Stop(controller);std::puts("PASS scoped cancellation and grace");
}
void PendingRetirement() {
    auto& controller=Fresh();auto a=Prime(401,4001,{402,403});
    auto b=Prime(402,4002,{403});
    Check(controller.retiring.size()==1,"superseded primed A retained until commit");
    ResetEvents();Commit(a);Pump(controller);
    Check(Cleared().empty() && controller.pending && controller.retiring.size()==1,
          "A commit cannot clear incoming uncommitted B");
    Commit(b);Pump(controller);
    Check(controller.active && controller.active->layer==402,"B adopted after its commit");
    Check(controller.retiring.empty() && Cleared()==std::vector<int>{401},
          "retiring A clears only lost primary, preserves B primary and companion");
    Stop(controller);
    // Reverse commit order: late A cleanup must still preserve already-active B.
    auto& reversed=Fresh();a=Prime(411,4101,{412,413});b=Prime(412,4102,{413});
    Commit(b);Pump(reversed);ResetEvents();Commit(a);Pump(reversed);
    Check(Cleared()==std::vector<int>{411} && reversed.active->layer==412,
          "late superseded commit preserves active successor");
    Stop(reversed);
    auto& canceled=Fresh();a=Prime(421,4201);ResetEvents();Stop(canceled);
    Check(!canceled.active && canceled.retiring.size()==1 && Cleared().empty(),
          "canceled uncommitted prime remains owned for eventual cleanup");
    Commit(a);Pump(canceled);
    Check(canceled.retiring.empty() && Cleared()==std::vector<int>{421},
          "canceled pending prime cleared after eventual commit");
    std::puts("PASS superseded and canceled pending-prime retirement");
}
bool Running() {return Java_ng_lst_genie_ContentAnimator_nativeRunning(nullptr,nullptr)==JNI_TRUE;}
bool RequestRunning(int64_t request) {
    return Java_ng_lst_genie_ContentAnimator_nativeRequestRunning(nullptr,nullptr,request)==JNI_TRUE;
}
void JniStopRequestValidation() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    Check(QueueSession(311,3101,1120,2436,opening,false,NowNs(),{},false),
          "seed opening request for JNI stop validation");
    Pump(controller);
    const auto request=controller.active->request_ns;
    // The last value would alias the live parent if JNI truncated before checking.
    const jlong invalid_parents[]={0,-1,static_cast<jlong>(INT32_MAX)+1,(jlong{1}<<32)+3101};
    for(jlong parent:invalid_parents) {
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,parent,request);
        Check(controller.active->cancel_ns==0,"JNI rejects invalid or overflowing parent without canceling the live request");
    }
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3101,request-1);
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3102,request);
    Check(controller.active->cancel_ns==0,"JNI stop requires both current parent and immutable request epoch");
    const auto before=NowNs();
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3101,request);
    const auto after=NowNs();
    Check(controller.active->cancel_ns>=before+48000000LL &&
          controller.active->cancel_ns<=after+48000000LL,
          "JNI stop uses the same 48ms scoped grace as the native API");
    ResetEvents();
    auto* delayed_publication=ASurfaceTransaction_create();
    Check(!QueueSession(312,3101,1120,2436,opening,false,request,{},true,0,0,delayed_publication),
          "revoked epoch cannot prime newly discovered ordinary content before grace expires");
    Check(!QueueSession(313,3101,1120,2436,opening,false,request,{},false),
          "revoked epoch cannot queue ordinary worker content before grace expires");
    Check(events.empty()&&!controller.pending,"rejected late content writes no shader and retains no pending owner");
    ASurfaceTransaction_delete(delayed_publication);
    // Advance cancellation deterministically without depending on wall-clock scheduling.
    controller.active->cancel_ns=NowNs()-1;
    ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active && Cleared().empty() && Running(),
          "expired cancellation grace pauses opening while preserving its owned presentation");
    controller.active->started_ns=NowNs()-1500000001LL;
    Frame(NowNs(),&controller);
    Check(!controller.active && Cleared()==std::vector<int>{311} && Running(),
          "original opening deadline clears its effect but remains busy until detach completion");
    DrainCompletions();Check(!Running(),"scoped opening stop becomes idle after its own detach completes");
    std::puts("PASS JNI stop range, identity, and opening retirement");
}
void OwnershipRevocation() {
    auto& controller=Fresh();
    Check(Queue(321,3201,1120,2436,{322}),"seed closing owner with a retained preview");
    Pump(controller);
    const auto request=controller.active->request_ns;
    for(jlong parent:{jlong{0},jlong{-1},static_cast<jlong>(INT32_MAX)+1,(jlong{1}<<32)+3201})
        Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,parent,request);
    Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3201,request-1);
    Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3202,request);
    Check(controller.active && controller.retired_requests.empty(),
          "revoke validates parent range and exact epoch without poisoning the current request");
    Check(Queue(323,3203),"seed an independently closing App");Pump(controller);
    ResetEvents();
    Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3201,request);
    auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
    Check(controller.active && controller.active->layer==323 && controller.overlapping.empty(),
          "ownership revoke removes an overlapping old close while keeping the independent owner");
    Check(cleared==std::vector<int>{321} && border_widths[322]>0,
          "revocation retires ordinary App input but freezes the retained preview's last mask");
    auto* late=ASurfaceTransaction_create();Check(late,"create a late revoked content transaction");
    Check(!AttachSession(late,324,3201,1120,2436,request),"revoked owner cannot attach a new visible buffer");
    Check(!QueueSession(324,3201,1120,2436,Geometry(1120,2436),true,request,{},true,0,0,late),
          "revoked close cannot return through a delayed primed discovery");
    ASurfaceTransaction_delete(late);
    ResetEvents();Frame(NowNs(),&controller);
    for(const auto& event:events) Check(event.layer==323,"revoked close never writes another frame");
    Stop(controller);

    auto& canceled=Fresh();Check(Queue(325,3205),"seed ordinary canceled close");Pump(canceled);
    const auto stopped=canceled.active->request_ns;
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3205,stopped);
    late=ASurfaceTransaction_create();ResetEvents();
    Check(!AttachSession(late,326,3205,1120,2436,stopped) && events.empty(),
          "ordinary cancellation also forbids late show attachments during its silhouette grace");
    ASurfaceTransaction_delete(late);Stop(canceled);
    std::puts("PASS exact ownership revoke ends closing writers and rejects late discovery");
}
void OpeningCancelKeepsPreviewMasks() {
    for(bool starting_primary:{false,true}) {
        auto& controller=Fresh();
        auto geometry=Geometry(1120,2436);
        for(size_t i=0;i<4;++i) std::swap(geometry[i],geometry[i+4]);
        std::swap(geometry[10],geometry[11]);
        const auto request=NowNs();
        Check(QueueSession(327,3207,1120,2436,geometry,false,request,{328},starting_primary),
              "seed opening owner with a verified still-visible starting/color companion");
        Pump(controller);
        auto* show=ASurfaceTransaction_create();Check(show,"create late OEM color show transaction");
        Check(Java_ng_lst_genie_ContentAnimator_nativeAttach(nullptr,nullptr,
            reinterpret_cast<jlong>(show),329,3207,1120,2436,request,JNI_TRUE),
            "JNI attachment retains authenticated starting/color metadata");
        ASurfaceTransaction_apply(show);ASurfaceTransaction_delete(show);Commit(0);Pump(controller);
        const auto preview_uniforms=layer_uniforms[328];
        const auto late_preview_uniforms=layer_uniforms[329];
        const auto primary_uniforms=layer_uniforms[327];
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        controller.active->cancel_ns=NowNs()-1;
        const auto state=controller.active->animation.Scalars();
        const auto last=controller.active->last_ns;
        const auto frame_count=controller.active->frames;
        const auto applies=renderer_applies.load();
        ResetEvents();Frame(NowNs(),&controller);
        Check(controller.active && controller.retiring.empty() && events.empty() &&
              renderer_applies.load()==applies && controller.active->last_ns==last &&
              controller.active->frames==frame_count,
              "ordinary opening cancel retains paused motion without advancing or submitting a frame");
        for(size_t i=0;i<state.size();++i)
            Check(controller.active->animation.Scalars()[i].value==state[i].value &&
                  controller.active->animation.Scalars()[i].velocity==state[i].velocity,
                  "cancelled opening preserves every presented spring and velocity");
        Check(border_widths[328]>0 && layer_uniforms[328]==preview_uniforms &&
              border_widths[329]>0 && layer_uniforms[329]==late_preview_uniforms,
              "opening cancel freezes primed and late-attached OEM starting/color masks");
        Check(Cleared().empty() && border_widths[327]>0 && layer_uniforms[327]==primary_uniforms,
              "paused primary keeps its last presentation until retarget or its existing deadline");
        ResetEvents();Frame(NowNs(),&controller);
        Check(events.empty() && renderer_applies.load()==applies,
              "additional paused callbacks submit no surface writes or empty transaction applies");
        const auto cancel_deadline=controller.active->cancel_ns;
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        Frame(NowNs(),&controller);
        Check(controller.active->cancel_ns==cancel_deadline && events.empty() &&
              renderer_applies.load()==applies && controller.active->last_ns==last &&
              controller.active->frames==frame_count,
              "repeated same-epoch cancellation neither resumes paused frames nor extends its grace period");
        for(size_t i=0;i<state.size();++i)
            Check(controller.active->animation.Scalars()[i].value==state[i].value &&
                  controller.active->animation.Scalars()[i].velocity==state[i].velocity,
                  "repeated cancellation leaves every paused coordinate and velocity unchanged");
        controller.active->started_ns=NowNs()-1500000001LL;
        Frame(NowNs(),&controller);
        Check(!controller.active && controller.retiring.empty(),
              "opening cancellation does not renew the original retirement deadline");
        if(starting_primary) Check(Cleared().empty() && border_widths[327]>0 && layer_uniforms[327]==primary_uniforms,
              "starting primary preserves its last mask after paused deadline retirement");
        else Check(Cleared()==std::vector<int>{327} && border_widths[327]==0,
              "ordinary main App input clears its shader at the original paused deadline");
    }
    // Exercise the actual three continuation entrypoints: initial host prime,
    // worker adoption, and an authenticated icon adjust preceding App dispatch.
    for(int path=0;path<5;++path) {
        auto& controller=Fresh();
        auto opening=Geometry(1120,2436);
        for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
        std::swap(opening[10],opening[11]);
        const auto request=NowNs()-200000000LL;
        Check(QueueSession(327,3207,1120,2436,opening,false,request,{},true),
              "seed opening before a cancelled host resumes as closing");
        Pump(controller);
        auto* source=controller.active.get();
        source->animation.Advance(.07);genie::model::AdvanceScalar(&source->corner,.07,1.0);
        auto* icon_transaction=ASurfaceTransaction_create();Check(icon_transaction,"create registered icon publication");
        Check(PrimeLinkedIcon(icon_transaction,328,3208,171,171,1120,2436,opening,false,request),
              "register exact icon capability before cancellation");
        ASurfaceTransaction_apply(icon_transaction);ASurfaceTransaction_delete(icon_transaction);Commit(0);
        source->last_ns=NowNs()-100000000LL;
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3206,request);
        Check(source->cancel_ns==0,"wrong parent cannot pause the opening stream");
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        source->cancel_ns=NowNs()-1;
        ResetEvents();Frame(NowNs(),&controller);
        Check(controller.active.get()==source && events.empty(),"cancelled source remains available without writes");
        const auto state=source->animation.Scalars();
        const auto corner=source->corner;
        auto* stale=ASurfaceTransaction_create();Check(stale,"create stale publication after cancel");
        Check(!QueueSession(327,3207,1120,2436,opening,false,request,{},true,0,0,stale) &&
              !AttachSession(stale,329,3207,1120,2436,request,true) &&
              !PrimeLinkedIcon(stale,328,3208,171,171,1120,2436,opening,false,request) && events.empty(),
              "paused source keeps its tombstone and rejects every old-epoch publication entrypoint");
        ASurfaceTransaction_delete(stale);
        const auto next_request=NowNs();
        const auto before_resume=NowNs();
        const bool prime=path<2 || path==4;
        auto* transaction=prime?ASurfaceTransaction_create():nullptr;
        if(prime) Check(transaction,"create resumed closing host publication");
        const auto commit=commits.size();
        const int next_layer=(path==1 || path==3)?329:327;
        if(path==4) Check(PrimeLinkedIcon(transaction,328,3208,171,171,1120,2436,Geometry(1120,2436),
                                        true,next_request,request,3207),
                         "newer exact icon capability resumes the paused App stream");
        else Check(QueueSession(next_layer,3207,1120,2436,Geometry(1120,2436),true,next_request,{},true,
                                0,0,transaction),"newer closing request resumes same primary or preview handoff");
        if(prime) {ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);Commit(commit);}
        Pump(controller);
        auto& resumed=*controller.active;
        Check(resumed.request_ns==next_request && resumed.closing && resumed.cancel_ns==0 &&
              resumed.started_ns==next_request && resumed.last_ns>=before_resume && resumed.last_ns<=NowNs(),
              "resumed close starts its own deadline and excludes the paused interval from its next frame delta");
        for(size_t i=0;i<state.size();++i)
            Check(resumed.animation.Scalars()[i].value==state[i].value &&
                  resumed.animation.Scalars()[i].velocity==state[i].velocity,
                  "all continuation paths preserve paused coordinates and velocities exactly");
        Check(resumed.corner.value==corner.value && resumed.corner.velocity==corner.velocity,
              "all continuation paths preserve the paused corner spring exactly");
        auto expected=resumed.animation;
        auto expected_corner=resumed.corner;
        expected.Advance(.016);genie::model::AdvanceScalar(&expected_corner,.016,1.0);
        Frame(resumed.last_ns+16000000LL,&controller);
        for(size_t i=0;i<state.size();++i)
            Check(resumed.animation.Scalars()[i].value==expected.Scalars()[i].value &&
                  resumed.animation.Scalars()[i].velocity==expected.Scalars()[i].velocity,
                  "first resumed frame integrates only the new display interval without pause catch-up");
        Check(resumed.corner.value==expected_corner.value && resumed.corner.velocity==expected_corner.velocity,
              "first resumed corner frame also excludes paused time");
        Stop(controller);
    }
    for(bool expired_during_lock:{false,true}) {
        auto& controller=Fresh();
        auto opening=Geometry(1120,2436);
        for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
        std::swap(opening[10],opening[11]);
        const auto request=NowNs()-200000000LL;
        Check(QueueSession(327,3207,1120,2436,opening,false,request,{329},true),
              "seed opening before a producer crosses cancellation while awaiting the owner lock");
        Pump(controller);
        auto* source=controller.active.get();
        source->animation.Advance(.07);genie::model::AdvanceScalar(&source->corner,.07,1.0);
        source->last_ns=NowNs()-100000000LL;
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        const auto state=source->animation.Scalars();
        const auto corner=source->corner;
        auto* transaction=ASurfaceTransaction_create();Check(transaction,"create waiting closing publication");
        const auto next_request=NowNs();
        const auto commit=commits.size();
        std::atomic<bool> accepted{false};
        std::unique_lock<std::mutex> owner_lock(controller.mutex);
        {std::lock_guard<std::mutex> lock(test_mutex);pause_layer=true;paused_layer=327;}
        std::thread producer([&] {
            accepted=QueueSession(327,3207,1120,2436,Geometry(1120,2436),true,next_request,
                                  {},true,0,0,transaction);
        });
        {
            std::unique_lock<std::mutex> lock(test_mutex);
            Wait(lock,layer_paused,"closing producer sampled its admission clock before the owner lock");
            source->cancel_ns=NowNs();
            if(expired_during_lock) source->started_ns=source->cancel_ns-1500000000LL;
            release_layer=true;test_cv.notify_all();
        }
        owner_lock.unlock();producer.join();
        Check(accepted && controller.pending,"waiting producer revalidates and publishes under the owner lock");
        if(expired_during_lock) {
            Check(!controller.pending->primed_continuation && !Owns(*controller.pending,329) &&
                  controller.pending->animation.Scalars()[18].value!=state[18].value,
                  "clock refreshed after lock acquisition rejects motion that expired while the producer waited");
        } else {
            for(size_t i=0;i<state.size();++i)
                Check(controller.pending->animation.Scalars()[i].value==state[i].value &&
                      controller.pending->animation.Scalars()[i].velocity==state[i].velocity,
                      "clock refreshed after lock acquisition preserves motion paused while the producer waited");
            Check(controller.pending->corner.value==corner.value &&
                  controller.pending->corner.velocity==corner.velocity &&
                  controller.pending->last_ns>=source->cancel_ns,
                  "waiting continuation preserves its paused corner and starts a current frame clock");
        }
        ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);Commit(commit);Pump(controller);
        Stop(controller);
    }
    for(bool pending_source:{false,true}) {
        auto& controller=Fresh();
        auto opening=Geometry(1120,2436);
        for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
        std::swap(opening[10],opening[11]);
        const auto request=NowNs()-200000000LL;
        auto* old_transaction=pending_source?ASurfaceTransaction_create():nullptr;
        if(pending_source) Check(old_transaction,"create expired pending opening publication");
        Check(QueueSession(327,3207,1120,2436,opening,false,request,{329},true,0,0,old_transaction),
              "seed opening whose cancelled continuation will expire");
        if(pending_source) {
            ASurfaceTransaction_apply(old_transaction);ASurfaceTransaction_delete(old_transaction);
        } else Pump(controller);
        auto* source=pending_source?controller.pending.get():controller.active.get();
        source->animation.Advance(.07);
        const auto expired_state=source->animation.Scalars();
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        source->cancel_ns=NowNs()-1;
        source->started_ns=NowNs()-1500000001LL;
        Check(!QueueSession(327,3207,1120,2436,opening,false,request-1,{},true),
              "expired paused ownership still rejects an older request on the same stream");
        auto* transaction=ASurfaceTransaction_create();Check(transaction,"create authorized fresh closing publication");
        const auto next_request=NowNs();
        const auto next_commit=commits.size();
        Check(QueueSession(327,3207,1120,2436,Geometry(1120,2436),true,next_request,{},false,0,0,transaction),
              "fresh App publication may replace an expired cancelled opening");
        Check(controller.pending && !controller.pending->primed_continuation &&
              !Owns(*controller.pending,329) &&
              controller.pending->animation.Scalars()[18].value!=expired_state[18].value,
              "expired paused owner contributes neither motion nor unregistered old companions");
        ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);Commit(next_commit);
        Pump(controller);
        if(pending_source) {
            Check(!controller.active && controller.pending && !Committed(*controller.pending) && RequestRunning(request),
                  "expired cancelled source still protects the successor from its outstanding borrowed publication");
            Commit(0);Pump(controller);
        }
        Check(controller.active && controller.active->request_ns==next_request && controller.retiring.empty(),
              "fresh successor starts after all expired predecessor publication dependencies complete");
        Check(RequestRunning(request),"same-primary fresh successor retains the old epoch's cleanup identity");
        Stop(controller);
    }
    for(bool revoke:{false,true}) {
        auto& controller=Fresh();
        auto opening=Geometry(1120,2436);
        for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
        const auto request=NowNs();
        Check(QueueSession(327,3207,1120,2436,opening,false,request,{},true),
              "seed paused opening for explicit ownership retirement");
        Pump(controller);Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,3207,request);
        controller.active->cancel_ns=NowNs()-1;Frame(NowNs(),&controller);
        Check(controller.active!=nullptr,"pause remains live before explicit retirement");
        if(revoke) Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3207,request);
        else Stop(controller);
        Check(!controller.active && !controller.pending && controller.retiring.empty(),
              "explicit disable and ownership revoke retire a paused opening immediately");
    }
    std::puts("PASS opening cancellation pauses bounded motion and preserves authenticated reversal continuity");
}

void RevocationCommitOrdering() {
    for(bool revoke_before_successor:{false,true}) {
        for(bool successor_first:{false,true}) {
            auto& controller=Fresh();auto first=Prime(331,3301,{332});
            const auto request=controller.pending->request_ns;
            size_t successor;
            if(revoke_before_successor) {
                Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3301,request);
                Check(!controller.pending && controller.retiring.size()==1,
                      "uncommitted revoked publication is retained solely for retirement");
                successor=Prime(331,3301);
                Check(controller.pending && !Owns(*controller.pending,332) && !controller.pending->primed_continuation,
                      "new owner does not inherit a revoked preview or its motion");
            } else {
                successor=Prime(331,3301,{332});
                Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,3301,request);
            }
            Check(controller.pending && controller.pending->request_ns!=request,
                  "revoke preserves the newer pending successor on the same host");
            Check(std::find(controller.retired_requests.begin(),controller.retired_requests.end(),request)!=
                      controller.retired_requests.end(),"revoke tombstones an older prime already in retirement");
            ResetEvents();
            if(successor_first) {Commit(successor);Pump(controller);Commit(first);}
            else {Commit(first);Pump(controller);Commit(successor);}
            Pump(controller);
            Check(controller.active && controller.active->layer==331 && controller.retiring.empty(),
                  "successor adopts after every borrowed transaction commits in either order");
            for(int layer:Cleared()) Check(layer!=331,"old retirement cannot clear the successor's shared App");
            Check(border_widths[331]>0,"shared App keeps SELF and CLIENT through revocation");
            if(revoke_before_successor) Check(border_widths[332]>0 && !Owns(*controller.active,332),
                "revoked preview freezes without joining the successor's motion owner");
            Stop(controller);
        }
    }
    std::puts("PASS revocation and late publication preserve successor commit ownership");
}

void IndependentDetachCompletions() {
    for(size_t first:{size_t{0},size_t{1}}) {
        auto& controller=Fresh();auto a=Prime(501,5001);
        const auto a_request=controller.pending->request_ns;
        auto b=Prime(502,5002);
        const auto b_request=controller.pending->request_ns;
        Check(RequestRunning(a_request) && RequestRunning(b_request),
              "independent queued and pending primes each retain their epoch before commit");
        Stop(controller);
        Check(RequestRunning(a_request) && RequestRunning(b_request),
              "cancelled uncommitted primes stay busy until their borrowed publications retire");
        Commit(a);Commit(b);Pump(controller);
        Check(!controller.active && !controller.pending && controller.retiring.empty(),
              "stopped primes leave no active, pending, or retiring session");
        Check(Cleared()==std::vector<int>({501,502}) && completions.size()==2 &&
              controller.detaches_pending.load()==2,"two primes submit independent detach transactions");
        Check(Running() && RequestRunning(a_request) && RequestRunning(b_request),
              "each epoch remains busy before its own detach completes");
        {std::lock_guard<std::mutex> lock(controller.mutex);Invoke(completions,first);}
        Check(!completions[first].callback && completions[1-first].callback,
              "completing one detach leaves the other transaction's completion pending");
        Check(controller.detaches_pending.load()==1 && Running(),
              "first detach completion cannot release snapshot guard while second remains");
        Check(!RequestRunning(first==0?a_request:b_request) &&
              RequestRunning(first==0?b_request:a_request),
              "completed epoch is released independently of the other detach's completion order");
        Invoke(completions,1-first);
        Check(controller.detaches_pending.load()==0 && !Running() &&
              !RequestRunning(a_request) && !RequestRunning(b_request),
              "renderer idle only after both independent detaches complete");
    }
    for(size_t first:{size_t{0},size_t{1}}) {
        auto& repeated=Fresh();Check(Queue(503,5003),"seed repeated retirement for one epoch");
        Pump(repeated);
        const auto request=repeated.active->request_ns;
        Check(Submit(repeated,*repeated.active,true),"submit the first detach for the epoch");
        Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,5003,request);
        Check(!repeated.active && repeated.retiring.empty() && completions.size()==2 &&
              repeated.detaches_pending.load()==2 && RequestRunning(request),
              "a second detach for the same epoch retains both completion barriers");
        Invoke(completions,first);
        Check(RequestRunning(request) && repeated.detaches_pending.load()==1,
              "one same-epoch completion cannot release the other outstanding detach");
        Invoke(completions,1-first);
        Check(!RequestRunning(request) && !Running(),
              "same-epoch retirement ends only after every completion in either order");
    }
    auto& independent=Fresh();Check(Queue(505,5005),"seed App A before independent cleanup");
    Pump(independent);
    const auto a_request=independent.active->request_ns;
    Check(Queue(506,5006),"start independent App B while A remains owned");Pump(independent);
    const auto b_request=independent.active->request_ns;
    Check(RequestRunning(a_request) && RequestRunning(b_request),
          "active and overlapping owners both retain their exact epochs");
    Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,5005,a_request);
    Check(RequestRunning(a_request) && RequestRunning(b_request) && completions.size()==1,
          "revoked A waits for its own clear while B remains active");
    Invoke(completions,0);
    Check(!RequestRunning(a_request) && RequestRunning(b_request) && Running(),
          "unrelated active B cannot pin A's completed group cleanup");
    Stop(independent);
    Check(!RequestRunning(a_request) && RequestRunning(b_request) && Running(),
          "unrelated B detach cannot reactivate A's completed cleanup barrier");
    Check(!RequestRunning(0) && !RequestRunning(-1) && !RequestRunning(b_request+1),
          "invalid and unowned epochs are idle while another epoch remains busy");
    DrainCompletions();Check(!RequestRunning(b_request) && !Running(),
                             "independent B becomes idle after its own detach completes");
    auto& replaced=Fresh();Check(Queue(507,5007),"seed owner before dropping an ordinary companion");
    Pump(replaced);
    const auto old_request=replaced.active->request_ns;
    auto* companion_show=ASurfaceTransaction_create();Check(companion_show,"create ordinary companion show");
    Check(AttachSession(companion_show,508,5007,1120,2436,old_request),
          "attach an ordinary companion that is not retained by the next epoch");
    ASurfaceTransaction_apply(companion_show);ASurfaceTransaction_delete(companion_show);Commit(0);
    const auto next_request=NowNs();
    Check(QueueSession(507,5007,1120,2436,Geometry(1120,2436),true,next_request,{},false),
          "same-layer successor drops the preceding ordinary companion");Pump(replaced);
    Check(completions.size()==1 && RequestRunning(old_request) && RequestRunning(next_request),
          "lost companion detach remains attributed to the old epoch after same-layer adoption");
    Invoke(completions,0);
    Check(RequestRunning(old_request) && RequestRunning(next_request) &&
          replaced.detaches_pending.load()==0,
          "lost companion completion does not release cleanup while the same primary still has a writer");
    Stop(replaced);DrainCompletions();
    Check(!RequestRunning(old_request) && !RequestRunning(next_request),
          "same-primary cleanup epochs end after their successor's final detach");
    auto& retained=Fresh();auto a=Prime(511,5101,{512});auto b=Prime(511,5102,{512});
    Commit(a);Commit(b);ResetEvents();Pump(retained);
    Check(retained.active && retained.retiring.empty() && Cleared().empty(),
          "fully retained pending prime retires without clearing successor");
    Check(completions.empty() && retained.detaches_pending.load()==0,
          "fully retained retirement registers no empty detach completion");
    Stop(retained);DrainCompletions();Check(!Running(),"retained successor eventually becomes idle");
    std::puts("PASS independent detach completions protect snapshot guard");
}
void OpeningPreviewLifetime() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const int64_t request=NowNs();
    Check(QueueSession(801,8001,1120,2436,opening,false,request,{802,803},true),
          "queue opening starting buffer with verified OEM previews");
    Pump(controller);ResetEvents();
    Check(QueueSession(801,8001,1120,2436,opening,false,request,{803},true),
          "OEM registry drops a preview while its SF buffer can still be visible");
    Pump(controller);
    Check(Cleared().empty() && controller.active->companions.size()==2,
          "registry disappearance cannot strip a still-visible preview's shader");
    Check(border_widths[802]>0,"retained preview keeps CLIENT alongside SELF");
    ResetEvents();
    Check(QueueSession(804,8001,1120,2436,opening,false,request,{},false),
          "real App replaces starting content after preview registries empty");
    Pump(controller);
    Check(controller.active->layer==804 && controller.active->companions.size()==3,
          "handoff retains previous starting buffer and all verified previews");
    Check(Cleared().empty(),"handoff exposes no old preview rectangle");
    Check(layer_uniforms.count(804)==1 && border_widths[804]>0,
          "new App receives inherited contour immediately during adoption");
    ResetEvents();
    Stop(controller);
    auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
    Check(cleared==std::vector<int>({801,802,803,804}),
          "final retirement clears real App and every retained preview");

    auto& isolated=Fresh();
    Check(QueueSession(811,8101,1120,2436,opening,false,request,{812},false),
          "seed separate opening request");
    Pump(isolated);ResetEvents();
    Check(QueueSession(811,8101,1120,2436,opening,false,request+1,{},false),
          "new immutable request reuses the App surface");
    Pump(isolated);
    const auto retired_preview=layer_uniforms.at(812);
    Check(Cleared().empty() && !Owns(*isolated.active,812) && border_widths[812]>0,
          "new epoch freezes an unregistered old preview without inheriting its motion ownership");
    ResetEvents();Frame(isolated.active->last_ns+16000000LL,&isolated);
    Check(layer_uniforms.at(812)==retired_preview &&
          std::none_of(events.begin(),events.end(),[](const Event& event) {return event.layer==812;}),
          "new request frames cannot replay the frozen old preview");
    auto closing=opening;
    for(size_t i=0;i<4;++i) std::swap(closing[i],closing[i+4]);
    std::swap(closing[10],closing[11]);
    Check(QueueSession(811,8101,1120,2436,closing,true,request+2,{812},false),
          "rapid closing explicitly registers a reused preview on the same App");
    Pump(isolated);
    Check(Owns(*isolated.active,812)&&border_widths[812]>0&&Cleared().empty(),
          "direction reversal cannot clear a visible starting buffer into a rectangular backdrop");
    Stop(isolated);
    std::puts("PASS opening preview lifetime and immediate content handoff");
}
void CornerEndpointRadii() {
    for(bool closing:{false,true}) {
        auto& controller=Fresh();
        auto geometry=Geometry(1120,2436);
        geometry[10]=180;geometry[11]=45.6085f;
        if(!closing) {
            for(size_t i=0;i<4;++i) std::swap(geometry[i],geometry[i+4]);
            std::swap(geometry[10],geometry[11]);
        }
        const int layer=closing?701:702;
        Check(QueueSession(layer,7001,1120,2436,geometry,closing,NowNs(),{},false),
              "queue actual icon/device corner endpoints");
        auto& pending=*controller.pending;
        const double start=static_cast<double>(geometry[10])/geometry[2];
        const double end=static_cast<double>(geometry[11])/geometry[6];
        Check(std::abs(pending.corner.value-start)<1e-12,
              "corner seeds supplied physical start radius in visible-width units");
        Check(std::abs(pending.corner.target-end)<1e-12,
              "corner targets supplied physical end radius in visible-width units");
        Check(pending.corner.response==pending.parameters.response,
              "corner response follows opening or closing dynamics");
        Check(Submit(controller,pending,false),"submit exact start corner before time advance");
        Check(std::abs(layer_uniforms[layer][38]-start)<1e-6,
              "first shader corner matches actual start instead of fixed .225");
        Pump(controller);
        auto& current=*controller.active;
        current.animation.Advance(5);
        genie::model::AdvanceScalar(&current.corner,5,1.0);
        Check(Submit(controller,current,false),"submit converged endpoint corner");
        Check(std::abs(layer_uniforms[layer][38]*geometry[6]-geometry[11])<1e-4,
              "final shader corner reconstructs physical icon/device endpoint");
        Stop(controller);
    }
    std::puts("PASS physical icon/device corner endpoints in both directions");
}
void CornerRetargetAndHandoff() {
    auto& controller=Fresh();
    auto closing=Geometry(1120,2436);
    closing[10]=180;closing[11]=45.6085f;
    Check(QueueSession(711,7101,1120,2436,closing,true,NowNs(),{},true),
          "seed starting layer corner state");
    Pump(controller);
    auto& current=*controller.active;
    current.animation.Advance(.12);
    genie::model::AdvanceScalar(&current.corner,.12,1.0);
    current.last_ns=NowNs()+10000000LL;
    const auto corner=current.corner;
    auto opening=closing;
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    Check(QueueSession(711,7101,1120,2436,opening,false,NowNs(),{},true),
          "queue unprimed corner reversal");
    Pump(controller);
    Check(controller.active->corner.value==corner.value &&
          controller.active->corner.velocity==corner.velocity,
          "unprimed reversal preserves corner position and velocity");
    Check(std::abs(controller.active->corner.target-180.0/1120)<1e-12,
          "unprimed reversal targets the supplied device corner");
    Check(controller.active->corner.response==genie::model::kOpeningResponse,
          "unprimed reversal updates corner response");
    Check(QueueSession(712,7101,1120,2436,opening,false,NowNs(),{},false),
          "queue real-content handoff");
    Pump(controller);
    Check(controller.active->layer==712 && controller.active->corner.value==corner.value &&
          controller.active->corner.velocity==corner.velocity,
          "starting-to-content handoff preserves the live corner and velocity");
    Check(std::abs(controller.active->corner.target-180.0/1120)<1e-12,
          "handoff retains the nonzero device corner target");
    Stop(controller);
    for(const bool was_closing:{false,true}) {
        auto& handoff=Fresh();
        const auto old_request=NowNs()-1400000000LL;
        Check(QueueSession(713,7102,1120,2436,was_closing?closing:opening,
                           was_closing,old_request,{},true),
              "seed an old starting owner before a simultaneous handoff and reversal");
        Pump(handoff);
        const auto scalars=handoff.active->animation.Scalars();
        const auto old_corner=handoff.active->corner;
        const auto next_request=NowNs();
        handoff.active->last_ns=next_request+10000000LL;
        Check(QueueSession(714,7102,1120,2436,was_closing?opening:closing,
                           !was_closing,next_request,{},false),
              "reverse direction while the real App replaces its starting layer");
        Pump(handoff);
        std::printf("Handoff reversal deadline age: %.3f ms\n",
            static_cast<double>(next_request-handoff.active->started_ns)/1e6);
        Check(handoff.active->started_ns==next_request && handoff.active->elapsed==0,
              "a simultaneous handoff and reversal starts a fresh deadline");
        Check(handoff.active->last_ns==next_request+10000000LL &&
              handoff.active->corner.value==old_corner.value &&
              handoff.active->corner.velocity==old_corner.velocity,
              "handoff reversal preserves the presentation clock and corner velocity");
        for(size_t i=0;i<scalars.size();++i)
            Check(handoff.active->animation.Scalars()[i].value==scalars[i].value &&
                  handoff.active->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "handoff reversal preserves every live spring position and velocity");
        Frame(next_request+200000000LL,&handoff);
        Check(handoff.active && handoff.active->layer==714,
              "new reversal remains alive beyond the preceding starting owner's deadline");
        Stop(handoff);
    }
    std::puts("PASS corner reversal and starting/content handoff continuity");
}
void PrimedReversalContinuity() {
    auto& controller=Fresh();Check(Queue(601,6001),"seed closing icon state");Pump(controller);
    auto& current=*controller.active;
    current.animation.Advance(.18);
    genie::model::AdvanceScalar(&current.corner,.18,1.0);
    // Model a vsync timestamp slightly ahead of the submission clock, so a
    // retarget must preserve the already-published state without extra advance.
    current.last_ns=NowNs()+10000000LL;
    const auto mesh=genie::model::ContourMesh(current.animation,current.parameters);
    const double corner=current.corner.value;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create reversal transaction");
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    Check(QueueSession(601,6001,1120,2436,opening,false,NowNs(),{},false,
                       171,171,transaction),"prime reversal on same buffer");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    Check(controller.pending && controller.pending->primed_continuation,"prime inherits active continuation");
    for(size_t i=0;i<mesh.size();++i) {
        Check(std::abs(layer_uniforms[601][i*4]-mesh[i].x)<1e-6 &&
              std::abs(layer_uniforms[601][i*4+1]-mesh[i].y)<1e-6,
              "first reversal transaction keeps live contour instead of a fresh endpoint");
    }
    Check(std::abs(layer_uniforms[601][38]-corner)<1e-6,"first reversal keeps current corner");
    Check(controller.pending->corner.velocity==current.corner.velocity,
          "primed reversal preserves corner velocity");
    Check(std::abs(controller.pending->corner.target-100.0/1120)<1e-12,
          "primed reversal targets supplied device radius");
    const auto primed_corner=controller.pending->corner;
    const auto primed=controller.pending->animation.Scalars();
    Commit(0);Pump(controller);
    Check(!controller.active->closing,"committed reversal changes direction");
    Check(controller.active->corner.value==primed_corner.value &&
          controller.active->corner.velocity==primed_corner.velocity &&
          controller.active->corner.target==primed_corner.target,
          "commit adopts exact primed corner state and endpoint");
    for(size_t i=0;i<primed.size();++i) {
        Check(controller.active->animation.Scalars()[i].value==primed[i].value &&
              controller.active->animation.Scalars()[i].velocity==primed[i].velocity,
              "commit adopts exact primed value and velocity");
    }
    Stop(controller);std::puts("PASS primed reversal preserves the published contour");
}
void OverlappingRequests() {
    auto& controller=Fresh();
    const int64_t a_request=NowNs();
    Check(QueueSession(901,9001,1120,2436,Geometry(1120,2436),true,a_request,{},false),
          "queue closing App A");
    Pump(controller);
    auto* a=controller.active.get();
    a->animation.Advance(.12);genie::model::AdvanceScalar(&a->corner,.12,1.0);
    const int64_t b_request=a_request-1000;
    ResetEvents();
    Check(QueueSession(902,9002,1120,2436,Geometry(1120,2436),true,b_request,{},false),
          "independent older B event is accepted without a global watermark");
    Pump(controller);
    Check(controller.active->layer==902 && controller.overlapping.size()==1 &&
          controller.overlapping[0].get()==a,"B adoption retains independent closing A");
    Check(Cleared().empty() && border_widths[901]>0 && border_widths[902]>0,
          "new App never exposes the old closing host by clearing its shader");
    const auto a_frames=a->frames;
    const auto b_frames=controller.active->frames;
    const auto applies=renderer_applies.load();
    ResetEvents();
    Frame(NowNs(),&controller);
    std::printf("Overlapping frame transaction applies: %d\n",renderer_applies.load()-applies);
    Check(a->frames>a_frames && controller.active->frames>b_frames,
          "both independent sessions receive display-event updates");
    Check(renderer_applies.load()==applies+1 && events.size()==2 &&
          events[0].transaction==events[1].transaction,
          "independent owners publish one atomic transaction per display-event frame");
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,9001,a_request);
    a->cancel_ns=NowNs()-1;
    ResetEvents();Frame(NowNs(),&controller);
    Check(Cleared().empty() && a->cancel_ns && controller.active->cancel_ns==0,
          "scoped A stop leaves both closing silhouettes protected and cannot stop B");
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    a->last_ns=NowNs()+10000000LL;
    const auto corner=a->corner;
    Check(QueueSession(901,9001,1120,2436,opening,false,NowNs(),{},false),
          "reverse nonforeground A while B remains live");
    Pump(controller);
    Check(controller.active.get()==a && !a->closing && a->corner.value==corner.value &&
          a->corner.velocity==corner.velocity && !a->cancel_ns,
          "nonforeground reversal recovers exact motion state and clears stale stop");
    Check(controller.overlapping.size()==1 && controller.overlapping[0]->layer==902,
          "A reversal preserves independent B ownership");
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,9001,a->request_ns);
    a->cancel_ns=NowNs()-1;
    const auto cancel_applies=renderer_applies.load();
    ResetEvents();Frame(NowNs(),&controller);
    Check(renderer_applies.load()==cancel_applies+1 && events.size()==1 &&
          events[0].layer==902 && !events[0].clear,
          "paused opening contributes no writes while the independent closing owner advances");
    Check(controller.active.get()==a && controller.overlapping.size()==1 &&
          controller.overlapping[0]->layer==902,
          "opening cancellation retains its continuation and keeps the independent closing owner alive");
    Stop(controller);DrainCompletions();Check(!Running(),"global stop retires every overlap and detach");
    std::puts("PASS independent overlapping requests and scoped retirement");
}

void IndependentCommitGates() {
    auto& controller=Fresh();
    auto a=Prime(911,9101);
    auto b=Prime(912,9102);
    Check(controller.queued.size()==1,"unrelated pending A is queued rather than superseded");
    Commit(b);Pump(controller);
    Check(controller.active && controller.active->layer==912 && controller.queued.size()==1 &&
          controller.queued[0]->layer==911,"committed B starts while A show remains owned and uncommitted");
    const auto frames=controller.active->frames;
    Frame(NowNs(),&controller);
    Check(controller.active->frames>frames,"uncommitted unrelated A cannot stall B frames");
    ResetEvents();Commit(a);Pump(controller);
    Check(controller.active->layer==911 && controller.overlapping.size()==1 &&
          controller.overlapping[0]->layer==912 && Cleared().empty(),
          "late A commit starts A without clearing already-visible B");
    Stop(controller);
    std::puts("PASS independent out-of-order publication commits");
}

void CrossParentPreviewHandoff() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs();
    Check(QueueSession(921,9201,1120,2436,opening,false,request,{922},true),
          "seed early preview before actual Task anchor");
    Pump(controller);
    controller.active->animation.Advance(.14);
    genie::model::AdvanceScalar(&controller.active->corner,.14,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto scalars=controller.active->animation.Scalars();
    const auto corner=controller.active->corner;
    ResetEvents();
    Check(QueueSession(923,9202,1120,2436,opening,false,request,{},false),
          "same trusted immutable request resolves to actual Task parent");
    Pump(controller);
    Check(controller.active->layer==923 && controller.active->parent_layer==9202 &&
          controller.overlapping.empty(),"early preview and Task are one continuing request");
    for(size_t i=0;i<scalars.size();++i)
        Check(controller.active->animation.Scalars()[i].value==scalars[i].value &&
              controller.active->animation.Scalars()[i].velocity==scalars[i].velocity,
              "cross-parent handoff preserves each spring position and velocity");
    Check(controller.active->corner.value==corner.value && controller.active->corner.velocity==corner.velocity,
          "cross-parent handoff preserves corner motion");
    Check(controller.active->companions.size()==2 && Cleared().empty() &&
          border_widths[921]>0 && border_widths[922]>0 && border_widths[923]>0,
          "cross-parent handoff keeps old visible preview buffers under shared contour");
    Stop(controller);
    std::puts("PASS trusted early-preview to Task handoff");
}

void AtomicCompanionAttachment() {
    auto& controller=Fresh();Check(Queue(931,9301),"seed live request for atomic show attachment");
    Pump(controller);
    const auto request=controller.active->request_ns;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create companion show transaction");
    ResetEvents();
    Check(Java_ng_lst_genie_ContentAnimator_nativeAttach(nullptr,nullptr,
              reinterpret_cast<jlong>(transaction),932,9301,1120,2436,request,JNI_FALSE)==JNI_TRUE,
          "nativeAttach appends new compatible buffer to exact request");
    Check(events.size()==1 && events[0].layer==932 && events[0].transaction==transaction &&
          border_widths[932]>0,"new buffer SELF and border use supplied show transaction only");
    const auto first=layer_uniforms[932];
    const auto frames=controller.active->frames;
    Frame(NowNs(),&controller);
    Check(controller.active->frames==frames && layer_uniforms[932]==first,
          "frame writer cannot overtake uncommitted companion first state");
    Check(!AttachSession(transaction,933,9302,1120,2436,request) &&
          !AttachSession(transaction,933,9301,1080,2340,request) &&
          !AttachSession(transaction,933,9301,1120,2436,request+1),
          "attachment rejects wrong parent, extent, and immutable epoch");
    auto* another=ASurfaceTransaction_create();Check(another,"create second show transaction");
    Check(AttachSession(another,934,9301,1120,2436,request),"second surface has independent show commit");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    ASurfaceTransaction_apply(another);ASurfaceTransaction_delete(another);
    Commit(1);Frame(NowNs(),&controller);
    Check(controller.active->frames==frames,"later show commit cannot release earlier outstanding publication");
    Commit(0);Frame(NowNs(),&controller);
    Check(controller.active->frames>frames && border_widths[932]>0 && border_widths[934]>0,
          "all shown buffers continue together after every publication commits");
    Stop(controller);
    std::puts("PASS atomic companion show attachment and independent commit barriers");
}

void StopDuringCompanionPublication() {
    auto& controller=Fresh();Check(Queue(941,9401),"seed session before publication cancellation");Pump(controller);
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create canceled companion transaction");
    Check(AttachSession(transaction,942,9401,1120,2436,controller.active->request_ns),
          "prime companion before global disable");
    ResetEvents();Stop(controller);
    Check(!controller.active && controller.retiring.size()==1 && Cleared().empty() && Running(),
          "global disable retains uncommitted show until it can be safely cleared");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    Commit(0);Pump(controller);
    auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
    Check(cleared==std::vector<int>({941,942}) && controller.retiring.empty(),
          "eventual show commit retires both primary and companion after disable");
    DrainCompletions();Check(!Running(),"disabled publication idle after cleanup completion");
    std::puts("PASS global disable during atomic companion publication");
}
void RepeatedPrimeCommitGates() {
    auto& controller=Fresh();
    auto a=Prime(951,9501);
    auto b=Prime(951,9501);
    ResetEvents();Commit(b);Pump(controller);
    Check(!controller.active && controller.pending && !Committed(*controller.pending),
          "later same-buffer show cannot advance before its earlier publication commits");
    Check(Cleared().empty(),"reordered same-buffer publication does not retire a visible prime");
    Commit(a);Pump(controller);
    Check(controller.active && controller.active->layer==951 && controller.retiring.empty(),
          "same-buffer session adopts after both publication commits");
    Stop(controller);
    std::puts("PASS repeated same-buffer priming commit dependencies");
}

void PrimedCrossParentHandoff() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs();
    Check(QueueSession(961,9601,1120,2436,opening,false,request,{},true),
          "seed early preview for atomic cross-parent handoff");
    Pump(controller);
    controller.active->animation.Advance(.1);
    genie::model::AdvanceScalar(&controller.active->corner,.1,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto mesh=genie::model::ContourMesh(controller.active->animation,controller.active->parameters);
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create actual Task show transaction");
    Check(QueueSession(962,9602,1120,2436,opening,false,request,{},false,0,0,transaction),
          "prime actual Task in its own show transaction using trusted original request");
    Check(controller.pending->primed_continuation,"cross-parent prime inherits preview motion");
    for(size_t i=0;i<mesh.size();++i)
        Check(std::abs(layer_uniforms[962][i*4]-mesh[i].x)<1e-6 &&
              std::abs(layer_uniforms[962][i*4+1]-mesh[i].y)<1e-6,
              "first actual Task shader preserves preview mesh before commit");
    const auto primed=controller.pending->animation.Scalars();
    const auto frames=controller.active->frames;
    Frame(NowNs(),&controller);
    Check(controller.active->frames==frames,"old preview updates wait for atomic Task show commit");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);Commit(0);Pump(controller);
    Check(controller.active->layer==962 && controller.active->companions.size()==1,
          "committed Task carries old preview protection across parent change");
    for(size_t i=0;i<primed.size();++i)
        Check(controller.active->animation.Scalars()[i].value==primed[i].value &&
              controller.active->animation.Scalars()[i].velocity==primed[i].velocity,
              "cross-parent adoption uses exactly the state already published in show");
    Stop(controller);
    std::puts("PASS atomic cross-parent preview handoff continuity");
}
void DelayedPendingPreviewCoalescence() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs()-700000000LL;
    auto* manager_show=ASurfaceTransaction_create();Check(manager_show,"create delayed manager preview show");
    Check(QueueSession(982,979,1120,2436,opening,false,request,{},true,0,0,manager_show),
          "manager preview primes before delayed worker resolves independent preview");
    Check(QueueSession(973,975,1120,2436,opening,false,request,{},true),
          "worker coalesces a same-request preview after original event is over500ms old");
    Check(controller.pending && Owns(*controller.pending,982),
          "coalesced worker request owns the earlier manager publication");
    ASurfaceTransaction_apply(manager_show);ASurfaceTransaction_delete(manager_show);
    ResetEvents();Commit(0);Pump(controller);
    Check(controller.active && controller.active->layer==973 && Owns(*controller.active,982),
          "delayed coalesced worker adopts instead of silently dropping published preview ownership");
    Check(Cleared().empty() && border_widths[982]>0,
          "retiring superseded manager prime cannot clear its still-visible buffer");
    Check(QueueSession(986,979,1120,2436,opening,false,request,{982},true),
          "standard starting buffer subsequently arrives under actual Task parent");
    auto* starting_show=ASurfaceTransaction_create();Check(starting_show,"create delayed starting buffer show");
    Check(QueueSession(986,979,1120,2436,opening,false,request,{},true,0,0,starting_show),
          "standard starting show primes the continuing immutable request");
    ASurfaceTransaction_apply(starting_show);ASurfaceTransaction_delete(starting_show);
    Commit(1);Pump(controller);
    Check(controller.active->layer==986 && Owns(*controller.active,973) && Owns(*controller.active,982),
          "real starting primary inherits manager and independent preview ownership");
    Check(Cleared().empty() && border_widths[982]>0 && border_widths[973]>0,
          "pending coalescence and prime adoption expose no earlier preview rectangle");
    Stop(controller);
    std::puts("PASS delayed same-request pending preview coalescence");
}
void PublicationUnionAfterLateAttachment() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs();
    Check(QueueSession(991,9901,1120,2436,opening,false,request,{},false),
          "seed real primary before same-request parent changes");
    Pump(controller);
    auto* next_show=ASurfaceTransaction_create();Check(next_show,"create alternate parent show");
    Check(QueueSession(992,9902,1120,2436,opening,false,request,{},false,0,0,next_show),
          "prime same-request nonstarting primary under another verified parent");
    auto* late_show=ASurfaceTransaction_create();Check(late_show,"create late old-parent preview show");
    Check(AttachSession(late_show,993,9901,1120,2436,request),
          "old-parent event adds a preview after pending successor was cloned");
    Check(QueueSession(994,9902,1120,2436,opening,false,request,{},false),
          "worker coalesces successor after publications diverge");
    Check(Owns(*controller.pending,991) && Owns(*controller.pending,992) && Owns(*controller.pending,993),
          "successor owns union of live and pending publications rather than one selected source");
    Check(controller.pending->commit_dependencies.size()==2,
          "successor carries both distinct outstanding show barriers without duplicates");
    ASurfaceTransaction_apply(next_show);ASurfaceTransaction_delete(next_show);
    ASurfaceTransaction_apply(late_show);ASurfaceTransaction_delete(late_show);
    const auto frames=controller.active->frames;
    ResetEvents();Commit(0);Pump(controller);Frame(NowNs(),&controller);
    Check(controller.active->layer==991 && controller.active->frames==frames && Cleared().empty(),
          "successor and current frame writer wait for late old-parent show commit");
    Commit(1);Pump(controller);
    Check(controller.active->layer==994 && controller.overlapping.empty() &&
          Owns(*controller.active,993) && Cleared().empty(),
          "same immutable request merges every publication across nonstarting parent changes");
    Stop(controller);
    std::puts("PASS same-request publication union and late attachment barriers");
}
void OneShotOpeningPublication() {
    auto& controller=Fresh();
    genie::Transition request;
    request.geometry=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(request.geometry[i],request.geometry[i+4]);
    std::swap(request.geometry[10],request.geometry[11]);
    request.event_ns=NowNs()-30000000LL;
    request.leash_layer=9950;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create first preview transaction");
    const auto applications=renderer_applies.load();
    Check(genie::PrimeOpeningPreview(transaction,reinterpret_cast<void*>(995),
        reinterpret_cast<void*>(9950),1120,2436,request),"verified preview receives one-shot opening shader");
    Check(events.size()==1 && events[0].layer==995 && !events[0].clear && events[0].transaction==transaction,
          "preview shader is already in the actual first-show transaction before its caller applies");
    Check(renderer_applies.load()==applications,"one-shot priming never applies a separate transaction");
    Check(!controller.active && !controller.pending && controller.queued.empty() &&
          controller.overlapping.empty() && commits.empty() && completions.empty(),
          "one-shot publication creates no launcher frame writer or retirement callbacks");
    Check(controller.detaches_pending.load()==0 && !Running(),
          "server-owned one-shot leaves no launcher detach outstanding and remains idle");
    Check(layer_uniforms[995][8]-layer_uniforms[995][0]<.99f && layer_uniforms[995][36]==1120,
          "first publication uses elapsed opening deformation and full buffer domain");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    request.closing=true;
    transaction=ASurfaceTransaction_create();
    Check(!genie::PrimeOpeningPreview(transaction,reinterpret_cast<void*>(995),
        reinterpret_cast<void*>(9950),1120,2436,request),"closing request cannot use opening capability priming");
    // The central renderer owns real preview retirement, represented here by
    // its same-transaction SELF/guard clear after the one-shot was delivered.
    ClearSurface(transaction,reinterpret_cast<void*>(995));
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    std::puts("PASS atomic one-shot preview publication without a second frame writer");
}
void ClosingGroupHostPublication() {
    auto& controller=Fresh();
    genie::Transition request;
    request.geometry=Geometry(1120,2436);
    request.closing=true;request.event_ns=NowNs()-16000000LL;
    auto* transaction=ASurfaceTransaction_create();
    const auto applies=renderer_applies.load();
    Check(genie::PrimeContentPublication(transaction,reinterpret_cast<void*>(1799),
        reinterpret_cast<void*>(1790),1120,2436,request),"closing live group primes its host transaction");
    Check(events.size()==1 && !events[0].clear && events[0].layer==1799 &&
          events[0].transaction==transaction,"closing output protection joins the caller's first host transaction");
    Check(!controller.active && !controller.pending && commits.empty() && renderer_applies.load()==applies,
          "closing one-shot creates neither a second writer nor an independent apply");
    ClearSurface(transaction,reinterpret_cast<void*>(1799));
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    std::puts("PASS closing group first host publication");
}
void GestureClosingSessionPublication() {
    auto& controller=Fresh();
    const std::array<float,12> release={456.9f,289.9f,663,1442.1f,80,2117,171,171,
                                        458,-335,106.56f,45.61f};
    const genie::model::Rect icon{80,2117,171,171},window{0,0,1120,2436};
    genie::model::Parameters expected;
    Check(genie::model::ParametersForIcon(icon,window,&expected),"gesture reference uses the authoritative content domain");
    expected.response=genie::model::kClosingResponse;
    // The bridge accepts small future clock skew. Keep the initial publication
    // exactly at release time so elapsed scheduling cannot hide a wrong seed.
    const auto request=NowNs()+50000000LL;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create gesture group reveal transaction");
    const auto applies=renderer_applies.load();
    Check(QueueSession(1811,1810,1120,2436,release,true,request,{},false,
                       0,0,transaction,reinterpret_cast<void*>(1812)),
          "gesture close primes a full buffer from a scaled translated release");
    Check(controller.pending && controller.pending->closing && controller.pending->width==1120 &&
          controller.pending->height==2436,"gesture session retains full buffer dimensions rather than release dimensions");
    auto& pending=*controller.pending;
    Check(pending.parameters.width==expected.width && pending.parameters.height==expected.height &&
          pending.parameters.icon_x==expected.icon_x && pending.parameters.icon_y==expected.icon_y &&
          pending.parameters.window_x==expected.window_x && pending.parameters.window_y==expected.window_y &&
          pending.parameters.minimized_scale==expected.minimized_scale &&
          pending.parameters.genie_amount_y==expected.genie_amount_y,
          "gesture translation and scale do not become the content geometry basis");
    const double start_corner=static_cast<double>(release[10])/release[2];
    const double end_corner=static_cast<double>(release[11])/release[6];
    Check(std::abs(pending.corner.value-start_corner)<1e-12 &&
          std::abs(pending.corner.target-end_corner)<1e-12,
          "gesture corners use released visible width and destination icon width");
    Check(pending.corner.response==genie::model::kClosingResponse,
          "gesture close preserves ordinary closing corner dynamics");
    Check(events.size()==2 && events[0].clear && events[0].layer==1812 &&
          !events[1].clear && events[1].layer==1811 &&
          events[0].transaction==transaction && events[1].transaction==transaction,
          "gesture source restoration and first output shader share the reveal transaction");
    Check(layer_uniforms[1811][36]==1120 && layer_uniforms[1811][37]==2436 &&
          layer_uniforms[1811][40]==1120 && layer_uniforms[1811][41]==2436,
          "gesture first shader uses the full buffer for destination and source extents");
    Check(std::abs(layer_uniforms[1811][38]-start_corner)<1e-7,
          "gesture first shader preserves the release radius instead of dividing it by full width");
    Check(commits.size()==1 && renderer_applies.load()==applies && border_widths[1811]>0 && border_widths[1812]==0,
          "gesture first publication is commit gated with atomic CLIENT protection and no separate apply");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    ResetEvents();Frame(request,&controller);
    Check(events.empty() && controller.pending && !controller.active,
          "gesture renderer cannot advance before its reveal transaction commits");
    Commit(0);Pump(controller);
    Check(controller.active && controller.active->layer==1811 && !controller.pending,
          "gesture closing session adopts after its reveal commit");
    ResetEvents();Frame(request+120000000LL,&controller);
    genie::model::Animation reference;
    Check(reference.Reset(expected,false) && reference.Retarget(true),"initialize independently based gesture closing reference");
    reference.Advance(.12);
    const auto mesh=genie::model::ContourMesh(reference,expected);
    for(size_t i=0;i<mesh.size();++i) {
        Check(std::abs(layer_uniforms[1811][i*4]-mesh[i].x)<1e-6 &&
              std::abs(layer_uniforms[1811][i*4+1]-mesh[i].y)<1e-6,
              "gesture mesh advances in the authoritative origin and full content domain");
    }
    Check(controller.active->frames==1 && border_widths[1811]>0,
          "committed gesture close advances while retaining output protection");
    ResetEvents();Stop(controller);
    Check(Cleared().empty() && border_widths[1811]>0,
          "gesture worker retirement preserves the visible composed silhouette");
    auto* restore=ASurfaceTransaction_create();
    Check(Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(nullptr,nullptr,
              reinterpret_cast<jlong>(restore),1811),"gesture source restoration owns the final effect cleanup");
    Check(Cleared()==std::vector<int>{1811} && border_widths[1811]==0,
          "gesture restoration clears its shader and CLIENT guard together");
    ASurfaceTransaction_delete(restore);
    DrainCompletions();Check(!Running(),"gesture renderer becomes idle after cleanup completion");
    std::puts("PASS gesture close full-domain session publication and cleanup");
}
void GestureClosingHostPublication() {
    auto& controller=Fresh();
    genie::Transition request;
    request.geometry={456.9f,289.9f,663,1442.1f,80,2117,171,171,458,-335,106.56f,45.61f};
    request.closing=true;request.event_ns=NowNs()+50000000LL;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create gesture first host transaction");
    const auto applies=renderer_applies.load();
    Check(genie::PrimeContentPublication(transaction,reinterpret_cast<void*>(1821),
        reinterpret_cast<void*>(1820),1120,2436,request),
        "gesture host publication accepts actual full dimensions with a scaled translated release");
    Check(events.size()==1 && !events[0].clear && events[0].layer==1821 && events[0].transaction==transaction,
          "gesture output shader is in the first host transaction before the caller applies it");
    Check(layer_uniforms[1821][36]==1120 && layer_uniforms[1821][37]==2436 &&
          layer_uniforms[1821][40]==1120 && layer_uniforms[1821][41]==2436,
          "gesture host publication samples the authoritative full content buffer");
    Check(std::abs(layer_uniforms[1821][38]-static_cast<double>(request.geometry[10])/request.geometry[2])<1e-7,
          "gesture host first corner ratio uses the scaled release width");
    Check(!controller.active && !controller.pending && controller.queued.empty() &&
          controller.overlapping.empty() && commits.empty() && completions.empty() &&
          renderer_applies.load()==applies && controller.detaches_pending.load()==0 && !Running(),
          "gesture host one-shot creates no extra renderer session, callbacks, or independent apply");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    // An elapsed frame distinguishes the content's (0,0) origin from the
    // translated release origin; the zero-time identity mesh alone cannot.
    ResetEvents();transaction=ASurfaceTransaction_create();Check(transaction,"create elapsed gesture host transaction");
    request.event_ns=NowNs()-120000000LL;
    const auto before=NowNs();
    Check(genie::PrimeContentPublication(transaction,reinterpret_cast<void*>(1821),
        reinterpret_cast<void*>(1820),1120,2436,request),"elapsed gesture host publication retains the full content domain");
    const auto after=NowNs();
    genie::model::Parameters expected;
    Check(genie::model::ParametersForIcon({80,2117,171,171},{0,0,1120,2436},&expected),
          "elapsed gesture reference uses full-screen origin instead of release translation");
    expected.response=genie::model::kClosingResponse;
    genie::model::Animation first,last;
    Check(first.Reset(expected,false) && first.Retarget(true) && last.Reset(expected,false) && last.Retarget(true),
          "initialize elapsed gesture reference at both observed publication times");
    first.Advance(static_cast<double>(before-request.event_ns)/1e9);
    last.Advance(static_cast<double>(after-request.event_ns)/1e9);
    const auto first_mesh=genie::model::ContourMesh(first,expected);
    const auto last_mesh=genie::model::ContourMesh(last,expected);
    for(size_t i=0;i<first_mesh.size();++i) {
        const double x=layer_uniforms[1821][i*4],y=layer_uniforms[1821][i*4+1];
        Check(x>=std::min(first_mesh[i].x,last_mesh[i].x)-1e-6 &&
              x<=std::max(first_mesh[i].x,last_mesh[i].x)+1e-6 &&
              y>=std::min(first_mesh[i].y,last_mesh[i].y)-1e-6 &&
              y<=std::max(first_mesh[i].y,last_mesh[i].y)+1e-6,
              "elapsed first-host mesh uses authoritative geometry within the observed clock interval");
    }
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    ResetEvents();transaction=ASurfaceTransaction_create();Check(transaction,"create gesture host cleanup transaction");
    ClearSurface(transaction,reinterpret_cast<void*>(1821));
    Check(events.size()==1 && events[0].clear && events[0].layer==1821 &&
          events[0].transaction==transaction && border_widths[1821]==0,
          "central-owner cleanup removes gesture host SELF and CLIENT guard atomically");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    std::puts("PASS gesture close full-domain first host publication");
}
void GesturePublicationRejectsIncompatibleGeometry() {
    auto& controller=Fresh();
    genie::Transition release;
    release.geometry={456.9f,289.9f,663,1442.1f,80,2117,171,171,458,-335,106.56f,45.61f};
    release.closing=true;
    auto distorted=release;distorted.geometry[3]=1500;
    auto oversized=release;oversized.geometry[2]=1232;oversized.geometry[3]=2679.6f;
    auto opening=release;opening.closing=false;
    for(size_t i=0;i<4;++i) std::swap(opening.geometry[i],opening.geometry[i+4]);
    std::swap(opening.geometry[10],opening.geometry[11]);
    const std::array<std::pair<genie::Transition,const char*>,3> cases{{
        {distorted,"distorted gesture close cannot publish a mismatched buffer aspect"},
        {oversized,"uniformly scaled close larger than the authoritative domain is rejected"},
        {opening,"partial opening cannot borrow the scaled-close publication exception"},
    }};
    for(const auto& item:cases) {
        auto request=item.first;request.event_ns=NowNs();
        auto* transaction=ASurfaceTransaction_create();Check(transaction,"create rejected gesture publication transaction");
        const auto applies=renderer_applies.load();
        const auto retained=references;
        Check(!genie::PrimeContentPublication(transaction,reinterpret_cast<void*>(1831),
            reinterpret_cast<void*>(1830),1120,2436,request),item.second);
        Check(events.empty() && border_widths.empty() && commits.empty() && completions.empty() &&
              !controller.active && !controller.pending && !Running() && controller.detaches_pending.load()==0,
              "rejected gesture geometry publishes no shader, CLIENT guard, session, or callbacks");
        Check(renderer_applies.load()==applies && references==retained,
              "rejected gesture geometry applies no transaction and retains no capabilities");
        ASurfaceTransaction_delete(transaction);
    }
    std::puts("PASS gesture publication rejects distorted, oversized, and partial-opening domains");
}
void GroupedSourcePublication() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs();
    Check(QueueSession(1801,1800,1120,2436,opening,false,request,{1802},false),
          "seed direct content with a starting companion");
    Pump(controller);ResetEvents();
    auto* transaction=ASurfaceTransaction_create();
    Check(QueueSession(1803,1800,1120,2436,opening,false,request,{1801,1802},false,
                       0,0,transaction,reinterpret_cast<void*>(1801)),
          "prime live grouped output in its reveal transaction");
    Check(controller.pending && !Owns(*controller.pending,1801) && Owns(*controller.pending,1802),
          "grouping excludes its source from explicit and inherited companions while retaining previews");
    Check(border_widths[1801]==0 && border_widths[1803]>0,
          "group source is cleared in the same publication as output SELF");
    Check(events.size()>=2 && events.front().clear && events.front().layer==1801 &&
          events.front().transaction==transaction && events[1].transaction==transaction,
          "source restoration and grouped first effect use the caller's one transaction");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    ResetEvents();Frame(NowNs(),&controller);
    Check(events.empty(),"old source writer pauses while grouped first publication is uncommitted");
    Commit(0);Pump(controller);ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active && controller.active->layer==1803 && !Owns(*controller.active,1801),
          "grouped output adopts the same request without retaining its source");
    for(const auto& event:events) Check(event.layer!=1801 || event.clear,
          "no subsequent frame rewrites SELF on the live source");
    transaction=ASurfaceTransaction_create();
    Check(!AttachSession(transaction,1801,1800,1120,2436,request),
          "a late source show cannot reintroduce double deformation");
    ASurfaceTransaction_delete(transaction);
    Check(QueueSession(1803,1800,1120,2436,opening,false,request,{1802},false),
          "same-group discovery update retains composed-output ownership");
    Pump(controller);
    Check(controller.active && controller.active->unwarped_layer==1801,
          "unprimed worker updates preserve the group's source identity");
    controller.active->animation.Advance(1.0);
    genie::model::AdvanceScalar(&controller.active->corner,1.0,1.0);
    ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active && controller.active->layer==1803 && border_widths[1803]>0,
          "settled opening retains SELF while the composed output remains visible");
    auto* restore=ASurfaceTransaction_create();
    Check(!Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(nullptr,nullptr,
              reinterpret_cast<jlong>(restore),1803),"restore cannot clear a group still owned by an animation");
    Stop(controller);ResetEvents();
    Check(border_widths[1803]>0,"worker retirement leaves the last masked group state until source restoration");
    Check(Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(nullptr,nullptr,
              reinterpret_cast<jlong>(restore),1803),"retired output clears in the caller's visibility restoration transaction");
    Check(Cleared()==std::vector<int>{1803}&&events[0].transaction==restore&&border_widths[1803]==0,
          "group restoration atomically clears SELF and CLIENT border on the hidden output");
    ASurfaceTransaction_delete(restore);
    std::puts("PASS grouped-source publication and masked retirement");
}

void RevokedGroupKeepsMask() {
    auto& controller=Fresh();const auto request=NowNs();
    auto* publication=ASurfaceTransaction_create();Check(publication,"create composed close publication");
    Check(QueueSession(1851,1850,1120,2436,Geometry(1120,2436),true,request,{1853},false,
                       0,0,publication,reinterpret_cast<void*>(1852)),"prime a closing composed output");
    ASurfaceTransaction_apply(publication);ASurfaceTransaction_delete(publication);Commit(0);Pump(controller);
    ResetEvents();
    Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,1850,request);
    Check(!controller.active && controller.retiring.empty(),"revocation immediately ends the composed closing writer");
    Check(Cleared().empty() && border_widths[1851]>0 && border_widths[1853]>0,
          "composed output and still-visible OEM color retain their last masks after revoke");
    ResetEvents();Frame(NowNs(),&controller);
    Check(events.empty(),"revoked output has no further closing deformation writes");
    auto* restore=ASurfaceTransaction_create();Check(restore,"create hidden output restoration transaction");
    Check(Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(nullptr,nullptr,
        reinterpret_cast<jlong>(restore),1851),"bridge may clear the retired group's mask with its hide/source restoration");
    Check(Cleared()==std::vector<int>{1851} && events[0].transaction==restore,
          "revoked composed mask clears only in the supplied visibility transaction");
    ASurfaceTransaction_delete(restore);
    std::puts("PASS revoked composed close preserves mask until visibility restoration");
}

void GroupReplacementDoesNotBecomePreview() {
    auto& controller=Fresh();
    auto opening=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(opening[i],opening[i+4]);
    std::swap(opening[10],opening[11]);
    const auto request=NowNs();
    auto* first=ASurfaceTransaction_create();
    Check(QueueSession(1841,1840,1120,2436,opening,false,request,{1843},false,
                       0,0,first,reinterpret_cast<void*>(1842)),"seed composed output and OEM preview");
    ASurfaceTransaction_apply(first);ASurfaceTransaction_delete(first);Commit(0);Pump(controller);
    const auto next_request=NowNs();
    Check(QueueSession(1841,1840,1120,2436,opening,false,next_request,{1843},false),
          "new epoch explicitly reuses the same composed output and visible preview");Pump(controller);
    Check(RequestRunning(request) && RequestRunning(next_request),
          "same-output epoch inheritance retains the original cleanup identity");
    ResetEvents();
    auto* replacement=ASurfaceTransaction_create();
    Check(QueueSession(1844,1840,1120,2436,opening,false,next_request,{},false,
                       0,0,replacement,reinterpret_cast<void*>(1845)),"replace a recreated source in the same Task/request");
    Check(controller.pending && !Owns(*controller.pending,1841) && Owns(*controller.pending,1843),
          "replacement retains the OEM preview but never inherits the previous composed output");
    ASurfaceTransaction_apply(replacement);ASurfaceTransaction_delete(replacement);Commit(1);Pump(controller);
    Check(!RequestRunning(request) && RequestRunning(next_request),
          "replacement output does not inherit cleanup aliases for a primary it no longer owns");
    ResetEvents();Frame(NowNs(),&controller);
    for(const auto& event:events) Check(event.layer!=1841,"new group cannot write or clear the previous group as a preview");
    Check(border_widths[1841]>0,"old group keeps its mask until bridge visibility restoration");
    Stop(controller);
    auto* restore=ASurfaceTransaction_create();
    for(int layer:{1841,1844}) Check(Java_ng_lst_genie_ContentAnimator_nativeClearRetiredGroup(
        nullptr,nullptr,reinterpret_cast<jlong>(restore),layer),"hidden retired group releases its effect");
    ASurfaceTransaction_delete(restore);
    std::puts("PASS composed output replacement preserves visibility ownership");
}

std::array<float,12> LinkedGeometry(bool closing) {
    auto geometry=Geometry(1120,2436);
    if(!closing) {
        for(size_t i=0;i<4;++i) std::swap(geometry[i],geometry[i+4]);
        std::swap(geometry[10],geometry[11]);
    }
    return geometry;
}
size_t PrimeTestIcon(int layer,int parent,const std::array<float,12>& geometry,
                     bool closing,int64_t request) {
    size_t index;
    {std::lock_guard<std::mutex> lock(test_mutex);index=commits.size();}
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create linked icon host transaction");
    const auto applies=renderer_applies.load();
    Check(PrimeLinkedIcon(transaction,layer,parent,171,171,1120,2436,geometry,closing,request),
          "prime real floating icon on its shared server motion owner");
    Check(renderer_applies.load()==applies,"linked icon first publication does not apply a separate transaction");
    Check(commits.size()==index+1,"linked icon host publication has an independent commit barrier");
    Check(std::any_of(events.begin(),events.end(),[&](const Event& event) {
              return event.layer==layer && !event.clear && event.transaction==transaction;
          }),"linked icon first shader uses the supplied host transaction alongside its owner");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    return index;
}
void CheckSharedIconFrame(int app,int icon) {
    Check(events.size()==2 && !events[0].clear && !events[1].clear &&
          events[0].layer==app && events[1].layer==icon,
          "one frame writes the App and its linked icon exactly once");
    Check(events[0].transaction==events[1].transaction,
          "App and linked icon frame uniforms are submitted in the same transaction");
    const auto& app_values=layer_uniforms.at(app);
    const auto& icon_values=layer_uniforms.at(icon);
    Check(std::equal(app_values.begin(),app_values.begin()+36,icon_values.begin()) &&
          app_values[38]==icon_values[38] && app_values[39]==icon_values[39],
          "App and icon use exactly the same mesh vertices and corner state");
    Check(app_values[36]==1120 && app_values[37]==2436 &&
          app_values[40]==1120 && app_values[41]==2436,
          "App samples the full content buffer in the full-size viewport");
    Check(icon_values[36]==171 && std::abs(icon_values[37]-171.0f*2436/1120)<1e-4f &&
          icon_values[40]==171 && icon_values[41]==171,
          "linked icon keeps its tall virtual viewport separate from its square source footprint");
}
void CheckLinkedRetirement(int app,int icon,bool preserve_icon=false) {
    auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
    Check(cleared==(preserve_icon?std::vector<int>{app}:std::vector<int>{app,icon}),
          "retirement clears the App and only icons whose visibility has ended");
    const auto app_clear=std::find_if(events.begin(),events.end(),
        [&](const Event& event) {return event.layer==app && event.clear;});
    const auto icon_clear=std::find_if(events.begin(),events.end(),
        [&](const Event& event) {return event.layer==icon && event.clear;});
    if(!preserve_icon) Check(app_clear->transaction==icon_clear->transaction,
          "App and linked icon SELF and CLIENT guards retire in the same transaction");
    Check(border_widths[app]==0 && (preserve_icon?border_widths[icon]>0:border_widths[icon]==0),
          "retirement retains only a still-visible icon's SELF and CLIENT guard");
    DrainCompletions();Check(!Running(),"linked stream becomes idle after its actual detach completion");
    for(const auto& entry:references)
        Check(entry.second==0,"retired linked stream releases every retained surface capability");
}
void LinkedIconVisibleRetirement() {
    enum class Retirement {Revoke,Deadline,Handoff,Disable};
    for(bool primary:{true,false}) for(const auto retirement:{Retirement::Revoke,Retirement::Deadline,
                                                            Retirement::Handoff,Retirement::Disable}) {
        auto& controller=Fresh();const auto request=NowNs();
        auto* publication=ASurfaceTransaction_create();Check(publication,"create visible icon publication");
        Check(QueueSession(2101,2100,1120,2436,LinkedGeometry(true),true,request,{},false,
                           primary?171:0,primary?171:0,publication),
              "seed icon-first or App-first visible stream");
        ASurfaceTransaction_apply(publication);ASurfaceTransaction_delete(publication);Commit(0);Pump(controller);
        if(!primary) Commit(PrimeTestIcon(2102,2103,LinkedGeometry(true),true,request));
        const int icon=primary?2101:2102;
        const auto last_mask=layer_uniforms.at(icon);
        ResetEvents();
        if(retirement==Retirement::Revoke)
            Java_ng_lst_genie_ContentAnimator_nativeRevokeRequest(nullptr,nullptr,2100,request);
        else if(retirement==Retirement::Deadline) {
            controller.active->started_ns=NowNs()-2000000000LL;
            Frame(NowNs(),&controller);
        } else if(retirement==Retirement::Handoff) {
            Session successor;
            successor.layer=2104;
            DetachLostSurfaces(controller,*controller.active,successor,request);
        } else Stop(controller);
        const bool clear=retirement==Retirement::Disable;
        const auto cleared=Cleared();
        Check((std::find(cleared.begin(),cleared.end(),icon)!=cleared.end())==clear,
              "visible primary and companion icons keep SELF through revoke, deadline, and lost-surface handoff");
        Check(clear?border_widths[icon]==0:border_widths[icon]>0,
              "only explicit disable removes the visible icon's CLIENT guard");
        if(retirement==Retirement::Revoke || retirement==Retirement::Handoff)
            Check(layer_uniforms.at(icon)==last_mask,"retirement freezes the exact last icon mask");
        if(retirement==Retirement::Handoff) Stop(controller);
        DrainCompletions();Check(!Running(),"retained icon masks create no independent renderer or cleanup wait");
    }
    std::puts("PASS visible icon masks survive motion retirement until platform removal or explicit disable");
}
void LateLinkedIconSharesFrames() {
    auto& controller=Fresh();
    const auto geometry=LinkedGeometry(true);
    const int64_t request=NowNs()-700000000LL;
    Check(QueueSession(1901,1900,1120,2436,geometry,true,request,{},false),
          "seed App animation before a floating icon stalls for more than 500ms");
    Pump(controller);ResetEvents();
    const auto frames=controller.active->frames;
    const auto last_ns=controller.active->last_ns;
    const auto scalars=controller.active->animation.Scalars();
    const auto commit=PrimeTestIcon(1902,1903,geometry,true,request);
    Check(controller.active->layer==1901 && Owns(*controller.active,1902) &&
          !controller.pending && controller.overlapping.empty(),
          "late icon joins the App owner without creating another animation clock");
    const auto icon_first=layer_uniforms[1902];
    ResetEvents();Frame(last_ns+48000000LL,&controller);
    Check(events.empty() && controller.active->frames==frames && controller.active->last_ns==last_ns &&
          layer_uniforms[1902]==icon_first,
          "an uncommitted icon publication freezes both App and icon frame updates");
    for(size_t i=0;i<scalars.size();++i)
        Check(controller.active->animation.Scalars()[i].value==scalars[i].value &&
              controller.active->animation.Scalars()[i].velocity==scalars[i].velocity,
              "the shared motion does not advance behind the icon publication barrier");
    Commit(commit);ResetEvents();
    const auto applies=renderer_applies.load();
    Frame(last_ns+64000000LL,&controller);
    Check(controller.active->frames==frames+1 && controller.active->last_ns==last_ns+64000000LL &&
          renderer_applies.load()==applies+1,
          "one shared frame catches up after commit with a single renderer apply");
    CheckSharedIconFrame(1901,1902);
    ResetEvents();Stop(controller);CheckLinkedRetirement(1901,1902);
    std::puts("PASS late linked icon shares App frames and publication barrier");
}
void LinkedIconBeforeAppHandoff() {
    for(bool closing:{false,true}) {
        auto& controller=Fresh();
        const auto geometry=LinkedGeometry(closing);
        const int app=closing?1921:1911,icon=app+1;
        const int64_t request=NowNs()-30000000LL;
        const auto commit=PrimeTestIcon(icon,app+2,geometry,closing,request);
        Pump(controller);
        Check(!controller.active && controller.pending,
              "icon-first provisional owner waits for its first host transaction commit");
        Commit(commit);Pump(controller);
        Check(controller.active && controller.active->layer==icon && controller.active->starting &&
              controller.active->texture_width==171 && controller.active->texture_height==171,
              "icon-first publication seeds one provisional owner with its actual texture dimensions");
        controller.active->animation.Advance(.11);
        genie::model::AdvanceScalar(&controller.active->corner,.11,1.0);
        controller.active->last_ns=NowNs()+10000000LL;
        const auto scalars=controller.active->animation.Scalars();
        const auto corner=controller.active->corner;
        const auto last_ns=controller.active->last_ns;
        ResetEvents();
        Check(QueueSession(app,app-1,1120,2436,geometry,closing,request,{},false),
              "real App arrives after its icon on the same immutable request");
        Pump(controller);
        Check(controller.active && controller.active->layer==app && !controller.active->starting &&
              controller.active->texture_width==0 && controller.overlapping.empty(),
              "real App takes over the single motion owner rather than an independent session");
        const auto linked=std::find_if(controller.active->companions.begin(),controller.active->companions.end(),
            [&](const Companion& item) {return item.layer==icon;});
        Check(linked!=controller.active->companions.end() && linked->texture_width==171 && linked->texture_height==171,
              "App handoff retains the icon capability and its square texture metadata in either direction");
        Check(Cleared().empty() && border_widths[app]>0 && border_widths[icon]>0,
              "icon-to-App handoff exposes neither a planar icon nor unprotected App source");
        Check(controller.active->last_ns==last_ns && controller.active->corner.value==corner.value &&
              controller.active->corner.velocity==corner.velocity,
              "App handoff preserves the exact motion timestamp and corner velocity");
        for(size_t i=0;i<scalars.size();++i)
            Check(controller.active->animation.Scalars()[i].value==scalars[i].value &&
                  controller.active->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "App handoff preserves every live spring position and velocity");
        CheckSharedIconFrame(app,icon);
        ResetEvents();Frame(last_ns+16000000LL,&controller);CheckSharedIconFrame(app,icon);
        ResetEvents();Stop(controller);CheckLinkedRetirement(app,icon);
    }
    std::puts("PASS icon-first opening and closing handoffs retain one live motion and source domains");
}
void LinkedIconPublicationCancellation() {
    auto& controller=Fresh();const auto geometry=LinkedGeometry(true);
    const auto request=NowNs();
    Check(QueueSession(1931,1930,1120,2436,geometry,true,request,{},false),
          "seed linked stream before a pending icon publication is cancelled");
    Pump(controller);
    const auto commit=PrimeTestIcon(1932,1933,geometry,true,request);
    ResetEvents();Stop(controller);
    Check(!controller.active && controller.retiring.size()==1 && Cleared().empty() && Running(),
          "global cancellation retains App and icon until the icon host publication commits");
    Commit(commit);Pump(controller);CheckLinkedRetirement(1931,1932);

    auto& deadline=Fresh();const auto later=NowNs();
    Check(QueueSession(1941,1940,1120,2436,geometry,true,later,{},false),
          "seed shared closing stream for deadline retirement");
    Pump(deadline);Commit(PrimeTestIcon(1942,1943,geometry,true,later));
    deadline.active->started_ns=NowNs()-2000000000LL;
    ResetEvents();Frame(NowNs(),&deadline);
    Check(!deadline.active,"closing deadline retires the shared motion owner");
    CheckLinkedRetirement(1941,1942,true);
    std::puts("PASS cancellation clears linked layers while deadline retains the visible icon mask");
}
void LinkedIconSeedLosesToAppPublication() {
    auto& controller=Fresh();const auto geometry=LinkedGeometry(true);
    const auto request=NowNs();
    auto* icon_transaction=ASurfaceTransaction_create();
    auto* app_transaction=ASurfaceTransaction_create();
    Check(icon_transaction && app_transaction,"create racing App and icon publication transactions");
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        pause_layer=true;paused_layer=1962;
    }
    std::atomic<bool> accepted{false};
    std::thread icon([&] {
        accepted=PrimeLinkedIcon(icon_transaction,1962,1963,171,171,1120,2436,
                                 geometry,true,request);
    });
    {
        std::unique_lock<std::mutex> lock(test_mutex);
        Wait(lock,layer_paused,"icon seed is constructed before it acquires controller publication lock");
    }
    Check(QueueSession(1961,1960,1120,2436,geometry,true,request,{},false,0,0,app_transaction),
          "App first publication wins while the provisional icon seed is being constructed");
    Check(controller.pending && controller.pending->layer==1961 && commits.size()==1,
          "App pending publication is established before the icon can enqueue its seed");
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        release_layer=true;test_cv.notify_all();
    }
    icon.join();Check(accepted,"racing icon joins the App publication that won the construction race");
    Check(controller.pending && controller.pending->layer==1961 && Owns(*controller.pending,1962) &&
          controller.retiring.empty() && controller.queued.empty() && commits.size()==2,
          "late seed construction retains App primary and adds only a linked icon plus its commit barrier");
    ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
    ASurfaceTransaction_apply(icon_transaction);ASurfaceTransaction_delete(icon_transaction);
    ResetEvents();Commit(1);Frame(NowNs(),&controller);
    Check(!controller.active && controller.pending && events.empty(),
          "icon commit cannot start the stream ahead of the winning App publication");
    Commit(0);Pump(controller);
    Check(controller.active && controller.active->layer==1961 && Owns(*controller.active,1962) &&
          controller.overlapping.empty() && Cleared().empty(),
          "both committed racing publications adopt one App owner without clearing the App source");
    ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);
    CheckSharedIconFrame(1961,1962);
    ResetEvents();Stop(controller);CheckLinkedRetirement(1961,1962);
    std::puts("PASS icon seed construction joins an App publication that wins the race");
}

void LinkedIconCannotCrossAppOwnership() {
    auto& controller=Fresh();const auto geometry=LinkedGeometry(true);
    const auto a_request=NowNs()-100000000LL;
    Check(QueueSession(1971,1970,1120,2436,geometry,true,a_request,{},false),
          "seed App A with its linked icon before an independent App starts");
    Pump(controller);Commit(PrimeTestIcon(1972,1973,geometry,true,a_request));
    const auto b_request=NowNs();
    Check(QueueSession(1974,1975,1120,2436,geometry,true,b_request,{},false),
          "start independent App B while closing A and its icon remain visible");
    Pump(controller);
    Check(controller.active->layer==1974 && controller.overlapping.size()==1 &&
          Owns(*controller.overlapping[0],1972),"independent App overlap retains A's icon ownership");
    auto* stolen=ASurfaceTransaction_create();Check(stolen,"create conflicting icon publication");
    ResetEvents();const auto callbacks=commits.size();
    Check(!PrimeLinkedIcon(stolen,1972,1973,171,171,1120,2436,geometry,true,b_request),
          "App B cannot bind an icon capability still owned by App A's live epoch");
    Check(events.empty() && commits.size()==callbacks && !Owns(*controller.active,1972) &&
          Owns(*controller.overlapping[0],1972),
          "rejected cross-App icon binding writes nothing and leaves both motion owners intact");
    ASurfaceTransaction_delete(stolen);
    ResetEvents();Frame(NowNs()+16000000LL,&controller);
    Check(events.size()==3,"both independent Apps and A's linked icon continue after rejection");
    const auto app=std::find_if(events.begin(),events.end(),[](const Event& event) {return event.layer==1971;});
    const auto icon=std::find_if(events.begin(),events.end(),[](const Event& event) {return event.layer==1972;});
    Check(app!=events.end() && icon!=events.end() && app->transaction==icon->transaction &&
          std::equal(layer_uniforms[1971].begin(),layer_uniforms[1971].begin()+36,layer_uniforms[1972].begin()),
          "App A retains its own shared icon mesh and transaction throughout independent App B motion");
    ResetEvents();Stop(controller);
    auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
    Check(cleared==std::vector<int>({1971,1972,1974}),"global stop retires both Apps and the correctly owned icon");
    DrainCompletions();Check(!Running(),"cross-App overlap releases every stream and detach completion");
    for(const auto& entry:references) Check(entry.second==0,"cross-App rejection retains no leaked capabilities");
    for(int proof=0;proof<5;++proof) {
        auto& isolated=Fresh();const auto opening=LinkedGeometry(false);
        const auto old_request=NowNs()-100000000LL;
        const std::vector<jlong> old_previews=proof==0?std::vector<jlong>{1976}:
            (proof>=2?std::vector<jlong>{1974}:std::vector<jlong>{});
        Check(QueueSession(1971,1970,1120,2436,opening,false,old_request,old_previews,true,
                           proof==4?171:0,proof==4?171:0),
              "seed old primary for explicit capability proof rejection");
        Pump(isolated);Commit(PrimeTestIcon(1972,1973,opening,false,old_request));
        if(proof==3) for(auto& companion:isolated.active->companions) if(companion.layer==1974) {
            companion.texture_width=171;companion.texture_height=171;
        }
        const auto next_request=NowNs();
        const std::vector<jlong> next_previews=proof==0?std::vector<jlong>{1976}:
            (proof==2?std::vector<jlong>{}:std::vector<jlong>{1971});
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create uncommitted App handoff");
        const auto app_commit=commits.size();
        Check(QueueSession(1974,1975,1120,2436,geometry,true,next_request,next_previews,true,
                           0,0,app_transaction),"prime a different primary with incomplete App capability proof");
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        auto* icon_transaction=ASurfaceTransaction_create();Check(icon_transaction,"create rejected icon handoff");
        ResetEvents();const auto pending_commits=commits.size();
        Check(!PrimeLinkedIcon(icon_transaction,1972,1973,171,171,1120,2436,geometry,true,next_request),
              "shared preview, one-way primary, or icon-typed primary cannot authorize cross-parent icon transfer");
        Check(events.empty() && commits.size()==pending_commits &&
              Owns(*isolated.active,1972) && !Owns(*isolated.pending,1972),
              "invalid App capability proof leaves old icon ownership and pending publication unchanged");
        ASurfaceTransaction_delete(icon_transaction);
        Commit(app_commit);Pump(isolated);Stop(isolated);DrainCompletions();
        Check(!Running(),"rejected capability handoff leaves no live owner or detach barrier");
        for(const auto& entry:references) Check(entry.second==0,"rejected handoff releases every capability");
    }
    std::puts("PASS a linked icon cannot migrate to another live App request");
}

void LinkedIconComposedPreviewReversal() {
    // Reproduce the rapid-home topology: the old preview owns the icon, while
    // a newly composed Task output explicitly retains that preview capability.
    for(const auto& order:std::array<std::array<int,3>,6>{{
            {{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}}) {
        auto& controller=Fresh();const auto opening=LinkedGeometry(false);
        const auto opening_request=NowNs()-100000000LL;
        Check(QueueSession(342,344,1120,2436,opening,false,opening_request,{},true),
              "seed the OEM opening preview before its composed Task output exists");
        Pump(controller);
        controller.active->animation.Advance(.07);
        genie::model::AdvanceScalar(&controller.active->corner,.07,1.0);
        const auto opening_commit=PrimeTestIcon(347,346,opening,false,opening_request);
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,344,opening_request);
        controller.active->cancel_ns=NowNs()-1;
        const auto opening_state=controller.active->animation.Scalars();
        const auto opening_frames=controller.active->frames;
        const auto closing=LinkedGeometry(true);const auto closing_request=NowNs();
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create composed close host transaction");
        const auto app_commit=commits.size();
        Check(QueueSession(352,183,1120,2436,closing,true,closing_request,{342,348},false,
                           0,0,app_transaction,reinterpret_cast<void*>(351)),
              "prime a composed closing output with the exact old preview capability");
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        Check(controller.pending && controller.pending->primed_continuation &&
              !Owns(*controller.pending,347) && !Owns(*controller.active,352),
              "composed close inherits motion but awaits explicit icon registration without a reverse output capability");
        for(size_t i=0;i<opening_state.size();++i)
            Check(controller.pending->animation.Scalars()[i].value==opening_state[i].value &&
                  controller.pending->animation.Scalars()[i].velocity==opening_state[i].velocity,
                  "composed close preserves paused opening spring positions and velocities");
        const auto app_snapshot=layer_uniforms.at(352);
        auto* icon_transaction=ASurfaceTransaction_create();Check(icon_transaction,"create composed icon close transaction");
        const auto icon_commit=commits.size();ResetEvents();
        Check(PrimeLinkedIcon(icon_transaction,347,346,171,171,1120,2436,closing,true,
                              closing_request,closing_request,183),
              "authenticated composed close accepts its explicitly retained opening preview's icon");
        Check(Owns(*controller.pending,347) && layer_uniforms.at(352)==app_snapshot &&
              std::equal(app_snapshot.begin(),app_snapshot.begin()+36,layer_uniforms.at(347).begin()),
              "icon close joins the composed snapshot without resetting or advancing its mesh");
        Check(layer_uniforms.at(347)[40]==171 && layer_uniforms.at(347)[41]==171,
              "composed close retains the icon's square source footprint");
        ASurfaceTransaction_apply(icon_transaction);ASurfaceTransaction_delete(icon_transaction);
        const std::array<size_t,3> barriers{opening_commit,app_commit,icon_commit};
        for(size_t index=0;index<order.size();++index) {
            ResetEvents();Commit(barriers[order[index]]);Pump(controller);
            if(index+1<order.size()) Check(controller.pending && controller.active &&
                controller.active->request_ns==opening_request &&
                controller.active->frames==opening_frames && events.empty(),
                "no commit ordering lets the old preview overwrite the new icon or starts an incomplete closing publication");
        }
        Check(controller.active && controller.active->layer==352 && !controller.pending &&
              controller.overlapping.empty() && Owns(*controller.active,347) &&
              Owns(*controller.active,342) && Owns(*controller.active,348),
              "all publications commit into one composed closing owner with its registered preview and icon");
        ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);
        Check(events.size()==4 && std::all_of(events.begin(),events.end(),[&](const Event& event) {
            return !event.clear && event.transaction==events.front().transaction;
        }),"composed output, old previews and icon advance in one transaction after adoption");
        Check(std::equal(layer_uniforms.at(352).begin(),layer_uniforms.at(352).begin()+36,
                         layer_uniforms.at(347).begin()),"composed output and icon retain one live mesh");
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,344,opening_request);
        ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);
        Check(controller.active && controller.active->request_ns==closing_request && events.size()==4,
              "a delayed stop for the old preview cannot stop the composed closing icon");
        Stop(controller);DrainCompletions();Check(!Running(),"composed icon reversal drains every publication and detach");
        for(const auto& entry:references) Check(entry.second==0,"composed icon reversal releases all retained capabilities");
    }
    std::puts("PASS rapid-home composed preview icon reversal across every host commit order");
}

void ComposedCloseInheritsEstablishedDonor() {
    // The real rapid-home topology after preview→content takeover: the donor is
    // an established content owner (starting=false) on its own output layer,
    // paused at reversal. A composed close on a new output layer carries the old
    // previews as companions. Its first publication must inherit the donor's
    // mesh motion instead of starting flat from zero deformation.
    auto& controller=Fresh();const auto opening=LinkedGeometry(false);
    const auto opening_request=NowNs()-100000000LL;
    Check(QueueSession(340,344,1120,2436,opening,false,opening_request,{342},false),
          "seed an established composed opening owner, not a starting preview");
    Pump(controller);
    Check(controller.active&&controller.active->layer==340&&!controller.active->starting,
          "established opening owner adopted with its preview companion");
    controller.active->animation.Advance(.07);
    genie::model::AdvanceScalar(&controller.active->corner,.07,1.0);
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,344,opening_request);
    controller.active->cancel_ns=NowNs()-1;
    const auto opening_state=controller.active->animation.Scalars();
    const auto closing=LinkedGeometry(true);const auto closing_request=NowNs();
    auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create established-donor close transaction");
    const auto app_commit=commits.size();
    Check(QueueSession(352,183,1120,2436,closing,true,closing_request,{342,348},false,
                       0,0,app_transaction,reinterpret_cast<void*>(351)),
          "prime the composed close on a new output while its established donor is paused");
    ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
    Check(controller.pending&&controller.pending->layer==352&&controller.pending->primed_continuation,
          "a new-output composed close inherits the established donor instead of starting flat");
    for(size_t i=0;i<opening_state.size();++i)
        Check(controller.pending->animation.Scalars()[i].value==opening_state[i].value &&
              controller.pending->animation.Scalars()[i].velocity==opening_state[i].velocity,
              "established-donor close preserves every spring position and velocity");
    Commit(app_commit);Pump(controller);
    Stop(controller);DrainCompletions();Check(!Running(),"established-donor reversal drains its publication");
    for(const auto& entry:references) Check(entry.second==0,"established-donor reversal releases all capabilities");
    std::puts("PASS composed close inherits an established content donor across output layers");
}

void AdjacentSameParentStreamsDoNotInherit() {
    // Same parent, same domain, same direction, different request epochs and no
    // shared companion: adjacent streams on one Task host must not inherit each
    // other's donor motion merely because Related() sees their common parent.
    auto& controller=Fresh();const auto opening=LinkedGeometry(false);
    const auto first_request=NowNs()-100000000LL;
    Check(QueueSession(340,344,1120,2436,opening,false,first_request,{},false),
          "seed the first same-parent stream");
    Pump(controller);
    controller.active->animation.Advance(.07);
    const auto second_request=NowNs();
    auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create adjacent stream transaction");
    Check(QueueSession(352,344,1120,2436,opening,false,second_request,{},false,
                       0,0,app_transaction,nullptr),
          "prime the adjacent same-parent stream with no shared companion");
    ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
    Check(controller.pending&&!controller.pending->primed_continuation,
          "same-parent adjacent streams without shared proof never inherit donor motion");
    Commit(commits.size()-1);Pump(controller);
    Stop(controller);DrainCompletions();Check(!Running(),"adjacent-stream rejection drains its publication");
    for(const auto& entry:references) Check(entry.second==0,"adjacent-stream rejection releases all capabilities");
    std::puts("PASS adjacent same-parent streams require shared stream proof");
}

void OppositeDirectionWithoutSharedProofDoesNotInherit() {
    // A reversal on the same parent without shared preview/companion proof is
    // not a stream continuation either: direction alone must never admit motion.
    auto& controller=Fresh();const auto opening=LinkedGeometry(false);
    const auto open_request=NowNs()-100000000LL;
    Check(QueueSession(340,344,1120,2436,opening,false,open_request,{},false),
          "seed the opposite-direction donor");
    Pump(controller);
    controller.active->animation.Advance(.07);
    const auto close_request=NowNs();
    auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create opposite-direction transaction");
    Check(QueueSession(352,344,1120,2436,LinkedGeometry(true),true,close_request,{},false,
                       0,0,app_transaction,nullptr),
          "prime the opposite-direction stream with no shared companion");
    ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
    Check(controller.pending&&!controller.pending->primed_continuation,
          "opposite-direction streams without shared proof never inherit donor motion");
    Commit(commits.size()-1);Pump(controller);
    Stop(controller);DrainCompletions();Check(!Running(),"opposite-direction rejection drains its publication");
    for(const auto& entry:references) Check(entry.second==0,"opposite-direction rejection releases all capabilities");
    std::puts("PASS opposite-direction streams require shared stream proof");
}

std::array<float,4> ScreenQuad(const std::array<float,44>& values) {
    float left=values[0],top=values[1],right=values[0],bottom=values[1];
    for(size_t i=1;i<9;++i) {
        left=std::min(left,values[i*4]);right=std::max(right,values[i*4]);
        top=std::min(top,values[i*4+1]);bottom=std::max(bottom,values[i*4+1]);
    }
    return {left*1120.f,top*2436.f,right*1120.f,bottom*2436.f};
}
void ComposedCloseKeepsPresentedContour() {
    // The observed 20-50ms failure: the old owner's handoff/retire dropped the
    // icon's mask before any successor registered the same child, so the raw
    // plate flashed until re-registration. The first closing frames must keep
    // the mask and continue the last presented contour in screen space.
    auto& controller=Fresh();
    const auto opening=LinkedGeometry(false);
    const auto open_request=NowNs()-100000000LL;
    Check(QueueSession(340,344,1120,2436,opening,false,open_request,{342},false),
          "seed the established opening owner with its shared preview");
    Pump(controller);
    Commit(PrimeTestIcon(347,346,opening,false,open_request));
    Pump(controller);
    controller.active->animation.Advance(.07);
    genie::model::AdvanceScalar(&controller.active->corner,.07,1.0);
    Frame(controller.active->last_ns+16000000LL,&controller);
    Check(layer_uniforms.count(347)==1,"presented icon carries its mesh mask");
    const auto presented=ScreenQuad(layer_uniforms.at(347));
    const auto presented_mask=layer_uniforms.at(347);
    ResetEvents();
    const auto closing=LinkedGeometry(true);
    const auto closing_request=NowNs();
    auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create closing publication transaction");
    const auto app_commit=commits.size();
    Check(QueueSession(352,183,1120,2436,closing,true,closing_request,{342,348},false,
                       0,0,app_transaction,reinterpret_cast<void*>(351)),
          "publish the composed close on its new output before icon re-registration");
    ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
    Commit(app_commit);Pump(controller);
    const auto cleared=Cleared();
    Check(std::find(cleared.begin(),cleared.end(),347)==cleared.end(),
          "handoff keeps the presented icon mask while a live successor will present the child");
    Check(std::find(cleared.begin(),cleared.end(),342)==cleared.end(),
          "handoff keeps the presented preview mask while a live successor will present it");
    Check(layer_uniforms.count(347)==1&&layer_uniforms.at(347)==presented_mask,
          "the retained icon keeps its last presented mask, not a raw plate");
    const auto continued=ScreenQuad(layer_uniforms.at(347));
    auto within=[&](const std::array<float,4>& a,const std::array<float,4>& b) {
        for(size_t i=0;i<4;++i) if(std::abs(a[i]-b[i])>8.f) return false;
        return true;
    };
    Check(within(continued,presented),
          "first closing frames continue the last presented contour in screen space");
    Stop(controller);DrainCompletions();Check(!Running(),"contour continuity drains every publication");
    for(const auto& entry:references) Check(entry.second==0,"contour continuity releases all capabilities");
    std::puts("PASS closing publication keeps the presented contour with its mask in place");
}

void LinkedIconComposedPreviewProofIsolation() {
    for(int invalid=0;invalid<8;++invalid) {
        auto& controller=Fresh();const auto opening=LinkedGeometry(false);
        const auto opening_request=NowNs()-100000000LL;
        Check(QueueSession(342,344,1120,2436,opening,false,opening_request,{},true),
              "seed the exact preview and icon before checking composed-close proof isolation");
        Pump(controller);Commit(PrimeTestIcon(347,346,opening,false,opening_request));
        const auto closing=LinkedGeometry(true);const auto closing_request=NowNs();
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create composed proof test transaction");
        const auto app_commit=commits.size();
        Check(QueueSession(352,183,1120,2436,closing,true,closing_request,
                           invalid==0?std::vector<jlong>{348}:std::vector<jlong>{342,348},false,
                           0,0,app_transaction,reinterpret_cast<void*>(351)),
              "prime a group before corrupting one required predecessor proof");
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        if(invalid==1) controller.active->starting=false;
        if(invalid==2) controller.active->texture_width=171;
        if(invalid==3) for(auto& item:controller.pending->companions) if(item.layer==342) item.texture_width=171;
        if(invalid==4) {
            controller.active->cancel_ns=NowNs()-1;
            controller.active->started_ns=NowNs()-1500000001LL;
        }
        auto* icon_transaction=ASurfaceTransaction_create();Check(icon_transaction,"create rejected composed icon transaction");
        ResetEvents();const auto callbacks=commits.size();
        Check(!PrimeLinkedIcon(icon_transaction,347,346,171,171,1120,2436,closing,true,closing_request,
                               invalid==5?closing_request-1:closing_request,
                               invalid==6?-1:(invalid==7?344:183)),
              "a composed output cannot bypass an absent, expired, icon-typed, or mismatched preview/request proof");
        Check(events.empty() && commits.size()==callbacks && Owns(*controller.active,347) &&
              !Owns(*controller.pending,347),"invalid composed proof leaves both ownership and transaction unchanged");
        ASurfaceTransaction_delete(icon_transaction);Commit(app_commit);Pump(controller);
        Stop(controller);DrainCompletions();Check(!Running(),"rejected composed proof drains normal publication cleanup");
        for(const auto& entry:references) Check(entry.second==0,"rejected composed proof leaks no capabilities");
    }
    std::puts("PASS composed icon transfer requires exact live preview and bridge Task/request proof");
}

void LinkedIconPendingReversalPublication() {
    auto& controller=Fresh();const auto closing=LinkedGeometry(true);
    const auto old_request=NowNs()-100000000LL;
    Check(QueueSession(1981,1980,1120,2436,closing,true,old_request,{},false),
          "seed closing App and linked icon before an uncommitted reversal");
    Pump(controller);Commit(PrimeTestIcon(1982,1983,closing,true,old_request));
    controller.active->animation.Advance(.09);
    genie::model::AdvanceScalar(&controller.active->corner,.09,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto old_frames=controller.active->frames;
    const auto opening=LinkedGeometry(false);
    const auto new_request=NowNs();
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create pending reversal App publication");
    Check(QueueSession(1981,1980,1120,2436,opening,false,new_request,{},false,0,0,transaction),
          "prime new opening epoch without committing its App host transaction");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    Check(controller.active->request_ns==old_request && controller.pending &&
          controller.pending->request_ns==new_request && Owns(*controller.active,1982) &&
          !Owns(*controller.pending,1982),
          "old active retains its icon while a new pending reversal awaits explicit icon registration");
    const auto pending_app=layer_uniforms[1981];
    ResetEvents();
    const auto icon_commit=PrimeTestIcon(1982,1983,opening,false,new_request);
    Check(Owns(*controller.pending,1982),
          "explicit icon registration authorizes the capability for the pending reversal");
    Check(layer_uniforms[1981]==pending_app,"icon publication leaves the pending App state unchanged");
    Check(std::equal(layer_uniforms[1981].begin(),layer_uniforms[1981].begin()+36,
                     layer_uniforms[1982].begin()) &&
          layer_uniforms[1981][38]==layer_uniforms[1982][38],
          "new icon publication uses the pending reversal mesh and corner rather than old active state");
    ResetEvents();Commit(icon_commit);Frame(NowNs(),&controller);
    Check(controller.active->request_ns==old_request && controller.active->frames==old_frames && events.empty(),
          "icon commit cannot overtake the pending reversal App publication");
    Commit(icon_commit-1);Pump(controller);
    Check(controller.active->request_ns==new_request && !controller.active->closing && !controller.pending &&
          Owns(*controller.active,1982),"both commits adopt the reversed App and icon as a single owner");
    ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(1981,1982);
    ResetEvents();Stop(controller);CheckLinkedRetirement(1981,1982);
    for(bool icon_first:{false,true}) {
        auto& handoff=Fresh();
        const auto source_request=NowNs()-100000000LL;
        Check(QueueSession(1981,1980,1120,2436,opening,false,source_request,{1984},true),
              "seed App opening with the registered preview as an explicit capability");
        Pump(handoff);Commit(PrimeTestIcon(1982,1983,opening,false,source_request));
        auto* source=handoff.active.get();
        source->animation.Advance(.07);genie::model::AdvanceScalar(&source->corner,.07,1.0);
        Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,1980,source_request);
        source->cancel_ns=NowNs()-1;
        const auto paused_state=source->animation.Scalars();const auto paused_corner=source->corner;
        const auto paused_frames=source->frames;
        const auto close_request=NowNs();
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create registered preview close transaction");
        const auto app_commit=commits.size();
        Check(QueueSession(1984,1985,1120,2436,closing,true,close_request,{1981},true,
                           0,0,app_transaction),
              "registered close changes primary and parent while explicitly retaining the old App primary");
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        Check(handoff.pending && handoff.pending->primed_continuation &&
              handoff.active.get()==source && !Owns(*handoff.pending,1982),
              "registered close waits for host commit and fresh icon authorization");
        const auto pending_last=handoff.pending->last_ns;
        const auto pending_uniforms=layer_uniforms[1984];
        ResetEvents();const auto icon_commit=PrimeTestIcon(1982,1983,closing,true,close_request);
        Check(Owns(*handoff.pending,1982) && handoff.pending->last_ns==pending_last &&
              layer_uniforms[1984]==pending_uniforms,
              "mutual App-primary capabilities authorize the icon without resampling its pending owner");
        for(size_t i=0;i<paused_state.size();++i)
            Check(handoff.pending->animation.Scalars()[i].value==paused_state[i].value &&
                  handoff.pending->animation.Scalars()[i].velocity==paused_state[i].velocity,
                  "registered close and icon retain each paused coordinate and velocity");
        Check(handoff.pending->corner.value==paused_corner.value &&
              handoff.pending->corner.velocity==paused_corner.velocity,
              "registered close and icon retain paused corner position and velocity");
        Check(std::equal(layer_uniforms[1984].begin(),layer_uniforms[1984].begin()+36,
                         layer_uniforms[1982].begin()) && layer_uniforms[1984][38]==layer_uniforms[1982][38],
              "registered App and icon publish one inherited silhouette");
        ResetEvents();Commit(icon_first?icon_commit:app_commit);Frame(NowNs(),&handoff);
        Check(handoff.active.get()==source && handoff.active->frames==paused_frames && events.empty(),
              "neither registered App nor icon commit can overtake the other publication");
        Commit(icon_first?app_commit:icon_commit);Pump(handoff);
        Check(handoff.active && handoff.active->layer==1984 && handoff.active->parent_layer==1985 &&
              handoff.active->closing && handoff.active->request_ns==close_request &&
              Owns(*handoff.active,1981) && Owns(*handoff.active,1982) && handoff.overlapping.empty(),
              "both commits adopt one closing owner across the authenticated primary and parent handoff");
        ResetEvents();Frame(handoff.active->last_ns+16000000LL,&handoff);
        Check(events.size()==3 && std::all_of(events.begin(),events.end(),[&](const Event& event) {
                  return !event.clear && event.transaction==events.front().transaction &&
                      std::equal(event.uniforms.begin(),event.uniforms.begin()+36,layer_uniforms[1984].begin()) &&
                      event.uniforms[38]==layer_uniforms[1984][38];
              }),"App, registered preview, and icon continue with one mesh and transaction");
        ResetEvents();Stop(handoff);DrainCompletions();
        auto cleared=Cleared();std::sort(cleared.begin(),cleared.end());
        Check(cleared==std::vector<int>({1981,1982,1984}) && !Running(),
              "registered handoff retires every App and icon capability after disable");
        for(const auto& entry:references) Check(entry.second==0,"registered handoff retains no capability references");
    }
    std::puts("PASS new icon publication joins an uncommitted reversal while old active retains its capability");
}

std::vector<Event> TransactionWrites(void* transaction) {
    std::lock_guard<std::mutex> lock(test_mutex);
    std::vector<Event> writes;
    for(const auto& event:events) if(event.transaction==transaction) writes.push_back(event);
    return writes;
}
using VisibleUniforms=std::map<int,std::array<float,44>>;
void PresentWrites(VisibleUniforms& visible,const std::vector<Event>& writes) {
    for(const auto& write:writes) {
        if(write.clear) visible.erase(write.layer);
        else visible[write.layer]=write.uniforms;
    }
}
void CheckVisibleContour(const VisibleUniforms& visible,int app,int icon) {
    if(!visible.count(app) || !visible.count(icon)) return;
    const auto& a=visible.at(app);const auto& i=visible.at(icon);
    Check(std::equal(a.begin(),a.begin()+36,i.begin()) && a[38]==i[38] && a[39]==i[39],
          "each visible transaction commit keeps App and icon mesh and corner in the same presentation state");
}
void WorkerCoalescencePreservesPrimedSnapshot() {
    for(bool closing:{false,true}) for(bool icon_first:{false,true}) {
        auto& controller=Fresh();
        const auto old_geometry=LinkedGeometry(!closing);
        const auto old_request=NowNs()-100000000LL;
        Check(QueueSession(2091,2090,1120,2436,old_geometry,!closing,old_request,{},false),
              "seed a moving App before worker/icon publication race");
        Pump(controller);Commit(PrimeTestIcon(2092,2094,old_geometry,!closing,old_request));
        controller.active->animation.Advance(.09);
        genie::model::AdvanceScalar(&controller.active->corner,.09,1.0);
        controller.active->last_ns=NowNs()+10000000LL;
        const auto request=NowNs();auto geometry=LinkedGeometry(closing);
        if(closing) geometry[10]=0; // Captured OEM close input has a zero starting corner.
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create primed reversal host transaction");
        const auto app_commit=commits.size();
        Check(QueueSession(2091,2090,1120,2436,geometry,closing,request,{},false,0,0,app_transaction),
              "prime a reversal with inherited live contour and corner");
        const auto app_snapshot=layer_uniforms[2091];
        const auto scalars=controller.pending->animation.Scalars();
        const auto corner=controller.pending->corner;
        const auto last_ns=controller.pending->last_ns;
        Check(app_snapshot[38]>0 && std::abs(corner.velocity)>1e-6,
              "race seed has a rounded inherited silhouette and observable corner velocity");
        Check(QueueSession(2091,2090,1120,2436,geometry,closing,request,{2093},false),
              "worker discovery coalesces the same epoch before host adoption");
        Check(controller.pending && controller.pending->corner.value==corner.value &&
              controller.pending->corner.velocity==corner.velocity && controller.pending->last_ns==last_ns,
              "unprimed same-epoch discovery preserves the already-published corner and frozen clock");
        for(size_t i=0;i<scalars.size();++i)
            Check(controller.pending->animation.Scalars()[i].value==scalars[i].value &&
                  controller.pending->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "unprimed same-epoch discovery preserves every published mesh position and velocity");
        Check(!Owns(*controller.pending,2092) && Owns(*controller.pending,2093),
              "same-epoch discovery retains its new preview without importing an unregistered old icon");
        auto* icon_transaction=ASurfaceTransaction_create();Check(icon_transaction,"create racing icon transaction");
        const auto icon_commit=commits.size();ResetEvents();
        Check(PrimeLinkedIcon(icon_transaction,2092,2094,171,171,1120,2436,geometry,closing,request),
              "icon publication joins the coalesced pending owner");
        Check(Owns(*controller.pending,2092) && Owns(*controller.pending,2093),
              "explicit icon registration joins the pending owner alongside its current preview");
        Check(layer_uniforms[2091]==app_snapshot,
              "icon publication cannot overwrite the correct App prime with fresh endpoint geometry");
        const auto icon_writes=TransactionWrites(icon_transaction);
        VisibleUniforms visible;PresentWrites(visible,icon_writes);CheckVisibleContour(visible,2091,2092);
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        ASurfaceTransaction_apply(icon_transaction);ASurfaceTransaction_delete(icon_transaction);
        ResetEvents();Commit(icon_first?icon_commit:app_commit);Pump(controller);Frame(NowNs(),&controller);
        Check(controller.active && controller.active->request_ns==old_request && events.empty(),
              "neither host commit order advances the old or pending writer before both barriers");
        Commit(icon_first?app_commit:icon_commit);Pump(controller);
        Check(controller.active && controller.active->request_ns==request &&
              controller.active->corner.value==corner.value && controller.active->corner.velocity==corner.velocity,
              "both host commits adopt the exact inherited corner rather than an endpoint reset");
        for(size_t i=0;i<scalars.size();++i)
            Check(controller.active->animation.Scalars()[i].value==scalars[i].value &&
                  controller.active->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "adoption preserves every primed spring through worker coalescence");
        Stop(controller);
    }
    std::puts("PASS worker discovery cannot reset a primed contour before linked-icon publication");
}

void LinkedIconWholeOwnerPublication() {
    auto& controller=Fresh();const auto geometry=LinkedGeometry(true);const auto request=NowNs();
    Check(QueueSession(2011,2010,1120,2436,geometry,true,request,{},false),
          "seed visible App before attaching a newly visible icon");
    Pump(controller);
    VisibleUniforms visible;
    for(const auto& write:events) if(!write.clear) visible[write.layer]=write.uniforms;
    controller.active->animation.Advance(.08);
    genie::model::AdvanceScalar(&controller.active->corner,.08,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto old_app=visible.at(2011);
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create whole-owner icon publication");
    ResetEvents();
    Check(PrimeLinkedIcon(transaction,2012,2013,171,171,1120,2436,geometry,true,request),
          "new icon publication accepts the current shared App state");
    const auto writes=TransactionWrites(transaction);
    Check(writes.size()==2 && writes[0].layer==2011 && writes[1].layer==2012,
          "first icon transaction republishes the App together with its icon at one snapshot");
    Check(!std::equal(old_app.begin(),old_app.begin()+36,writes[0].uniforms.begin()),
          "whole-owner publication regression distinguishes old visible App from its current unpublished state");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    Commit(0);PresentWrites(visible,writes);CheckVisibleContour(visible,2011,2012);
    ResetEvents();Stop(controller);CheckLinkedRetirement(2011,2012);
    std::puts("PASS first linked icon transaction publishes the whole owner at one visible snapshot");
}
void LinkedIconAppCommitOrders() {
    for(bool app_first:{false,true}) {
        auto& controller=Fresh();const auto geometry=LinkedGeometry(true);
        const auto request=NowNs()-100000000LL;
        auto* icon_transaction=ASurfaceTransaction_create();
        auto* app_transaction=ASurfaceTransaction_create();
        Check(icon_transaction && app_transaction,"create separate icon-first and App-handoff publications");
        Check(PrimeLinkedIcon(icon_transaction,2022,2023,171,171,1120,2436,geometry,true,request),
              "prime icon-first owner before either host transaction commits");
        const auto icon_writes=TransactionWrites(icon_transaction);
        Check(icon_writes.size()==1 && icon_writes[0].layer==2022,
              "icon-first transaction contains the original provisional icon state");
        Check(controller.pending && !Committed(*controller.pending),"provisional icon publication is still gated");
        // Simulate waiting on an outstanding host transaction without sleeping.
        // The visible state remains its published mesh at this earlier clock.
        controller.pending->last_ns=NowNs()-50000000LL;
        controller.pending->elapsed=static_cast<double>(controller.pending->last_ns-controller.pending->started_ns)/1e9;
        const auto held_last=controller.pending->last_ns;
        const auto held_elapsed=controller.pending->elapsed;
        const auto held=controller.pending->animation.Scalars();
        ResetEvents();
        Check(QueueSession(2021,2020,1120,2436,geometry,true,request,{},false,0,0,app_transaction),
              "prime real App while the original icon host publication is uncommitted");
        const auto app_writes=TransactionWrites(app_transaction);
        Check(app_writes.size()==2 && app_writes[0].layer==2021 && app_writes[1].layer==2022,
              "App handoff transaction contains both the new App and retained icon");
        Check(controller.pending->last_ns==held_last && controller.pending->elapsed==held_elapsed,
              "inherited prime preserves the frozen source clock instead of advancing across an uncommitted publication");
        for(size_t i=0;i<held.size();++i)
            Check(controller.pending->animation.Scalars()[i].value==held[i].value &&
                  controller.pending->animation.Scalars()[i].velocity==held[i].velocity,
                  "inherited App prime preserves every spring behind the original icon commit barrier");
        ASurfaceTransaction_apply(icon_transaction);ASurfaceTransaction_delete(icon_transaction);
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        VisibleUniforms visible;
        const size_t first=app_first?1:0,second=app_first?0:1;
        Commit(first);PresentWrites(visible,app_first?app_writes:icon_writes);
        CheckVisibleContour(visible,2021,2022);
        ResetEvents();Pump(controller);Frame(NowNs(),&controller);
        Check(!controller.active && controller.pending && events.empty(),
              "first commit in either order cannot advance the shared stream before the other publication");
        Commit(second);PresentWrites(visible,app_first?icon_writes:app_writes);
        CheckVisibleContour(visible,2021,2022);
        ResetEvents();Pump(controller);
        for(const auto& write:events) PresentWrites(visible,{write});
        CheckVisibleContour(visible,2021,2022);
        Check(controller.active && controller.active->layer==2021 && !controller.pending,
              "both commit orders eventually adopt one App owner with the original icon");
        ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(2021,2022);
        ResetEvents();Stop(controller);CheckLinkedRetirement(2021,2022);
    }
    std::puts("PASS icon/App transactions preserve visible contour through either commit order");
}

void RejectedLateIcon(int icon,int parent,const std::array<float,12>& geometry,int64_t request) {
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create late retired icon publication");
    ResetEvents();const auto callbacks=commits.size();const auto applies=renderer_applies.load();
    Check(!PrimeLinkedIcon(transaction,icon,parent,171,171,1120,2436,geometry,true,request),
          "stopped or retired request rejects a late icon capability before any shader publication");
    Check(events.empty() && commits.size()==callbacks && renderer_applies.load()==applies,
          "rejected late icon adds no shader writes, commit barrier, or independent apply");
    ASurfaceTransaction_delete(transaction);
}
void LinkedIconCannotReviveCancelledRequest() {
    const auto geometry=LinkedGeometry(true);
    for(bool published:{false,true}) {
        auto& controller=Fresh();const auto request=NowNs();
        Check(QueueSession(1991,1990,1120,2436,geometry,true,request,{},false),
              "seed App before cancelling a linked icon stream");
        Pump(controller);
        const auto commit=PrimeTestIcon(1992,1993,geometry,true,request);
        if(published) Commit(commit);
        genie::CancelIconRenderer();
        Check(controller.stop_requested,"global cancellation is pending before its worker consumes it");
        RejectedLateIcon(1994,1995,geometry,request);
        Pump(controller);
        Check(!controller.active && !controller.pending,"cancelled stream has no live motion owner");
        if(!published) {
            Check(controller.retiring.size()==1,"cancelled uncommitted icon remains owned until its host commit");
            RejectedLateIcon(1994,1995,geometry,request);
            Commit(commit);Pump(controller);
        }
        DrainCompletions();
        Check(controller.retiring.empty() && !Running(),"cancelled icon stream completes its retirement");
        RejectedLateIcon(1994,1995,geometry,request);
        Check(!controller.active && !controller.pending && controller.queued.empty() && controller.overlapping.empty(),
              "a late icon cannot seed a fresh provisional owner for a retired epoch");
        for(const auto& entry:references) Check(entry.second==0,"cancelled and rejected icon paths release every capability");
    }
    std::puts("PASS late icon cannot revive cancellation before or after host commit and retirement");
}
void LinkedIconSelectsPendingPublication() {
    auto& controller=Fresh();const auto geometry=LinkedGeometry(true);const auto request=NowNs();
    Check(QueueSession(2001,2000,1120,2436,geometry,true,request,{},false),
          "seed App before repeating a same-request first-show publication");
    Pump(controller);Commit(PrimeTestIcon(2002,2003,geometry,true,request));
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create newer pending same-request App publication");
    Check(QueueSession(2001,2000,1120,2436,geometry,true,request,{},false,0,0,transaction),
          "prime pending same-request owner while the previous App owner remains active");
    Check(controller.pending && controller.active,"same epoch has both active and pending publication states");
    // Deliberately make the pending publication distinguishable while the old
    // owner carries a newer callback timestamp. Selection must follow the
    // uncommitted publication, rather than choosing the largest last_ns.
    controller.pending->animation.Advance(.04);
    genie::model::AdvanceScalar(&controller.pending->corner,.04,1.0);
    controller.active->last_ns=controller.pending->last_ns+10000000LL;
    Check(Submit(controller,*controller.pending,false,transaction),"publish a distinct pending contour in its borrowed transaction");
    const auto pending=layer_uniforms[2001];
    const auto old_mesh=genie::model::ContourMesh(controller.active->animation,controller.active->parameters);
    Check(std::abs(pending[0]-old_mesh[0].x)>1e-6 || std::abs(pending[1]-old_mesh[0].y)>1e-6,
          "pending and active contours differ so owner-selection regression is observable");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    const auto icon_commit=PrimeTestIcon(2002,2003,geometry,true,request);
    Check(std::equal(pending.begin(),pending.begin()+36,layer_uniforms[2002].begin()) &&
          pending[38]==layer_uniforms[2002][38],
          "icon joins the pending publication even when old active has a newer frame timestamp");
    const auto frames=controller.active->frames;
    Commit(icon_commit);ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active->frames==frames && events.empty(),"same-request icon still waits for the pending App transaction");
    Commit(icon_commit-1);Pump(controller);
    ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(2001,2002);
    ResetEvents();Stop(controller);CheckLinkedRetirement(2001,2002);
    std::puts("PASS linked icon prioritizes the pending publication over a newer old-owner timestamp");
}

void LinkedIconAdjustPrecedesAppDispatch() {
    for(bool pending_source:{false,true}) for(bool was_closing:{false,true})
    for(int source_form:{0,1,2}) {
        auto& controller=Fresh();
        const auto old_geometry=LinkedGeometry(was_closing);
        const auto next_geometry=LinkedGeometry(!was_closing);
        const auto source_request=NowNs()-80000000LL;
        auto* source_transaction=pending_source?ASurfaceTransaction_create():nullptr;
        if(pending_source) Check(source_transaction,"create uncommitted predecessor App transaction");
        Check(QueueSession(2031,2030,1120,2436,old_geometry,was_closing,source_request,{},false,
                           0,0,source_transaction),"seed exact predecessor before App adjust dispatch reaches the worker");
        if(!pending_source) Pump(controller);
        Session* source=pending_source?controller.pending.get():controller.active.get();
        Check(source && source->request_ns==source_request,"predecessor is in the requested pending or committed state");
        source->animation.Advance(.07);genie::model::AdvanceScalar(&source->corner,.07,1.0);
        source->last_ns=NowNs()+10000000LL;
        if(pending_source) Check(Submit(controller,*source,false,source_transaction),
                                "predecessor App transaction records its current model state before icon attachment");
        const auto source_icon_commit=PrimeTestIcon(2032,2033,old_geometry,was_closing,source_request);
        if(pending_source) {
            ASurfaceTransaction_apply(source_transaction);ASurfaceTransaction_delete(source_transaction);
            Check(!controller.active && !Committed(*source),"pending predecessor retains both outstanding first publications");
        } else Commit(source_icon_commit);
        const auto scalars=source->animation.Scalars();
        const auto corner=source->corner;
        const auto last_ns=source->last_ns;
        const auto frames=source->frames;
        const auto generation=source->generation;
        Check(std::any_of(scalars.begin(),scalars.end(),[](const genie::model::Scalar& scalar) {
                  return std::abs(scalar.velocity)>1e-6;
              }) && std::abs(corner.velocity)>1e-6,
              "adjust continuity seed has observable coordinate and corner velocities");
        const auto next_request=NowNs();
        source->cancel_ns=NowNs()+48000000LL;
        auto* adjust_transaction=ASurfaceTransaction_create();Check(adjust_transaction,"create icon adjust preceding App dispatch");
        const auto adjust_commit=commits.size();
        ResetEvents();
        const auto bridge_source=source_form==0?source_request:
            source_form==1?next_request:source_request+1;
        const int32_t bridge_parent=source_form==0?-1:2030;
        Check(PrimeLinkedIcon(adjust_transaction,2032,2033,171,171,1120,2436,next_geometry,
                              !was_closing,next_request,bridge_source,bridge_parent),
              "icon adjust joins its exact predecessor or same-parent native owner before the App worker sees the new epoch");
        Check(RequestRunning(source_request) && RequestRunning(next_request),
              "native icon retag protects old cleanup before the bridge publishes a new App worker");
        const auto adjust_writes=TransactionWrites(adjust_transaction);
        Check(adjust_writes.size()==2 && adjust_writes[0].layer==2031 && adjust_writes[1].layer==2032,
              "source-based icon adjust republishes App and icon in one borrowed transaction");
        CheckSharedIconFrame(2031,2032);
        Check((pending_source?controller.pending.get():controller.active.get())==source &&
              source->request_ns==next_request && source->closing!=was_closing && source->cancel_ns==0 &&
              source->started_ns==next_request && source->generation==generation+1,
              "source-based reversal retargets its existing owner and advances only request identity and target");
        Check(source->last_ns==last_ns && source->frames==frames && source->corner.value==corner.value &&
              source->corner.velocity==corner.velocity,
              "source-based reversal preserves presentation timestamp, frame count, and corner velocity");
        for(size_t i=0;i<scalars.size();++i)
            Check(source->animation.Scalars()[i].value==scalars[i].value &&
                  source->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "source-based reversal preserves every spring position and velocity before asynchronous App dispatch");
        Check(source->parameters.response==(!was_closing?genie::model::kClosingResponse:genie::model::kOpeningResponse) &&
              source->corner.response==source->parameters.response,
              "source-based reversal applies the new opening or closing response to shared motion and corner");
        ASurfaceTransaction_apply(adjust_transaction);ASurfaceTransaction_delete(adjust_transaction);
        auto* app_transaction=ASurfaceTransaction_create();Check(app_transaction,"create later App adjust worker publication");
        const auto app_commit=commits.size();
        ResetEvents();
        Check(QueueSession(2031,2030,1120,2436,next_geometry,!was_closing,next_request,{},false,
                           0,0,app_transaction),"asynchronous App worker subsequently joins the icon-retargeted epoch");
        Check(controller.pending && controller.pending->request_ns==next_request && Owns(*controller.pending,2032),
              "later App publication retains the icon on the already-retargeted shared owner");
        const auto app_writes=TransactionWrites(app_transaction);
        Check(app_writes.size()==2 && app_writes[0].uniforms==adjust_writes[0].uniforms &&
              app_writes[1].uniforms==adjust_writes[1].uniforms,
              "later App publication copies the exact frozen App and icon snapshot without restarting either");
        for(size_t i=0;i<scalars.size();++i)
            Check(controller.pending->animation.Scalars()[i].value==scalars[i].value &&
                  controller.pending->animation.Scalars()[i].velocity==scalars[i].velocity,
                  "new App epoch adoption retains original predecessor spring motion through the adjust barrier");
        Check(controller.pending->corner.value==corner.value && controller.pending->corner.velocity==corner.velocity &&
              controller.pending->generation==generation+1,"later App worker does not retarget or reset corner motion a second time");
        ASurfaceTransaction_apply(app_transaction);ASurfaceTransaction_delete(app_transaction);
        Commit(app_commit);ResetEvents();Frame(NowNs(),&controller);
        Check(events.empty(),"later App commit cannot overtake the earlier icon adjust publication");
        Commit(adjust_commit);
        if(pending_source) {
            ResetEvents();Frame(NowNs(),&controller);
            Check(events.empty() && !controller.active,"new epoch still waits for pending predecessor publications");
            Commit(source_icon_commit);Commit(source_icon_commit-1);
        }
        Pump(controller);
        Check(controller.active && controller.active->request_ns==next_request && controller.active->closing!=was_closing &&
              controller.active->generation==generation+1 && !controller.pending,
              "all predecessor and adjust commits adopt a single continuously retargeted App owner");
        Check(RequestRunning(source_request) && RequestRunning(next_request),
              "same-layer App adoption preserves inherited cleanup epochs after all commits");
        ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(2031,2032);
        ResetEvents();Stop(controller);
        Check(RequestRunning(source_request) && RequestRunning(next_request),
              "inherited cleanup epochs wait for the final shared detach completion");
        CheckLinkedRetirement(2031,2032);
        Check(!RequestRunning(source_request) && !RequestRunning(next_request),
              "final completion releases both current and inherited cleanup epochs");
    }
    std::puts("PASS exact and production parent-proved icon reversal precede App dispatch for pending and committed owners");
}

void LinkedIconProductionSourceParentProof() {
    auto& controller=Fresh();const auto closing=LinkedGeometry(true);const auto opening=LinkedGeometry(false);
    const auto old_request=NowNs()-80000000LL;
    Check(QueueSession(2051,2050,1120,2436,closing,true,old_request,{},false),
          "seed old native App owner while the bridge is about to publish a new request");
    Pump(controller);Commit(PrimeTestIcon(2052,2053,closing,true,old_request));
    const auto next_request=NowNs();const auto old_state=controller.active->animation.Scalars();
    auto* wrong=ASurfaceTransaction_create();Check(wrong,"create wrong-parent production icon adjust");
    ResetEvents();const auto callbacks=commits.size();
    Check(!PrimeLinkedIcon(wrong,2052,2053,171,171,1120,2436,opening,false,
                           next_request,next_request,2059),
          "production source event equal to the new request cannot borrow an old icon from a different App parent");
    Check(events.empty() && commits.size()==callbacks && !controller.pending && controller.active->request_ns==old_request &&
          controller.active->closing,"wrong parent cannot replace, write, or retarget the existing App and icon owner");
    for(size_t i=0;i<old_state.size();++i)
        Check(controller.active->animation.Scalars()[i].value==old_state[i].value &&
              controller.active->animation.Scalars()[i].velocity==old_state[i].velocity,
              "wrong-parent production proof preserves all old native spring state");
    ASurfaceTransaction_delete(wrong);
    auto* valid=ASurfaceTransaction_create();Check(valid,"create correct-parent production icon adjust");
    Check(PrimeLinkedIcon(valid,2052,2053,171,171,1120,2436,opening,false,
                          next_request,next_request,2050),
          "the same missed-worker request succeeds with the actual App parent after wrong-parent rejection");
    Check(controller.active->request_ns==next_request && !controller.active->closing,
          "correct production parent proof advances the old native owner instead of suppressing its icon");
    ASurfaceTransaction_apply(valid);ASurfaceTransaction_delete(valid);Commit(callbacks);
    ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(2051,2052);
    ResetEvents();Stop(controller);CheckLinkedRetirement(2051,2052);

    auto& first=Fresh();const auto initial_request=NowNs();
    auto* initial=ASurfaceTransaction_create();Check(initial,"create production icon-first transaction without a native predecessor");
    Check(PrimeLinkedIcon(initial,2062,2063,171,171,1120,2436,closing,true,
                          initial_request,initial_request,2060),
          "production source event equal to the request still allows a legitimate icon-first seed when no App owner exists");
    Check(first.pending && first.pending->layer==2062 && first.pending->request_ns==initial_request &&
          first.pending->starting && !first.active,"production icon-first request creates one provisional pending owner");
    ASurfaceTransaction_apply(initial);ASurfaceTransaction_delete(initial);
    auto* app=ASurfaceTransaction_create();Check(app,"create first real App publication after production icon-first seed");
    Check(QueueSession(2061,2060,1120,2436,closing,true,initial_request,{},false,0,0,app),
          "first real App joins the production icon-first source while its original publication is uncommitted");
    ASurfaceTransaction_apply(app);ASurfaceTransaction_delete(app);
    Commit(1);Pump(first);
    Check(!first.active && first.pending,"App publication still waits for the production icon-first transaction");
    Commit(0);Pump(first);
    Check(first.active && first.active->layer==2061 && Owns(*first.active,2062),
          "production icon-first source hands off to its actual App without losing the icon");
    ResetEvents();Frame(first.active->last_ns+16000000LL,&first);CheckSharedIconFrame(2061,2062);
    ResetEvents();Stop(first);CheckLinkedRetirement(2061,2062);
    std::puts("PASS production parent proof rejects another App and preserves legitimate icon-first publication");
}

void LinkedIconRejectsStaleAdjustSource() {
    auto& controller=Fresh();const auto closing=LinkedGeometry(true);const auto opening=LinkedGeometry(false);
    const auto source_request=NowNs()-80000000LL;
    Check(QueueSession(2041,2040,1120,2436,closing,true,source_request,{},false),
          "seed App with its exact source epoch before testing rejected icon adjusts");
    Pump(controller);Commit(PrimeTestIcon(2042,2043,closing,true,source_request));
    const auto next_request=NowNs();
    auto reject=[&](int icon,int64_t source_epoch,const char* reason) {
        auto* transaction=ASurfaceTransaction_create();Check(transaction,"create rejected source-based adjust transaction");
        ResetEvents();const auto callbacks=commits.size();
        Check(!PrimeLinkedIcon(transaction,icon,2043,171,171,1120,2436,opening,false,next_request,source_epoch),reason);
        Check(events.empty() && commits.size()==callbacks && !controller.pending,
              "invalid source adjust writes no shader, creates no publication gate, and seeds no provisional icon owner");
        ASurfaceTransaction_delete(transaction);
    };
    reject(2042,source_request-1,"stale unmatched source timestamp cannot borrow another App epoch");
    reject(2044,source_request,"exact source timestamp cannot authorize an icon capability it does not already own");
    Check(controller.active->request_ns==source_request && controller.active->closing,
          "invalid predecessor proofs leave live App direction and identity unchanged");
    genie::CancelIconRenderer();
    reject(2042,source_request,"a queued global cancellation rejects source-based adjust before worker processing");
    Pump(controller);DrainCompletions();
    reject(2042,source_request,"a retired source cannot be revived by presenting a newer adjust epoch");
    Check(!controller.active && !Running(),"rejected cancelled source leaves the controller idle");
    for(const auto& entry:references) Check(entry.second==0,"rejected source-based adjusts retain no surface references");

    auto& pending=Fresh();const auto pending_request=NowNs()-30000000LL;
    Check(QueueSession(2045,2040,1120,2436,closing,true,pending_request,{},false),
          "seed App before cancelling an outstanding icon publication");
    Pump(pending);const auto icon_commit=PrimeTestIcon(2046,2043,closing,true,pending_request);
    Stop(pending);Check(pending.retiring.size()==1,"cancelled uncommitted source remains in retirement only");
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create adjust against cancelled uncommitted source");
    ResetEvents();const auto callbacks=commits.size();
    Check(!PrimeLinkedIcon(transaction,2046,2043,171,171,1120,2436,opening,false,NowNs(),pending_request),
          "a cancelled source waiting for commit cannot act as a live predecessor");
    Check(events.empty() && commits.size()==callbacks && !pending.pending,
          "cancelled pending source creates neither a new shader write nor a provisional owner");
    ASurfaceTransaction_delete(transaction);Commit(icon_commit);Pump(pending);DrainCompletions();
    Check(!Running(),"cancelled pending source finishes normal cleanup after rejected adjust");
    for(const auto& entry:references) Check(entry.second==0,"cancelled pending source cleanup releases every capability");
    auto& expired=Fresh();const auto expired_request=NowNs()-100000000LL;
    Check(QueueSession(2047,2040,1120,2436,opening,false,expired_request,{},true),
          "seed exact icon predecessor before its cancelled pause expires");
    Pump(expired);Commit(PrimeTestIcon(2048,2043,opening,false,expired_request));
    Java_ng_lst_genie_ContentAnimator_nativeStopRequest(nullptr,nullptr,2040,expired_request);
    expired.active->cancel_ns=NowNs()-1;expired.active->started_ns=NowNs()-1500000001LL;
    transaction=ASurfaceTransaction_create();Check(transaction,"create icon adjust after original pause deadline");
    ResetEvents();const auto expired_callbacks=commits.size();
    Check(!PrimeLinkedIcon(transaction,2048,2043,171,171,1120,2436,closing,true,NowNs(),expired_request,2040),
          "even an exact owned icon cannot revive a paused predecessor past its original deadline");
    Check(events.empty() && commits.size()==expired_callbacks && !expired.pending,
          "expired paused source creates no fresh provisional icon owner or surface writes");
    ASurfaceTransaction_delete(transaction);Stop(expired);
    std::puts("PASS stale, unowned, cancelled and retired icon adjust sources are rejected");
}

void LinkedIconReversalEpochIsolation() {
    auto& controller=Fresh();const auto closing=LinkedGeometry(true);
    const auto old_request=NowNs()-100000000LL;
    Check(QueueSession(1951,1950,1120,2436,closing,true,old_request,{},false),
          "seed closing App for linked icon reversal");
    Pump(controller);Commit(PrimeTestIcon(1952,1953,closing,true,old_request));
    controller.active->animation.Advance(.09);
    genie::model::AdvanceScalar(&controller.active->corner,.09,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto scalars=controller.active->animation.Scalars();
    const auto corner=controller.active->corner;
    const auto opening=LinkedGeometry(false);
    const auto new_request=NowNs();
    const auto retired_icon=layer_uniforms.at(1952);
    ResetEvents();
    Check(QueueSession(1951,1950,1120,2436,opening,false,new_request,{},false),
          "rapid reversal publishes a new immutable App epoch");
    Pump(controller);
    Check(!Owns(*controller.active,1952) && Cleared().empty() &&
          layer_uniforms.at(1952)==retired_icon && border_widths[1952]>0,
          "rapid reversal freezes the old icon until its exact capability is registered for the new request");
    Check(!controller.active->closing && controller.active->request_ns==new_request &&
          controller.active->corner.value==corner.value && controller.active->corner.velocity==corner.velocity,
          "reversal changes direction and ownership while preserving live corner motion");
    for(size_t i=0;i<scalars.size();++i)
        Check(controller.active->animation.Scalars()[i].value==scalars[i].value &&
              controller.active->animation.Scalars()[i].velocity==scalars[i].velocity,
              "rapid reversal retains every shared spring position and velocity");
    Commit(PrimeTestIcon(1952,1953,opening,false,new_request));
    auto* late=ASurfaceTransaction_create();Check(late,"create stale icon callback transaction");
    ResetEvents();const auto callbacks=commits.size();
    Check(!PrimeLinkedIcon(late,1952,1953,171,171,1120,2436,closing,true,old_request),
          "late old epoch cannot bind the icon capability now owned by the reversed App");
    Check(events.empty() && commits.size()==callbacks && !controller.pending &&
          controller.active->request_ns==new_request && !controller.active->closing,
          "rejected old icon callback cannot write a shader, create a session, or block new motion");
    ASurfaceTransaction_delete(late);
    ResetEvents();Frame(controller.active->last_ns+16000000LL,&controller);CheckSharedIconFrame(1951,1952);
    ResetEvents();Stop(controller);CheckLinkedRetirement(1951,1952);

    auto& retagged=Fresh();const auto first_request=NowNs()-100000000LL;
    Check(QueueSession(2201,2200,1120,2436,closing,true,first_request,{2202,2203},false),
          "seed closing owner with previews before a same-icon reversal");
    Pump(retagged);Commit(PrimeTestIcon(2204,2214,closing,true,first_request));
    Commit(PrimeTestIcon(2205,2215,closing,true,first_request));
    retagged.active->animation.Advance(.08);retagged.active->last_ns=NowNs()+10000000LL;
    const auto positions=retagged.active->animation.Scalars();
    const auto old_preview=layer_uniforms.at(2202),old_icon=layer_uniforms.at(2205);
    const auto retag_request=NowNs();
    auto* retag=ASurfaceTransaction_create();Check(retag,"create same-icon retag publication");
    const auto retag_commit=commits.size();ResetEvents();
    Check(PrimeLinkedIcon(retag,2204,2214,171,171,1120,2436,opening,false,
                          retag_request,first_request,2200),
          "a proven same-icon reversal advances the owner before App discovery");
    Check(retagged.active->companions.size()==1 && Owns(*retagged.active,2204) &&
          !Owns(*retagged.active,2202) && !Owns(*retagged.active,2203) && !Owns(*retagged.active,2205),
          "same-icon retag renews only the explicitly registered icon capability");
    Check(Cleared().empty() && layer_uniforms.at(2202)==old_preview && layer_uniforms.at(2205)==old_icon,
          "unregistered old preview and icon keep their last masks without following the reversal");
    for(size_t i=0;i<positions.size();++i)
        Check(retagged.active->animation.Scalars()[i].value==positions[i].value &&
              retagged.active->animation.Scalars()[i].velocity==positions[i].velocity,
              "registered same-icon reversal preserves every position and velocity");
    ASurfaceTransaction_apply(retag);ASurfaceTransaction_delete(retag);Commit(retag_commit);
    Check(QueueSession(2201,2200,1120,2436,opening,false,retag_request,{2206},false),
          "later same-epoch App discovery contributes only its current preview");
    Pump(retagged);
    Check(retagged.active->companions.size()==2 && Owns(*retagged.active,2204) && Owns(*retagged.active,2206),
          "App discovery cannot resurrect companions removed before the icon retag");
    for(int cycle=0;cycle<12;++cycle) {
        const auto epoch=NowNs();const bool closes=cycle%2==0;
        const auto geometry=LinkedGeometry(closes);
        const int preview=2220+cycle*2,icon=preview+1;
        Check(QueueSession(2201,2200,1120,2436,geometry,closes,epoch,{preview},false),
              "a fresh epoch explicitly registers its current preview on a reused primary");
        Pump(retagged);Commit(PrimeTestIcon(icon,2300+cycle,geometry,closes,epoch));
        Check(retagged.active->companions.size()==2 && Owns(*retagged.active,preview) && Owns(*retagged.active,icon),
              "repeated fresh requests retain exactly their registered preview and icon without historical growth");
    }
    ResetEvents();Frame(retagged.active->last_ns+16000000LL,&retagged);
    Check(events.size()==3,"only the current primary, preview, and icon receive per-frame writes after repeated requests");
    Stop(retagged);DrainCompletions();
    for(const auto& entry:references) Check(entry.second==0,"epoch changes release old companion capability references");
    std::puts("PASS linked icon reversal continuity and stale epoch isolation");
}

} // namespace

int main() {
    alarm(25); // Bound failures in the real mutex/callback paths as well as fake barriers.
    g_post=Post;g_border=Border;g_layer_id=Layer;g_inc_strong=Inc;g_dec_strong=Dec;g_on_commit=OnCommit;
    g_on_complete=OnComplete;
    Check(std::isfinite(kClientBorderWidth) && kClientBorderWidth>0 && kClientBorderWidth<0.001f,
          "CLIENT guard stays finite, strictly positive, and below one thousandth of a pixel");
    FramePrimeOrdering();ReplacementPreservesSharedLayers();ScopedCancellation();PendingRetirement();
    IndependentDetachCompletions();JniStopRequestValidation();OwnershipRevocation();OpeningCancelKeepsPreviewMasks();
    RevocationCommitOrdering();
    PrimedReversalContinuity();CornerEndpointRadii();
    CornerRetargetAndHandoff();OpeningPreviewLifetime();
    OverlappingRequests();IndependentCommitGates();CrossParentPreviewHandoff();
    AtomicCompanionAttachment();StopDuringCompanionPublication();RepeatedPrimeCommitGates();
    PrimedCrossParentHandoff();DelayedPendingPreviewCoalescence();
    PublicationUnionAfterLateAttachment();OneShotOpeningPublication();ClosingGroupHostPublication();
    GestureClosingSessionPublication();GestureClosingHostPublication();GesturePublicationRejectsIncompatibleGeometry();
    GroupedSourcePublication();RevokedGroupKeepsMask();GroupReplacementDoesNotBecomePreview();
    LinkedIconVisibleRetirement();LateLinkedIconSharesFrames();LinkedIconBeforeAppHandoff();LinkedIconPublicationCancellation();
    LinkedIconSeedLosesToAppPublication();LinkedIconCannotCrossAppOwnership();LinkedIconComposedPreviewReversal();
    ComposedCloseInheritsEstablishedDonor();
    ComposedCloseKeepsPresentedContour();
    AdjacentSameParentStreamsDoNotInherit();
    OppositeDirectionWithoutSharedProofDoesNotInherit();
    LinkedIconComposedPreviewProofIsolation();
    LinkedIconPendingReversalPublication();LinkedIconSelectsPendingPublication();
    LinkedIconWholeOwnerPublication();LinkedIconAppCommitOrders();WorkerCoalescencePreservesPrimedSnapshot();
    LinkedIconCannotReviveCancelledRequest();LinkedIconReversalEpochIsolation();
    LinkedIconAdjustPrecedesAppDispatch();LinkedIconProductionSourceParentProof();
    LinkedIconRejectsStaleAdjustSource();DrainCompletions();
    {std::lock_guard<std::mutex> lock(test_mutex);
     for(const auto& entry:border_widths) Check(entry.second==0.0f,"all CLIENT guards cleared after retirement");
     for(const auto& entry:references) Check(entry.second==0,"all fake capabilities released");
     for(const auto& commit:commits) Check(!commit.callback,"all fake commit callbacks consumed");
     for(const auto& completion:completions) Check(!completion.callback,"all fake completions consumed");}
    std::printf("Renderer device regression: %d checks passed (actual genie_content.cpp)\n",checks);
    return 0;
}
