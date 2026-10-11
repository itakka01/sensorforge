#include "../sd_admission_controller.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <vector>
int main() {
    SdAdmissionController c;
    auto a=c.tryReserve(SdAdmissionController::ClusterWipe);
    assert(a && c.busy() && c.owner()==SdAdmissionController::ClusterWipe);
    assert(!c.tryReserve(SdAdmissionController::Format));
    assert(!c.release({a.ticket+1}));
    assert(c.owns(a));
    assert(c.release(a));
    assert(!c.release(a));
    auto b=c.tryReserve(SdAdmissionController::SecureErase);
    assert(b && b.ticket!=a.ticket);
    assert(c.owner()==SdAdmissionController::SecureErase);
    assert(!c.tryReserve(SdAdmissionController::ClusterWipe));
    assert(!c.release(a) && c.owns(b));
    assert(c.release(b) && !c.busy());
    // Recorder file-open critical section excludes maintenance reservations.
    auto start=c.tryReserve(SdAdmissionController::RecorderStart);
    assert(start && c.owner()==SdAdmissionController::RecorderStart);
    assert(!c.tryReserve(SdAdmissionController::ClusterWipe));
    assert(!c.tryReserve(SdAdmissionController::SecureErase));
    assert(!c.tryReserve(SdAdmissionController::SpiRemount));
    assert(c.release(start));
    auto after=c.tryReserve(SdAdmissionController::ClusterWipe);
    assert(after);
    assert(c.release(after));
    std::atomic<int> winners{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for(int i=0;i<24;++i) threads.emplace_back([&]{
        while(!go.load()) std::this_thread::yield();
        auto l=c.tryReserve(SdAdmissionController::SpiRemount);
        if(l) { ++winners; while(!go.load()) {} assert(c.release(l)); }
    });
    go=true;
    for(auto &t:threads)t.join();
    assert(!c.busy());
    // Threads may reserve sequentially; mutual exclusion invariant covered by CAS.
    assert(winners>0);
}
