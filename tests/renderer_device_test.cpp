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
struct Event { int layer; bool clear; };
struct CallbackCall { void* context; ASurfaceTransaction_OnComplete callback; };
std::mutex test_mutex;
std::condition_variable test_cv;
std::vector<Event> events;
std::vector<CallbackCall> commits;
std::vector<CallbackCall> completions;
std::map<std::pair<const void*, const void*>, int> references;
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
void* Post(void* transaction,const void* surface,const void* token,const void*,int32_t) {
    std::unique_lock<std::mutex> lock(test_mutex);
    const bool clear=*static_cast<void* const*>(token)==nullptr;
    events.push_back({Id(*static_cast<void* const*>(surface)),clear});
    if(pause_post && !clear) {
        pause_post=false;post_paused=true;test_cv.notify_all();
        Wait(lock,release_post,"paused Frame released within timeout");
    }
    return transaction; // Neither fake capability reaches an Android SDK function.
}
void* Rotate(void* transaction,const void*,float,float,float,float,float,
             float,float,float,float,float) {return transaction;}
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
} // namespace

int main() {
    alarm(25); // Bound failures in the real mutex/callback paths as well as fake barriers.
    g_post=Post;g_rotate=Rotate;g_layer_id=Layer;g_inc_strong=Inc;g_dec_strong=Dec;g_on_commit=OnCommit;
    g_on_complete=OnComplete;
    FramePrimeOrdering();ReplacementPreservesSharedLayers();ScopedCancellation();PendingRetirement();
    IndependentDetachCompletions();DrainCompletions();
    {std::lock_guard<std::mutex> lock(test_mutex);
     for(const auto& entry:references) Check(entry.second==0,"all fake capabilities released");
     for(const auto& commit:commits) Check(!commit.callback,"all fake commit callbacks consumed");
     for(const auto& completion:completions) Check(!completion.callback,"all fake completions consumed");}
    std::printf("Renderer device regression: %d checks passed (actual hha_content.cpp)\n",checks);
    return 0;
}
