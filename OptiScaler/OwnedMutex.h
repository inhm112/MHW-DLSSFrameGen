#pragma once

#include "SysUtils.h"

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <thread>

class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 }; // usage tag; don't use 0
    std::atomic<uintptr_t> holderThreadKey { 0 };

    static uintptr_t CurrentThreadKey() { return std::hash<std::thread::id> {}(std::this_thread::get_id()); }

  public:
    void lock(uint32_t _owner)
    {
        mtx.lock();
        owner.store(_owner, std::memory_order_release);
        holderThreadKey.store(CurrentThreadKey(), std::memory_order_release);
    }

    // Only unlocks if owner tag matches and the calling thread recorded the lock.
    void unlockThis(uint32_t _owner)
    {
        uint32_t current_owner = owner.load(std::memory_order_acquire);

        if (current_owner == 0 || current_owner != _owner)
        {
            LOG_WARN("current_owner: {}, _owner: {}", current_owner, _owner);
            return;
        }

        const uintptr_t holder = holderThreadKey.load(std::memory_order_acquire);
        if (holder == 0 || holder != CurrentThreadKey())
        {
            LOG_WARN("unlockThis owner {} called from non-holder thread", _owner);
            return;
        }

        holderThreadKey.store(0, std::memory_order_release);
        owner.store(0, std::memory_order_release);
        mtx.unlock();
    }

    uint32_t getOwner() { return owner.load(std::memory_order_seq_cst); }

    bool currentThreadHoldsLock() const
    {
        const uintptr_t holder = holderThreadKey.load(std::memory_order_acquire);
        return holder != 0 && holder == CurrentThreadKey();
    }
};

class OwnedLockGuard
{
  private:
    OwnedMutex& _mutex;
    uint32_t _owner_id;

  public:
    OwnedLockGuard(OwnedMutex& mutex, uint32_t owner_id) : _mutex(mutex), _owner_id(owner_id)
    {
        _mutex.lock(_owner_id);
    }

    ~OwnedLockGuard() { _mutex.unlockThis(_owner_id); }
};
