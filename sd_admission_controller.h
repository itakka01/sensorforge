#pragma once
// Experimental, platform-independent admission primitive.
// NOT wired to the production SD gates yet: migration requires all clients.
#include <atomic>
#include <cstdint>

class SdAdmissionController {
public:
    enum Owner : uint8_t { None=0, ClusterWipe=1, SecureErase=2, Format=3,
        SpiRemount=4, Benchmark=5, AudioDiagnostic=6, MediaMutation=7, RecorderStart=8 };
    struct Lease { uint32_t ticket=0; explicit operator bool() const { return ticket!=0; } };

    // Only one owner may reserve the destructive-operation lane at a time.
    Lease tryReserve(Owner owner) {
        if (owner==None) return {};
        const uint32_t generation = counter_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t ticket = ((generation & 0x00ffffffu) << 8) | uint32_t(owner);
        uint32_t expected=0;
        if (!active_.compare_exchange_strong(expected, ticket,
              std::memory_order_acq_rel, std::memory_order_acquire)) return {};
        return {ticket};
    }
    bool owns(Lease lease) const {
        return lease && active_.load(std::memory_order_acquire)==lease.ticket;
    }
    bool release(Lease lease) {
        if (!lease) return false;
        uint32_t expected=lease.ticket;
        return active_.compare_exchange_strong(expected, 0,
              std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool busy() const { return active_.load(std::memory_order_acquire)!=0; }
    Owner owner() const {
        return Owner(active_.load(std::memory_order_acquire) & 0xffu);
    }
private:
    std::atomic<uint32_t> active_{0};
    std::atomic<uint32_t> counter_{1};
};
