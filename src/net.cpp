// The Script Extender's network protocol, over the game's own connection.
//
// A port of bg3se's (Extender/Shared/ExtenderNet.cpp, Client/ClientNetworking.cpp,
// Server/ServerNetworking.cpp, by Norbyte and the bg3se contributors -- thank
// you), so a bg3le host serves Windows clients running bg3se and the reverse:
//
// - message 400 is the extender's, a protobuf MessageWrapper behind a 32-bit
//   length (Extender/Shared/ExtenderProtocol.proto);
// - a protocol at the head of each peer's protocol list takes it, and on the
//   server watches NETMSG_CLIENT_CONNECT for the " Extender_0" a client adds to
//   its build string;
// - the server answers that with an ExtenderHello, the client replies with its
//   own, and only then does either side send message 400 to the other: a peer
//   that has not registered the ID would crash parsing it.
//
// The engine structures are the Linux build's (see reference/NETWORK.md for
// where each offset was read from); every one is checked before use.

#include "net.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

#include "debug_server.h"
#include "game_state.h"
#include "hook.h"
#include "lauxlib.h"
#include "log.h"
#include "lua.h"
#include "mem.h"
#include "pb.h"
#include "targets.h"
#include "vendor/ls_string.h"

namespace bg3le {

namespace {

// ---- constants ----

constexpr std::uint32_t kExtenderMessage = 400;  // NETMSG_SCRIPT_EXTENDER
constexpr std::uint32_t kClientConnect = 6;      // NETMSG_CLIENT_CONNECT
constexpr std::uint32_t kMaxPayload = 0xfffff;
constexpr std::uint32_t kVersionInitial = 1;
constexpr std::uint32_t kVersionCurrent = 2;  // VerBinSerializer
constexpr std::int32_t kReservedUser = (std::int32_t)0xFFFF0000;
constexpr char kSignature[] = " Extender_0";

// net::AbstractPeer, ecl::GameClient and esv::GameServer, as the Linux build lays them out.
constexpr std::size_t kPeerFactory = 0x240;
constexpr std::size_t kPeerProtocols = 0x330;
constexpr std::size_t kClientHostPeer = 0x428;
constexpr std::size_t kServerActivePeers = 0x6e0;
constexpr std::size_t kServerPeerInfo = 0x720;
constexpr std::size_t kServerLocalPeer = 0x744;
constexpr std::size_t kEoCClientGameClient = 0x90;
constexpr std::size_t kSlotSendSingle = 25;

// net::MessageFactory: the pools array, and the lock GetFreeMessage takes.
constexpr std::size_t kFactoryPools = 0x8;
constexpr std::size_t kFactoryPoolCount = 0x14;
constexpr std::size_t kFactoryLock = 0x20;
constexpr std::size_t kFactoryLockDepth = 0x48;

// eocnet::ClientConnectMessage::Build.
constexpr std::size_t kConnectBuild = 0x38;

// ---- the protocol's messages ----

enum WrapperField : unsigned {
    kPostLua = 1, kResetLua = 2, kHello = 5, kKick = 7, kUserVars = 8,
};

struct Wrapper {
    unsigned kind = 0;
    // MsgPostLuaMessage
    std::string channel, payload, module;
    std::uint32_t requestId = 0, replyId = 0;
    bool binary = false;
    // MsgS2CResetLuaMessage, MsgC2SExtenderHello, MsgS2CKick
    bool bootstrap = false;
    std::uint32_t version = 0;
    std::string kick;
    // MsgUserVars
    std::vector<NetUserVar> vars;
};

void str_field(std::string* out, unsigned field, const std::string& v) {
    if (!v.empty()) pb::bytes_field(out, field, v);
}

// Guid's Val[0]/Val[1], as bg3se's Guid::Parse lays a UUID string out.
bool parse_guid(const std::string& s, std::uint64_t* lo, std::uint64_t* hi) {
    if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    auto byte = [&](std::size_t at, unsigned* out) {
        unsigned v = 0;
        for (std::size_t i = at; i < at + 2; ++i) {
            const char c = s[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        *out = v;
        return true;
    };
    // Bytes in memory order, from Guid::ToString's.
    static const std::size_t at[16] = {6, 4, 2, 0, 11, 9, 16, 14, 21, 19, 26, 24, 30, 28, 34, 32};
    unsigned char b[16];
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        if (!byte(at[i], &v)) return false;
        b[i] = (unsigned char)v;
    }
    std::memcpy(lo, b, 8);
    std::memcpy(hi, b + 8, 8);
    return true;
}

std::string format_guid(std::uint64_t lo, std::uint64_t hi) {
    unsigned char b[16];
    std::memcpy(b, &lo, 8);
    std::memcpy(b + 8, &hi, 8);
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14};
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[b[order[i]] >> 4]);
        out.push_back(hex[b[order[i]] & 15]);
    }
    return out;
}

void encode_var(std::string* out, const NetUserVar& v) {
    std::uint64_t lo = 0, hi = 0;
    parse_guid(v.guid, &lo, &hi);
    pb::uint_field(out, 1, lo);
    pb::uint_field(out, 2, hi);
    str_field(out, 3, v.key);
    // A oneof member is written even when it holds its default.
    switch (v.kind) {
    case NetUserVar::Kind::Int: pb::key(out, 4, 0); pb::varint(out, (std::uint64_t)v.i); break;
    case NetUserVar::Kind::Double: {
        pb::key(out, 5, 1);
        char raw[8];
        std::memcpy(raw, &v.d, 8);
        out->append(raw, 8);
        break;
    }
    case NetUserVar::Kind::String: pb::bytes_field(out, 6, v.s); break;
    case NetUserVar::Kind::Json: pb::bytes_field(out, 7, v.s); break;
    case NetUserVar::Kind::Bool: pb::key(out, 9, 0); pb::varint(out, v.b ? 1 : 0); break;
    case NetUserVar::Kind::Binary: pb::bytes_field(out, 10, v.s); break;
    case NetUserVar::Kind::Null: break;
    }
    pb::uint_field(out, 8, v.module ? 1 : 0);
}

std::string encode(const Wrapper& w) {
    std::string body;
    switch (w.kind) {
    case kPostLua:
        str_field(&body, 1, w.channel);
        str_field(&body, 2, w.payload);
        str_field(&body, 3, w.module);
        pb::uint_field(&body, 4, w.requestId);
        pb::uint_field(&body, 5, w.replyId);
        pb::uint_field(&body, 6, w.binary ? 1 : 0);
        break;
    case kResetLua: pb::uint_field(&body, 1, w.bootstrap ? 1 : 0); break;
    case kHello: pb::uint_field(&body, 1, w.version); break;
    case kKick: str_field(&body, 1, w.kick); break;
    case kUserVars:
        for (const auto& v : w.vars) {
            std::string var;
            encode_var(&var, v);
            pb::bytes_field(&body, 1, var);
        }
        break;
    default: break;
    }
    std::string out;
    pb::bytes_field(&out, w.kind, body);
    return out;
}

bool decode_var(const std::string& data, NetUserVar* v) {
    pb::Reader r(data.data(), data.size());
    std::uint64_t lo = 0, hi = 0;
    unsigned field = 0, wire = 0;
    while (r.next(&field, &wire)) {
        std::uint64_t n = 0;
        if (field == 1 && wire == 0) { if (!r.read_varint(&lo)) return false; }
        else if (field == 2 && wire == 0) { if (!r.read_varint(&hi)) return false; }
        else if (field == 3 && wire == 2) { if (!r.read_bytes(&v->key)) return false; }
        else if (field == 4 && wire == 0) {
            if (!r.read_varint(&n)) return false;
            v->kind = NetUserVar::Kind::Int;
            v->i = (std::int64_t)n;
        } else if (field == 5 && wire == 1) {
            std::string raw;
            if (!r.read_fixed(&raw, 8)) return false;
            v->kind = NetUserVar::Kind::Double;
            std::memcpy(&v->d, raw.data(), 8);
        } else if ((field == 6 || field == 7 || field == 10) && wire == 2) {
            if (!r.read_bytes(&v->s)) return false;
            v->kind = field == 6 ? NetUserVar::Kind::String
                    : field == 7 ? NetUserVar::Kind::Json : NetUserVar::Kind::Binary;
        } else if (field == 9 && wire == 0) {
            if (!r.read_varint(&n)) return false;
            v->kind = NetUserVar::Kind::Bool;
            v->b = n != 0;
        } else if (field == 8 && wire == 0) {
            if (!r.read_varint(&n)) return false;
            v->module = n == 1;
        } else if (!r.skip(wire)) {
            return false;
        }
    }
    v->guid = format_guid(lo, hi);
    return true;
}

bool decode(const char* data, std::size_t size, Wrapper* w) {
    pb::Reader outer(data, size);
    unsigned field = 0, wire = 0;
    std::string body;
    while (outer.next(&field, &wire)) {
        if (wire == 2 && (field == kPostLua || field == kResetLua || field == kHello
                          || field == kKick || field == kUserVars)) {
            if (!outer.read_bytes(&body)) return false;
            w->kind = field;
        } else if (!outer.skip(wire)) {
            return false;
        }
    }
    if (w->kind == 0) return false;

    pb::Reader r(body.data(), body.size());
    while (r.next(&field, &wire)) {
        std::uint64_t n = 0;
        bool ok = true;
        switch (w->kind) {
        case kPostLua:
            if (wire == 2 && field == 1) ok = r.read_bytes(&w->channel);
            else if (wire == 2 && field == 2) ok = r.read_bytes(&w->payload);
            else if (wire == 2 && field == 3) ok = r.read_bytes(&w->module);
            else if (wire == 0 && field == 4) { ok = r.read_varint(&n); w->requestId = (std::uint32_t)n; }
            else if (wire == 0 && field == 5) { ok = r.read_varint(&n); w->replyId = (std::uint32_t)n; }
            else if (wire == 0 && field == 6) { ok = r.read_varint(&n); w->binary = n == 1; }
            else ok = r.skip(wire);
            break;
        case kResetLua:
            if (wire == 0 && field == 1) { ok = r.read_varint(&n); w->bootstrap = n != 0; }
            else ok = r.skip(wire);
            break;
        case kHello:
            if (wire == 0 && field == 1) { ok = r.read_varint(&n); w->version = (std::uint32_t)n; }
            else ok = r.skip(wire);
            break;
        case kKick:
            if (wire == 2 && field == 1) ok = r.read_bytes(&w->kick);
            else ok = r.skip(wire);
            break;
        case kUserVars:
            if (wire == 2 && field == 1) {
                std::string var;
                ok = r.read_bytes(&var);
                NetUserVar v;
                if (ok && decode_var(var, &v)) w->vars.push_back(std::move(v));
            } else {
                ok = r.skip(wire);
            }
            break;
        }
        if (!ok) return false;
    }
    return true;
}

// ---- engine ABI ----
//
// Declared as the engine's are, so clang lays the vtables out the same:
// two destructor slots, then the virtuals in order.

// net::BitstreamSerializer: IsWriting at +8, WriteBytes and ReadBytes in slots 2 and 3.
bool serializer_writing(void* s) { return *reinterpret_cast<std::uint32_t*>(static_cast<char*>(s) + 8) != 0; }
void serializer_write(void* s, const void* buf, std::uint64_t n) {
    auto fn = reinterpret_cast<void (*)(void*, const void*, std::uint64_t)>((*static_cast<void***>(s))[2]);
    fn(s, buf, n);
}
void serializer_read(void* s, void* buf, std::uint64_t n) {
    auto fn = reinterpret_cast<void (*)(void*, void*, std::uint64_t)>((*static_cast<void***>(s))[3]);
    fn(s, buf, n);
}

class EngineMessage {
public:
    virtual ~EngineMessage() = default;
    virtual void Serialize(void* serializer) = 0;
    virtual void Unknown() {}
    virtual EngineMessage* CreateNew() = 0;
    virtual void Reset() {
        Timestamp = 0;
        OriginalSize = 0;
        Latency = 0;
    }

    std::uint32_t MsgId = 0;
    std::uint32_t Reliability = 4;
    std::uint32_t Priority = 1;
    std::uint8_t OrderingSequence = 0;
    bool Timestamped = false;
    std::uint64_t Timestamp = 0;
    std::uint32_t OriginalSize = 0;
    float Latency = 0;
};
static_assert(sizeof(EngineMessage) == 0x28, "net::Message is 0x28 bytes");

class ExtenderMessage final : public EngineMessage {
public:
    ExtenderMessage() { MsgId = kExtenderMessage; }

    void Serialize(void* serializer) override {
        if (serializer_writing(serializer)) {
            std::uint32_t size = (std::uint32_t)bytes.size();
            if (size > kMaxPayload) {
                // Zero length tells the peer the packet failed to serialize.
                logf("net: tried to write a packet of %u bytes, the maximum is %u", size, kMaxPayload);
                size = 0;
            }
            serializer_write(serializer, &size, sizeof(size));
            if (size > 0) serializer_write(serializer, bytes.data(), size);
            return;
        }
        std::uint32_t size = 0;
        valid = false;
        serializer_read(serializer, &size, sizeof(size));
        if (size > kMaxPayload) {
            logf("net: tried to read a packet of %u bytes, the maximum is %u", size, kMaxPayload);
        } else if (size > 0) {
            bytes.assign(size, '\0');
            serializer_read(serializer, bytes.data(), size);
            decoded = Wrapper{};
            valid = decode(bytes.data(), bytes.size(), &decoded);
            if (!valid) logf("net: failed to decode an extender message");
        }
    }

    EngineMessage* CreateNew() override { return new ExtenderMessage(); }

    void Reset() override {
        EngineMessage::Reset();
        bytes.clear();
        decoded = Wrapper{};
        valid = false;
    }

    std::string bytes;
    Wrapper decoded;
    bool valid = false;
};

void* vptr_of(const void* object) {
    void* vptr = nullptr;
    safe_read(object, &vptr, sizeof(vptr));
    return vptr;
}

void* extender_message_vtable() {
    static void* vtable = [] {
        ExtenderMessage probe;
        return *reinterpret_cast<void**>(&probe);
    }();
    return vtable;
}

class EngineProtocol {
public:
    virtual ~EngineProtocol() = default;
    virtual int ProcessMsg(void* unused, void* context, EngineMessage* msg) = 0;
    virtual int PreUpdate(void const*) { return 0; }
    virtual int PostUpdate(void const*) { return 0; }
    virtual void OnAddedToHost() {}
    virtual void OnRemovedFromHost() {}
    virtual void Reset() {}

    void* Peer = nullptr;
};

void handle_extender(bool server, std::int32_t user, const Wrapper& w);
void on_client_connect(void* context, EngineMessage* msg);

class ExtenderProtocol final : public EngineProtocol {
public:
    explicit ExtenderProtocol(bool server) : server_(server) {}

    // ProtocolResult: 0 Unhandled, 1 Handled.
    int ProcessMsg(void*, void* context, EngineMessage* msg) override {
        // Every message the peer receives passes here, so plain reads: the
        // engine hands over live objects.
        const std::uint32_t id = msg->MsgId;
        if (id == kExtenderMessage) {
            if (*reinterpret_cast<void**>(msg) == extender_message_vtable()) {
                auto* m = static_cast<ExtenderMessage*>(msg);
                const std::int32_t user = *static_cast<std::int32_t*>(context);
                if (m->valid) handle_extender(server_, user, m->decoded);
            }
            return 1;
        }
        if (server_ && id == kClientConnect) on_client_connect(context, msg);
        return 0;
    }

private:
    bool server_;
};

void* extender_protocol_vtable() {
    static void* vtable = [] {
        ExtenderProtocol probe(false);
        return *reinterpret_cast<void**>(&probe);
    }();
    return vtable;
}

// ---- state ----

std::mutex g_mutex;

// Server: the peers that sent an ExtenderHello, with their protocol version.
std::map<std::int32_t, std::uint32_t> g_peer_versions;
std::atomic<void*> g_server{nullptr};
std::atomic<long long> g_server_tick_ms{0};

// Client: whether the host sent its ExtenderHello, and its version.
std::atomic<void*> g_client{nullptr};
std::atomic<bool> g_host_extended{false};
std::atomic<std::uint32_t> g_host_version{kVersionInitial};

// A developer switch: the host's own client treated as remote, so single
// player exercises the network path (Ext._Internal.NetForceRemote).
std::atomic<bool> g_force_remote{false};

using SerializeProc = void (*)(EngineMessage*, void*);
SerializeProc g_connect_serialize = nullptr;

long long now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void warn_once(std::atomic<bool>& flag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void warn_once(std::atomic<bool>& flag, const char* fmt, ...) {
    if (flag.exchange(true)) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    statusf("WARNING: net: %s", line);
}

template <class T>
bool read_at(const void* base, std::size_t offset, T* out) {
    return safe_read(static_cast<const char*>(base) + offset, out, sizeof(T));
}

struct PtrArray {
    void** buf;
    std::uint32_t cap;
    std::uint32_t size;
};

// A peer's factory, checked against what it holds: pool 1's template is
// message 1 and pool 6's is ClientConnect.
char* checked_factory(void* peer) {
    char* factory = nullptr;
    void** pools = nullptr;
    std::uint32_t count = 0;
    if (!read_at(peer, kPeerFactory, &factory) || factory == nullptr
        || !read_at(factory, kFactoryPools, &pools) || pools == nullptr
        || !read_at(factory, kFactoryPoolCount, &count) || count < 50 || count > 4096) {
        return nullptr;
    }
    for (std::uint32_t id : {1u, kClientConnect}) {
        void* pool = nullptr;
        void* tmpl = nullptr;
        std::uint32_t msgId = 0;
        if (!safe_read(&pools[id], &pool, sizeof(pool)) || pool == nullptr
            || !read_at(pool, 0, &tmpl) || tmpl == nullptr
            || !read_at(tmpl, 8, &msgId) || msgId != id) {
            return nullptr;
        }
    }
    return factory;
}

void* pool_template(char* factory, std::uint32_t id) {
    void** pools = nullptr;
    std::uint32_t count = 0;
    void* pool = nullptr;
    void* tmpl = nullptr;
    if (!read_at(factory, kFactoryPools, &pools) || !read_at(factory, kFactoryPoolCount, &count)
        || id >= count || !safe_read(&pools[id], &pool, sizeof(pool)) || pool == nullptr
        || !read_at(pool, 0, &tmpl)) {
        return nullptr;
    }
    return tmpl;
}

// The protocol list, checked: every entry names this peer as its own.
PtrArray* checked_protocols(void* peer) {
    auto* list = reinterpret_cast<PtrArray*>(static_cast<char*>(peer) + kPeerProtocols);
    PtrArray copy{};
    if (!safe_read(list, &copy, sizeof(copy)) || copy.buf == nullptr || copy.size == 0
        || copy.size > 64 || copy.size > copy.cap) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < copy.size; ++i) {
        void* protocol = nullptr;
        void* owner = nullptr;
        if (!safe_read(&copy.buf[i], &protocol, sizeof(protocol)) || protocol == nullptr) continue;
        if (!read_at(protocol, 8, &owner) || owner != peer) return nullptr;
    }
    return list;
}

bool has_our_protocol(PtrArray* list) {
    for (std::uint32_t i = 0; i < list->size; ++i) {
        if (list->buf[i] != nullptr && *static_cast<void**>(list->buf[i]) == extender_protocol_vtable()) {
            return true;
        }
    }
    return false;
}

bool has_our_message(char* factory) {
    void** pools = *reinterpret_cast<void***>(factory + kFactoryPools);
    const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(factory + kFactoryPoolCount);
    return pools != nullptr && count > kExtenderMessage && pools[kExtenderMessage] != nullptr;
}

// At the head of the list, as upstream inserts it, so message 400 is ours
// before any of the game's protocols sees it.
bool insert_protocol(void* peer, PtrArray* list, bool server) {
    if (target::NetProtocolsReserve() == 0) return false;
    if (list->size >= list->cap) {
        auto reserve = reinterpret_cast<void (*)(PtrArray*, std::uint64_t)>(
            load_bias() + target::NetProtocolsReserve());
        reserve(list, (std::uint64_t)list->size + 1);
        if (list->size >= list->cap) return false;
    }
    auto* protocol = new ExtenderProtocol(server);
    protocol->Peer = peer;
    std::memmove(list->buf + 1, list->buf, (std::size_t)list->size * sizeof(void*));
    list->buf[0] = protocol;
    list->size += 1;
    return true;
}

// Message 400 in the factory, through the engine's own Register, under the
// lock GetFreeMessage takes.
bool register_message(char* factory) {
    if (pool_template(factory, kExtenderMessage) != nullptr) return true;
    if (target::NetRegisterMessage() == 0) return false;
    auto reg = reinterpret_cast<void (*)(void*, std::int32_t, void*, std::int32_t, const char*)>(
        load_bias() + target::NetRegisterMessage());
    auto* mutex = reinterpret_cast<pthread_mutex_t*>(factory + kFactoryLock);
    auto* depth = reinterpret_cast<std::int32_t*>(factory + kFactoryLockDepth);
    pthread_mutex_lock(mutex);
    ++*depth;
    reg(factory, (std::int32_t)kExtenderMessage, new ExtenderMessage(), 4, "bg3se::net::ExtenderMessage");
    --*depth;
    pthread_mutex_unlock(mutex);
    return pool_template(factory, kExtenderMessage) != nullptr;
}

// ClientConnect's Serialize: a client writing it appends the signature to
// its build string, which is how the server learns it has the extender.
void connect_serialize_hook(EngineMessage* msg, void* serializer) {
    if (serializer_writing(serializer)) {
        // A new connection: nothing goes to the host until it says hello.
        g_host_extended.store(false);
        g_host_version.store(kVersionInitial);
        auto* build = reinterpret_cast<unsigned char*>(msg) + kConnectBuild;
        std::string text;
        static std::atomic<bool> warned{false};
        if (read_ls_string(build, &text) && text.find(" Extender") == std::string::npos) {
            const std::string tagged = text + kSignature;
            if ((build[15] & 0x80) == 0 && tagged.size() <= 15) {
                std::memset(build, 0, 16);
                std::memcpy(build, tagged.data(), tagged.size());
                build[15] = (unsigned char)tagged.size();
                logf("net: appending the extender signature to ClientConnect (\"%s\")", tagged.c_str());
            } else {
                warn_once(warned, "the build string \"%s\" has no room for the extender signature; "
                          "the host will not know this client has the Script Extender", text.c_str());
            }
        }
    }
    g_connect_serialize(msg, serializer);
}

bool hook_client_connect(char* factory) {
    static std::once_flag once;
    static bool hooked = false;
    std::call_once(once, [&] {
        void* tmpl = pool_template(factory, kClientConnect);
        void** vtable = static_cast<void**>(vptr_of(tmpl));
        void* serialize = nullptr;
        if (vtable == nullptr || !safe_read(&vtable[2], &serialize, sizeof(serialize))
            || serialize == nullptr) {
            return;
        }
        // Set before the slot changes: another thread may serialize meanwhile.
        g_connect_serialize = reinterpret_cast<SerializeProc>(serialize);
        void* original = nullptr;
        if (hook_pointer(&vtable[2], reinterpret_cast<void*>(&connect_serialize_hook), &original)) {
            hooked = true;
            logf("net: hooked ClientConnectMessage::Serialize (was image+%#lx)",
                 (unsigned long)(reinterpret_cast<std::uintptr_t>(original) - load_bias()));
        }
    });
    return hooked;
}

// A peer checked once, and again if it or its factory changes.
struct CheckedPeer {
    void* peer = nullptr;
    char* factory = nullptr;
    PtrArray* list = nullptr;
};

// Upstream's ExtendNetworking, for one peer; run every tick, on the peer's
// own thread, so it stays cheap once the peer is checked.
bool extend(void* peer, bool server, CheckedPeer& checked) {
    static std::atomic<bool> warnedLayout{false};
    static std::atomic<bool> warnedTargets{false};
    if (checked.peer != peer || checked.factory == nullptr
        || *reinterpret_cast<char**>(static_cast<char*>(peer) + kPeerFactory) != checked.factory) {
        checked = CheckedPeer{peer, checked_factory(peer), nullptr};
        checked.list = checked.factory != nullptr ? checked_protocols(peer) : nullptr;
        if (checked.list == nullptr) checked.factory = nullptr;
    }
    char* factory = checked.factory;
    PtrArray* list = checked.list;
    if (factory == nullptr) {
        warn_once(warnedLayout, "the %s's network layout is not the one bg3le knows; "
                  "Ext.Net stays local to this machine", server ? "server" : "client");
        return false;
    }
    if (has_our_message(factory) && has_our_protocol(list)) return true;

    if (target::NetGetFreeMessage() == 0 || target::NetRegisterMessage() == 0
        || target::NetSendSinglePeer() == 0 || target::NetProtocolsReserve() == 0) {
        warn_once(warnedTargets, "engine network functions not found in this build; "
                  "Ext.Net stays local to this machine");
        return false;
    }
    if (!register_message(factory)) {
        logf("net: could not register message %u on the %s", kExtenderMessage, server ? "server" : "client");
        return false;
    }
    if (!has_our_protocol(list) && !insert_protocol(peer, list, server)) {
        logf("net: could not add the extender protocol to the %s", server ? "server" : "client");
        return false;
    }
    hook_client_connect(factory);
    logf("net: registered the extender protocol on the %s (peer %p)", server ? "server" : "client", peer);
    return true;
}

// ---- sending ----

bool send_to_peer(void* peer, std::int32_t peerId, const std::string& bytes) {
    static std::atomic<bool> warned{false};
    char* factory = nullptr;
    if (!read_at(peer, kPeerFactory, &factory) || factory == nullptr) return false;
    if (pool_template(factory, kExtenderMessage) == nullptr) return false;

    void** vtable = static_cast<void**>(vptr_of(peer));
    void* send = nullptr;
    if (vtable == nullptr || !safe_read(&vtable[kSlotSendSingle], &send, sizeof(send))
        || reinterpret_cast<std::uintptr_t>(send) != load_bias() + target::NetSendSinglePeer()) {
        warn_once(warned, "the peer's SendMessageSinglePeer is not where bg3le expects it");
        return false;
    }

    auto getFree = reinterpret_cast<EngineMessage* (*)(void*, std::int32_t)>(
        load_bias() + target::NetGetFreeMessage());
    EngineMessage* msg = getFree(factory, (std::int32_t)kExtenderMessage);
    if (msg == nullptr || vptr_of(msg) != extender_message_vtable()) {
        logf("net: could not get a free extender message");
        return false;
    }
    static_cast<ExtenderMessage*>(msg)->bytes = bytes;
    reinterpret_cast<void (*)(void*, std::int32_t, EngineMessage*)>(send)(peer, peerId, msg);
    return true;
}

std::int32_t peer_of(std::int64_t user) { return (std::int32_t)((std::uint32_t)user >> 16); }

// The server's own client: LocalPeerId if it reads sensibly, else peer 1,
// which is what upstream assumes for local delivery.
std::int32_t local_peer(void* server) {
    std::int32_t id = 0;
    if (server != nullptr && read_at(server, kServerLocalPeer, &id) && id > 0 && id < 0x10000) return id;
    return 1;
}

bool peer_active(void* server, std::int32_t peerId) {
    PtrArray ids{};
    if (!read_at(server, kServerActivePeers, &ids) || ids.buf == nullptr || ids.size > 1024) return false;
    for (std::uint32_t i = 0; i < ids.size; ++i) {
        std::int32_t id = 0;
        if (safe_read(reinterpret_cast<std::int32_t*>(ids.buf) + i, &id, sizeof(id)) && id == peerId) {
            return true;
        }
    }
    return false;
}

// Server: to one peer, if it has said hello.
bool server_send(std::int32_t peerId, const std::string& bytes) {
    void* server = g_server.load();
    if (server == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_peer_versions.find(peerId) == g_peer_versions.end()) return false;
    }
    return send_to_peer(server, peerId, bytes);
}

// Server: to every peer on another machine that has the extender.
int server_broadcast(const std::string& bytes, std::int32_t excludePeer) {
    void* server = g_server.load();
    if (server == nullptr) return 0;
    const std::int32_t local = g_force_remote.load() ? -1 : local_peer(server);
    std::vector<std::int32_t> peers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [peer, version] : g_peer_versions) {
            if (peer != local && peer != excludePeer) peers.push_back(peer);
        }
    }
    int sent = 0;
    for (std::int32_t peer : peers) {
        if (peer_active(server, peer) && send_to_peer(server, peer, bytes)) ++sent;
    }
    return sent;
}

bool client_send(const std::string& bytes) {
    void* client = g_client.load();
    std::int32_t host = -1;
    if (client == nullptr || !g_host_extended.load() || !read_at(client, kClientHostPeer, &host) || host < 0) {
        return false;
    }
    return send_to_peer(client, host, bytes);
}

std::uint32_t server_shared_version() {
    std::uint32_t version = kVersionCurrent;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [peer, v] : g_peer_versions) version = std::min(version, v);
    return version;
}

// ---- receiving ----

NetInbound inbound_post(const Wrapper& w, std::int64_t user) {
    NetInbound in;
    in.kind = NetInbound::Kind::Post;
    in.channel = w.channel;
    in.payload = w.payload;
    in.module = w.module;
    in.user = user;
    in.requestId = w.requestId;
    in.replyId = w.replyId;
    in.binary = w.binary;
    return in;
}

void handle_extender(bool server, std::int32_t user, const Wrapper& w) {
    if (server) {
        switch (w.kind) {
        case kPostLua:
            lua_net_deliver(true, inbound_post(w, user));
            return;
        case kHello: {
            const std::int32_t peer = peer_of(user);
            logf("net: extender hello from user %d (peer %d, version %u)", user, peer, w.version);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_peer_versions[peer] = w.version;
            return;
        }
        case kUserVars: {
            NetInbound in;
            in.kind = NetInbound::Kind::UserVars;
            in.user = user;
            in.vars = w.vars;
            lua_net_deliver(true, std::move(in));
            return;
        }
        default:
            statusf("ERROR: net: unknown extension message type %u received", w.kind);
            return;
        }
    }

    switch (w.kind) {
    case kPostLua:
        lua_net_deliver(false, inbound_post(w, kReservedUser));
        return;
    case kHello: {
        logf("net: extender hello from the host (version %u)", w.version);
        g_host_version.store(w.version);
        g_host_extended.store(true);
        Wrapper reply;
        reply.kind = kHello;
        reply.version = kVersionCurrent;
        if (!client_send(encode(reply))) logf("net: could not answer the host's hello");
        return;
    }
    case kResetLua: {
        NetInbound in;
        in.kind = NetInbound::Kind::ResetLua;
        lua_net_deliver(false, std::move(in));
        return;
    }
    case kKick:
        statusf("ERROR: the host disconnected this client: %s", w.kick.c_str());
        return;
    case kUserVars: {
        NetInbound in;
        in.kind = NetInbound::Kind::UserVars;
        in.user = kReservedUser;
        in.vars = w.vars;
        lua_net_deliver(false, std::move(in));
        return;
    }
    default:
        statusf("ERROR: net: unknown extension message type %u received", w.kind);
        return;
    }
}

// Server: a client connecting. Upstream's OnClientConnectMessage, and its
// peer forgotten first, so a peer ID reused by a client without the
// extender is never sent message 400.
void on_client_connect(void* context, EngineMessage* msg) {
    std::int32_t user = kReservedUser;
    safe_read(context, &user, sizeof(user));
    const std::int32_t peer = peer_of(user);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_peer_versions.erase(peer);
    }

    std::string build;
    read_ls_string(reinterpret_cast<char*>(msg) + kConnectBuild, &build);
    if (build.find(" Extender") == std::string::npos) {
        logf("net: no extender trailer in ClientConnect from peer %d (\"%s\")", peer, build.c_str());
        return;
    }
    if (build.find(kSignature) == std::string::npos) {
        logf("net: extender signature incorrect in ClientConnect from peer %d (\"%s\")", peer, build.c_str());
        return;
    }
    void* server = g_server.load();
    Wrapper hello;
    hello.kind = kHello;
    hello.version = kVersionCurrent;
    logf("net: sending ExtenderHello to peer %d", peer);
    if (server == nullptr || !send_to_peer(server, peer, encode(hello))) {
        logf("net: could not send ExtenderHello to peer %d", peer);
    }
}

// ---- Lua ----

void table_field(lua_State* L, const char* name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

std::string opt_string(lua_State* L, int index) {
    std::size_t length = 0;
    const char* s = lua_isstring(L, index) ? lua_tolstring(L, index, &length) : nullptr;
    return s != nullptr ? std::string(s, length) : std::string();
}

}  // namespace

// ---- the ticks ----

void net_server_tick(void* gameServer) {
    if (gameServer == nullptr) return;
    void* previous = g_server.exchange(gameServer);
    if (previous != gameServer) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_peer_versions.clear();
    }
    g_server_tick_ms.store(now_ms());
    static CheckedPeer checked;
    extend(gameServer, true, checked);
}

void net_client_tick() {
    static std::atomic<bool> warned{false};
    if (target::EoCClient() == 0) {
        warn_once(warned, "ecl::EoCClient not found; Ext.Net stays local to this machine");
        return;
    }
    // The global is in the image and EoCClient is live once it is set.
    char* eoc = *reinterpret_cast<char**>(load_bias() + target::EoCClient());
    void* client = eoc != nullptr ? *reinterpret_cast<void**>(eoc + kEoCClientGameClient) : nullptr;
    if (client == nullptr) return;
    void* previous = g_client.exchange(client);
    if (previous != client) {
        g_host_extended.store(false);
        g_host_version.store(kVersionInitial);
    }
    static CheckedPeer checked;
    extend(client, false, checked);
}

bool net_hosting() {
    return g_server.load() != nullptr && now_ms() - g_server_tick_ms.load() < 30000;
}

// The first player of the server's own peer, in its PeerInfo: upstream's
// GameServer::GetLocalUserId.
std::int64_t net_local_user() {
    void* server = g_server.load();
    if (server == nullptr) return ((std::int64_t)1 << 16) | 1;
    const std::int32_t peer = local_peer(server);
    std::uint16_t slot = 1;

    struct LegacyMap { std::uint32_t count; std::uint32_t hashSize; void** table; } info{};
    if (read_at(server, kServerPeerInfo, &info) && info.table != nullptr && info.hashSize > 0
        && info.hashSize < 4096) {
        void* node = nullptr;
        safe_read(&info.table[(std::uint32_t)peer % info.hashSize], &node, sizeof(node));
        for (int guard = 0; node != nullptr && guard < 64; ++guard) {
            std::int32_t key = 0;
            read_at(node, 8, &key);
            if (key == peer) {
                // GamePeerInfo at +0x10: Flags, then the Players map at +8.
                LegacyMap players{};
                if (read_at(node, 0x10 + 8, &players) && players.table != nullptr
                    && players.hashSize > 0 && players.hashSize < 4096) {
                    for (std::uint32_t b = 0; b < players.hashSize; ++b) {
                        void* player = nullptr;
                        if (safe_read(&players.table[b], &player, sizeof(player)) && player != nullptr) {
                            read_at(player, 8, &slot);
                            break;
                        }
                    }
                }
                break;
            }
            void* next = nullptr;
            read_at(node, 0, &next);
            node = next;
        }
    }
    return ((std::int64_t)peer << 16) | slot;
}

void net_reset_remote_clients() {
    Wrapper reset;
    reset.kind = kResetLua;
    reset.bootstrap = true;
    const int sent = server_broadcast(encode(reset), -1);
    if (sent > 0) logf("net: asked %d remote client(s) to reset their Lua", sent);
}

// Ext._Internal.NetInfo() -> table
int l_net_info(lua_State* L) {
    const bool client = lua_state_is_client(L);
    const bool forced = g_force_remote.load();
    const bool hosting = net_hosting() && !(client && forced);
    lua_createtable(L, 0, 8);
    lua_pushboolean(L, hosting);
    lua_setfield(L, -2, "Hosting");
    table_field(L, "ReservedUser", kReservedUser);
    if (client) {
        void* gameClient = g_client.load();
        std::int32_t host = -1;
        const bool connected = forced
            || (gameClient != nullptr && read_at(gameClient, kClientHostPeer, &host) && host >= 0);
        lua_pushboolean(L, connected);
        lua_setfield(L, -2, "Connected");
        // Whether this process runs a game server, ticking or not yet: upstream's
        // GameServer being there, which a load has before the first tick.
        char const* serverState = forced ? nullptr : server_game_state();
        const bool serverRunning = serverState != nullptr
            && std::strcmp(serverState, "Uninitialized") != 0 && std::strcmp(serverState, "Unknown") != 0;
        lua_pushboolean(L, serverRunning);
        lua_setfield(L, -2, "ServerRunning");
        const bool extended = g_host_extended.load();
        lua_pushboolean(L, extended);
        lua_setfield(L, -2, "HostHasExtender");
        const std::uint32_t version = hosting ? kVersionCurrent
            : std::min(extended ? g_host_version.load() : kVersionInitial, kVersionCurrent);
        table_field(L, "Version", version);
        table_field(L, "LocalUser", hosting || serverRunning ? net_local_user() : kReservedUser);
        return 1;
    }
    void* server = g_server.load();
    table_field(L, "Version", server_shared_version());
    table_field(L, "LocalPeer", forced ? -1 : local_peer(server));
    table_field(L, "LocalUser", net_local_user());
    lua_newtable(L);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [peer, version] : g_peer_versions) {
            lua_pushinteger(L, version);
            lua_rawseti(L, -2, peer);
        }
    }
    lua_setfield(L, -2, "Peers");
    return 1;
}

// Ext._Internal.NetForceRemote(on) -> the state it was in. On the server,
// turning it on greets the host's own client as a remote one would be.
int l_net_force_remote(lua_State* L) {
    const bool on = lua_toboolean(L, 1) != 0;
    const bool was = g_force_remote.exchange(on);
    void* server = g_server.load();
    if (on && !lua_state_is_client(L) && server != nullptr) {
        Wrapper hello;
        hello.kind = kHello;
        hello.version = kVersionCurrent;
        const std::int32_t peer = local_peer(server);
        logf("net: forced remote; sending ExtenderHello to the host's own client (peer %d)", peer);
        send_to_peer(server, peer, encode(hello));
    }
    lua_pushboolean(L, was);
    return 1;
}

// Ext._Internal.NetPost(to, channel, payload, module, requestId, replyId, binary, excludeUser)
//   server: to is a user ID, or "broadcast" (every peer on another machine
//   but the excluded user's); client: to is ignored, it goes to the host.
//   -> true, or false and why
int l_net_post(lua_State* L) {
    Wrapper w;
    w.kind = kPostLua;
    w.channel = luaL_checkstring(L, 2);
    w.payload = opt_string(L, 3);
    w.module = opt_string(L, 4);
    w.requestId = (std::uint32_t)luaL_optinteger(L, 5, 0);
    w.replyId = (std::uint32_t)luaL_optinteger(L, 6, 0);
    w.binary = lua_toboolean(L, 7) != 0;
    const std::string bytes = encode(w);

    if (lua_state_is_client(L)) {
        if (client_send(bytes)) {
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Attempted to send extender message to a host that does not understand extender protocol!");
        return 2;
    }

    if (lua_type(L, 1) == LUA_TSTRING) {
        const std::int32_t exclude = lua_isinteger(L, 8) ? peer_of(lua_tointeger(L, 8)) : -1;
        server_broadcast(bytes, exclude);
        lua_pushboolean(L, 1);
        return 1;
    }
    const std::int64_t user = luaL_checkinteger(L, 1);
    if (server_send(peer_of(user), bytes)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushfstring(L, "Attempted to send extender message to user %d that does not understand extender protocol!",
                    (int)user);
    return 2;
}

// Ext._Internal.NetSyncVars(vars) -> how many messages went out
//   vars: {{Module = bool, Guid, Key, Value}, ...}; a table value arrives
//   already serialized, as {Binary = string}.
int l_net_sync_vars(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Wrapper w;
    w.kind = kUserVars;
    const lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 1, i);
        NetUserVar v;
        lua_getfield(L, -1, "Module");
        v.module = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "Guid");
        v.guid = opt_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "Key");
        v.key = opt_string(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "Value");
        switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN: v.kind = NetUserVar::Kind::Bool; v.b = lua_toboolean(L, -1) != 0; break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1)) {
                v.kind = NetUserVar::Kind::Int;
                v.i = lua_tointeger(L, -1);
            } else {
                v.kind = NetUserVar::Kind::Double;
                v.d = lua_tonumber(L, -1);
            }
            break;
        case LUA_TSTRING: v.kind = NetUserVar::Kind::String; v.s = opt_string(L, -1); break;
        case LUA_TTABLE:
            lua_getfield(L, -1, "Binary");
            if (lua_isstring(L, -1)) {
                v.kind = NetUserVar::Kind::Binary;
                v.s = opt_string(L, -1);
            }
            lua_pop(L, 1);
            break;
        default: break;
        }
        lua_pop(L, 2);
        w.vars.push_back(std::move(v));
    }

    // Split well below the packet limit, as upstream's sync budget does.
    int sent = 0;
    const bool client = lua_state_is_client(L);
    std::size_t at = 0;
    while (at < w.vars.size()) {
        Wrapper part;
        part.kind = kUserVars;
        std::size_t budget = 0;
        while (at < w.vars.size() && (part.vars.empty() || budget < 0x40000)) {
            budget += w.vars[at].key.size() + w.vars[at].s.size() + 64;
            part.vars.push_back(w.vars[at++]);
        }
        const std::string bytes = encode(part);
        if (client) {
            if (client_send(bytes)) ++sent;
        } else {
            sent += server_broadcast(bytes, -1) > 0 ? 1 : 0;
        }
    }
    lua_pushinteger(L, sent);
    return 1;
}

}  // namespace bg3le
