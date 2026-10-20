// SPDX-License-Identifier: Apache-2.0
// Runs the actual renderer with fake capabilities and real, empty SDK transactions.
#include "../app/src/main/cpp/hha_content.cpp"
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
     Check(controller.active && !controller.cancellation.Due(b,requested+47000000LL),
           "matching B retains its 48ms cancellation grace");}
    std::this_thread::sleep_for(60ms);ResetEvents();Frame(NowNs(),&controller);
    Check(!controller.active && Cleared()==std::vector<int>{301},"matching B clears after grace");
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
} // namespace

int main() {
    alarm(25); // Bound failures in the real mutex/callback paths as well as fake barriers.
    g_post=Post;g_border=Border;g_layer_id=Layer;g_inc_strong=Inc;g_dec_strong=Dec;g_on_commit=OnCommit;
    g_on_complete=OnComplete;
    Check(std::isfinite(kClientBorderWidth) && kClientBorderWidth>0 && kClientBorderWidth<0.001f,
          "CLIENT guard stays finite, strictly positive, and below one thousandth of a pixel");
    FramePrimeOrdering();ReplacementPreservesSharedLayers();ScopedCancellation();PendingRetirement();
    IndependentDetachCompletions();PrimedReversalContinuity();CornerEndpointRadii();
    CornerRetargetAndHandoff();OpeningPreviewLifetime();DrainCompletions();
    {std::lock_guard<std::mutex> lock(test_mutex);
     for(const auto& entry:border_widths) Check(entry.second==0.0f,"all CLIENT guards cleared after retirement");
     for(const auto& entry:references) Check(entry.second==0,"all fake capabilities released");
     for(const auto& commit:commits) Check(!commit.callback,"all fake commit callbacks consumed");
     for(const auto& completion:completions) Check(!completion.callback,"all fake completions consumed");}
    std::printf("Renderer device regression: %d checks passed (actual hha_content.cpp)\n",checks);
    return 0;
}
