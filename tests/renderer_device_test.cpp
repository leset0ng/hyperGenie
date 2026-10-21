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
#include "../app/src/main/cpp/hha_content.cpp"
#undef ASurfaceTransaction_apply
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <signal.h>

namespace {
using namespace std::chrono_literals;
struct Event { int layer; bool clear; void* transaction; };
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
    std::lock_guard<std::mutex> lock(test_mutex);
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
    events.push_back({layer,clear,transaction});
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
    Java_ng_lst_hha_ContentAnimator_nativeStop(nullptr,nullptr);Pump(controller);
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
    Check(Cleared()==std::vector<int>{203},"only lost layer cleared, shared primary and companion kept");
    Check(border_widths[201]>0 && border_widths[202]>0 && border_widths[203]==0 && border_widths[204]>0,
          "replacement preserves shared CLIENT guards and retires only the lost companion");
    Stop(controller);std::puts("PASS extent replacement retains shared layers");
}
void ScopedCancellation() {
    auto& controller=Fresh();Check(Queue(301,3001),"seed current B");Pump(controller);
    hha::RenderIdentity b{controller.active->parent_layer,controller.active->request_ns};
    hha::CancelIconRenderer({3000,b.request_ns-1});
    hha::CancelIconRenderer({b.parent,b.request_ns-1});
    std::this_thread::sleep_for(60ms);Frame(NowNs(),&controller);
    Check(controller.active && controller.active->layer==301,"old parent and epoch cannot cancel B");
    const int64_t requested=NowNs();
    hha::CancelIconRenderer(b);
    {std::lock_guard<std::mutex> lock(controller.mutex);
     Check(controller.active && controller.active->cancel_ns>=requested+47000000LL,
           "matching B records a scoped cancellation without changing its watchdog");}
    std::this_thread::sleep_for(60ms);ResetEvents();Frame(NowNs(),&controller);
    Check(controller.active && Cleared().empty() && border_widths[301]>0,
          "closing B remains protected after stop because SF stop does not hide it");
    controller.active->started_ns=NowNs()-2000000000LL;
    Frame(NowNs(),&controller);
    Check(!controller.active && Cleared()==std::vector<int>{301},
          "stopped closing B retires at its existing deadline");
    // Pending replacement also wins over an old active stop.
    Check(Queue(302,3002),"seed another active session");Pump(controller);
    hha::RenderIdentity old{controller.active->parent_layer,controller.active->request_ns};
    auto next=Prime(303,3003);hha::CancelIconRenderer(old);Commit(next);Pump(controller);
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
bool Running() {return Java_ng_lst_hha_ContentAnimator_nativeRunning(nullptr,nullptr)==JNI_TRUE;}
void IndependentDetachCompletions() {
    for(size_t first:{size_t{0},size_t{1}}) {
        auto& controller=Fresh();auto a=Prime(501,5001);auto b=Prime(502,5002);
        Stop(controller);Commit(a);Commit(b);Pump(controller);
        Check(!controller.active && !controller.pending && controller.retiring.empty(),
              "stopped primes leave no active, pending, or retiring session");
        Check(Cleared()==std::vector<int>({501,502}) && completions.size()==2 &&
              controller.detaches_pending.load()==2,"two primes submit independent detach transactions");
        Check(Running(),"renderer busy before either detach completes");
        Invoke(completions,first);
        Check(controller.cleared_serial.load()==controller.submitted_serial.load(),
              "first callback advances serial watermark over both retirements");
        Check(controller.detaches_pending.load()==1 && Running(),
              "first detach completion cannot release snapshot guard while second remains");
        Invoke(completions,1-first);
        Check(controller.detaches_pending.load()==0 && !Running(),
              "renderer idle only after both independent detaches complete");
    }
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
    Check(Cleared()==std::vector<int>{812} && isolated.active->companions.empty(),
          "preview retention does not leak across request identity");
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
        hha::genie::AdvanceScalar(&current.corner,5,1.0);
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
    hha::genie::AdvanceScalar(&current.corner,.12,1.0);
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
    Check(controller.active->corner.response==hha::genie::kOpeningResponse,
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
    std::puts("PASS corner reversal and starting/content handoff continuity");
}
void PrimedReversalContinuity() {
    auto& controller=Fresh();Check(Queue(601,6001),"seed closing icon state");Pump(controller);
    auto& current=*controller.active;
    current.animation.Advance(.18);
    hha::genie::AdvanceScalar(&current.corner,.18,1.0);
    // Model a vsync timestamp slightly ahead of the submission clock, so a
    // retarget must preserve the already-published state without extra advance.
    current.last_ns=NowNs()+10000000LL;
    const auto mesh=hha::genie::ContourMesh(current.animation,current.parameters);
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
    a->animation.Advance(.12);hha::genie::AdvanceScalar(&a->corner,.12,1.0);
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
    Frame(NowNs(),&controller);
    Check(a->frames>a_frames && controller.active->frames>b_frames,
          "both independent sessions receive display-event updates");
    Java_ng_lst_hha_ContentAnimator_nativeStopRequest(nullptr,nullptr,9001,a_request);
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
    hha::genie::AdvanceScalar(&controller.active->corner,.14,1.0);
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
    Check(Java_ng_lst_hha_ContentAnimator_nativeAttach(nullptr,nullptr,
              reinterpret_cast<jlong>(transaction),932,9301,1120,2436,request)==JNI_TRUE,
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
    hha::genie::AdvanceScalar(&controller.active->corner,.1,1.0);
    controller.active->last_ns=NowNs()+10000000LL;
    const auto mesh=hha::genie::ContourMesh(controller.active->animation,controller.active->parameters);
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
    hha::Transition request;
    request.geometry=Geometry(1120,2436);
    for(size_t i=0;i<4;++i) std::swap(request.geometry[i],request.geometry[i+4]);
    std::swap(request.geometry[10],request.geometry[11]);
    request.event_ns=NowNs()-30000000LL;
    request.leash_layer=9950;
    auto* transaction=ASurfaceTransaction_create();Check(transaction,"create first preview transaction");
    const auto applications=renderer_applies.load();
    Check(hha::PrimeOpeningPreview(transaction,reinterpret_cast<void*>(995),
        reinterpret_cast<void*>(9950),1120,2436,request),"verified preview receives one-shot opening shader");
    Check(events.size()==1 && events[0].layer==995 && !events[0].clear && events[0].transaction==transaction,
          "preview shader is already in the actual first-show transaction before its caller applies");
    Check(renderer_applies.load()==applications,"one-shot priming never applies a separate transaction");
    Check(!controller.active && !controller.pending && controller.queued.empty() &&
          controller.overlapping.empty() && commits.empty() && completions.empty(),
          "one-shot publication creates no launcher frame writer or retirement callbacks");
    Check(controller.submitted_serial.load()==0 && !Java_ng_lst_hha_ContentAnimator_nativeRunning(nullptr,nullptr),
          "server-owned one-shot does not poison launcher retirement bookkeeping");
    Check(layer_uniforms[995][8]-layer_uniforms[995][0]<.99f && layer_uniforms[995][36]==1120,
          "first publication uses elapsed opening deformation and full buffer domain");
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    request.closing=true;
    transaction=ASurfaceTransaction_create();
    Check(!hha::PrimeOpeningPreview(transaction,reinterpret_cast<void*>(995),
        reinterpret_cast<void*>(9950),1120,2436,request),"closing request cannot use opening capability priming");
    // The central renderer owns real preview retirement, represented here by
    // its same-transaction SELF/guard clear after the one-shot was delivered.
    ClearSurface(transaction,reinterpret_cast<void*>(995));
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
    std::puts("PASS atomic one-shot preview publication without a second frame writer");
}
} // namespace

int main() {
    alarm(25); // Bound failures in the real mutex/callback paths as well as fake barriers.
    g_post=Post;g_border=Border;g_layer_id=Layer;g_inc_strong=Inc;g_dec_strong=Dec;g_on_commit=OnCommit;
    g_on_complete=OnComplete;
    Check(std::isfinite(kClientBorderWidth) && kClientBorderWidth>0 && kClientBorderWidth<0.001f,
          "CLIENT guard stays finite, strictly positive, and below one thousandth of a pixel");
    FramePrimeOrdering();ReplacementPreservesSharedLayers();ScopedCancellation();PendingRetirement();
    IndependentDetachCompletions();PrimedReversalContinuity();CornerEndpointRadii();
    CornerRetargetAndHandoff();OpeningPreviewLifetime();
    OverlappingRequests();IndependentCommitGates();CrossParentPreviewHandoff();
    AtomicCompanionAttachment();StopDuringCompanionPublication();RepeatedPrimeCommitGates();
    PrimedCrossParentHandoff();DelayedPendingPreviewCoalescence();
    PublicationUnionAfterLateAttachment();OneShotOpeningPublication();DrainCompletions();
    {std::lock_guard<std::mutex> lock(test_mutex);
     for(const auto& entry:border_widths) Check(entry.second==0.0f,"all CLIENT guards cleared after retirement");
     for(const auto& entry:references) Check(entry.second==0,"all fake capabilities released");
     for(const auto& commit:commits) Check(!commit.callback,"all fake commit callbacks consumed");
     for(const auto& completion:completions) Check(!completion.callback,"all fake completions consumed");}
    std::printf("Renderer device regression: %d checks passed (actual hha_content.cpp)\n",checks);
    return 0;
}
