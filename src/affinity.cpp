// Undoes the engine's thread pinning.
//
// The engine pins its threads to one logical CPU each: ServerWorker,
// GameThread, WorkerThreadSch and the WT/High pool all get a single cpu. That
// core then becomes the frame gate -- a thread that needs cpu waits for its
// one assigned core while fifteen others idle. The same game under Proton runs
// every thread on 0-15, because Wine does not apply the requests, and that
// build never had the problem.
//
// Measured over three runs each of the startup window, which was the worst of
// it: p95 frametime 75.6ms -> 59.4ms, p99 133ms -> 77.8ms, frames over 100ms
// 9 -> 3.7, time lost above 50ms 4598ms -> 3333ms. The number of hitches
// barely moves (173 -> 175); each one gets much shorter. Pinning was not
// creating hitches, it was making every hitch longer.
//
// It does not fix the cutscene stutter, which is a separate problem.
//
// The interesting part is which call to hook. An earlier attempt at this
// hooked sched_setaffinity and pthread_setaffinity_np, measured as doing
// nothing, and was reverted -- but the engine sets its masks through
// pthread_attr_setaffinity_np before pthread_create, so every thread that
// mattered stayed pinned while the hook reported success. Both pthread calls
// are interposed here; the attribute one is the one that does the work.
//
// sched_setaffinity is left alone: only Wwise's threads call it, and widening
// them crashed a Steam Deck in JobManager_dispatchMultiple at startup.
//
// GameThread's core is the exception: the engine pins GameThread, ClientInit
// and LoadPsoCache to it together, and those keep their pins.
//
// BG3LE_AFFINITY=off disables this.

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "log.h"

namespace {

using bg3le::logf;

bool enabled() {
    static const bool on = [] {
        const char* opt = std::getenv("BG3LE_AFFINITY");
        return opt == nullptr || std::strcmp(opt, "off") != 0;
    }();
    return on;
}

int cpu_count() {
    static const int n = [] {
        const long c = ::sysconf(_SC_NPROCESSORS_CONF);
        return (c > 0 && c <= CPU_SETSIZE) ? (int)c : CPU_SETSIZE;
    }();
    return n;
}

// True if the mask already allows every cpu, in which case there is nothing
// to widen and nothing worth logging.
bool unrestricted(const cpu_set_t* mask, std::size_t size) {
    for (int c = 0; c < cpu_count(); ++c) {
        if (!CPU_ISSET_S((std::size_t)c, size, mask)) return false;
    }
    return true;
}

void fill(cpu_set_t* out, std::size_t size) {
    CPU_ZERO_S(size, out);
    for (int c = 0; c < cpu_count(); ++c) CPU_SET_S((std::size_t)c, size, out);
}

// "3" or "0,4": the cpus a mask asks for.
void cpu_list(const cpu_set_t* mask, std::size_t size, char* out, std::size_t cap) {
    std::size_t used = 0;
    out[0] = 0;
    for (int c = 0; c < cpu_count() && used + 8 < cap; ++c) {
        if (!CPU_ISSET_S((std::size_t)c, size, mask)) continue;
        used += (std::size_t)std::snprintf(out + used, cap - used, used ? ",%d" : "%d", c);
    }
}

// Pinned requests by attribute and then by thread, so the thread's name can
// be logged when the engine gives it one.
struct Pin {
    const void* Key;
    char Cpus[64];
    int Single;  // the one cpu asked for, or -1
};
std::mutex g_pins_lock;
Pin g_attr_pins[64];
Pin g_thread_pins[256];
unsigned g_next_attr = 0, g_next_thread = 0;

// GameThread's cpu once it is named; requests for it alone keep their pin.
std::atomic<int> g_game_cpu{-1};

int single_cpu(const cpu_set_t* mask, std::size_t size) {
    if (CPU_COUNT_S(size, mask) != 1) return -1;
    for (int c = 0; c < cpu_count(); ++c) {
        if (CPU_ISSET_S((std::size_t)c, size, mask)) return c;
    }
    return -1;
}

void remember(Pin* pins, unsigned count, unsigned* next, const Pin& pin) {
    std::lock_guard<std::mutex> held(g_pins_lock);
    Pin* slot = nullptr;
    for (unsigned i = 0; i < count && slot == nullptr; ++i) {
        if (pins[i].Key == pin.Key) slot = &pins[i];
    }
    if (slot == nullptr) slot = &pins[(*next)++ % count];
    *slot = pin;
}

bool recall(Pin* pins, unsigned count, const void* key, Pin* out) {
    std::lock_guard<std::mutex> held(g_pins_lock);
    for (unsigned i = 0; i < count; ++i) {
        if (pins[i].Key == key) {
            *out = pins[i];
            return true;
        }
    }
    return false;
}

// Whether to replace the requested mask, and what with. Requests are logged
// with BG3LE_AFFINITY=off too, to compare runs.
bool widen(const cpu_set_t* mask, std::size_t size, cpu_set_t* out,
           const char* api, const char* target, const void* key) {
    if (mask == nullptr) return false;
    if (unrestricted(mask, size)) return false;

    Pin pin{key, {}, single_cpu(mask, size)};
    cpu_list(mask, size, pin.Cpus, sizeof(pin.Cpus));
    const char* cpus = pin.Cpus;
    if (key != nullptr) remember(g_attr_pins, 64, &g_next_attr, pin);
    if (!enabled()) {
        logf("affinity: %s asked for cpus %s%s, kept (from %s)", api, cpus, target, bg3le::thread_tag());
        return false;
    }
    if (pin.Single >= 0 && pin.Single == g_game_cpu.load()) {
        logf("affinity: %s asked for cpus %s%s, kept: GameThread's core (from %s)", api, cpus, target,
             bg3le::thread_tag());
        return false;
    }
    fill(out, size);
    logf("affinity: %s asked for cpus %s%s, gave it all %d (from %s)", api, cpus, target,
         cpu_count(), bg3le::thread_tag());
    return true;
}

using ThreadFn = int (*)(pthread_t, size_t, const cpu_set_t*);
using AttrFn = int (*)(pthread_attr_t*, size_t, const cpu_set_t*);
using CreateFn = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
using NameFn = int (*)(pthread_t, const char*);

template <typename Fn>
Fn next(const char* name) {
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

}  // namespace

// The engine calls these directly and we are earlier in the search order, so
// defining them interposes; dlsym(RTLD_NEXT) reaches the real ones.

// This is the one the engine actually uses: a mask in the thread attributes,
// applied by pthread_create. Hooking only the two below is what made the
// first attempt at this look like a no-op.
extern "C" int pthread_attr_setaffinity_np(pthread_attr_t* attr,
                                           size_t cpusetsize,
                                           const cpu_set_t* mask) {
    static const auto real = next<AttrFn>("pthread_attr_setaffinity_np");
    if (real == nullptr) return -1;

    cpu_set_t all;
    if (widen(mask, cpusetsize, &all, "pthread_attr_setaffinity_np", "", attr)) {
        return real(attr, cpusetsize, &all);
    }
    return real(attr, cpusetsize, mask);
}

extern "C" int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
                                      const cpu_set_t* mask) {
    static const auto real = next<ThreadFn>("pthread_setaffinity_np");
    if (real == nullptr) return -1;

    char name[16] = {};
    ::pthread_getname_np(thread, name, sizeof(name));
    char target[32];
    std::snprintf(target, sizeof(target), " for %s", name);

    cpu_set_t all;
    if (widen(mask, cpusetsize, &all, "pthread_setaffinity_np", target, nullptr)) {
        return real(thread, cpusetsize, &all);
    }
    return real(thread, cpusetsize, mask);
}

// Only to name the threads above: a thread made with a pinned attribute is
// remembered, and logged when it is named.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*start)(void*), void* arg) {
    static const auto real = next<CreateFn>("pthread_create");
    if (real == nullptr) return EAGAIN;

    const int result = real(thread, attr, start, arg);
    Pin pin;
    if (result == 0 && attr != nullptr && recall(g_attr_pins, 64, attr, &pin)) {
        pin.Key = (const void*)*thread;
        remember(g_thread_pins, 256, &g_next_thread, pin);
    }
    return result;
}

// Also where GameThread's core is learned, and GameThread pinned back to it.
extern "C" int pthread_setname_np(pthread_t thread, const char* name) {
    static const auto real = next<NameFn>("pthread_setname_np");
    static const auto set_affinity = next<ThreadFn>("pthread_setaffinity_np");
    if (real == nullptr) return ENOSYS;

    const int result = real(thread, name);
    Pin pin;
    if (!recall(g_thread_pins, 256, (const void*)thread, &pin)) return result;
    logf("affinity: thread %s was asked for cpus %s", name, pin.Cpus);
    if (enabled() && pin.Single >= 0 && std::strcmp(name, "GameThread") == 0 && set_affinity != nullptr) {
        g_game_cpu = pin.Single;
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET((std::size_t)pin.Single, &one);
        const int rc = set_affinity(thread, sizeof(one), &one);
        logf("affinity: pinned GameThread back to cpu %d (%d); threads asking for it alone keep it", pin.Single, rc);
    }
    return result;
}
