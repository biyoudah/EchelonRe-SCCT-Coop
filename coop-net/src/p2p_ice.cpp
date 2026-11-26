#include <juice/juice.h>

#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {

typedef void(__stdcall* ScctIceStateCb)(int state, void* user);
typedef void(__stdcall* ScctIceCandCb)(const char* sdp, void* user);
typedef void(__stdcall* ScctIceGatherDoneCb)(void* user);
typedef void(__stdcall* ScctIceRecvCb)(const uint8_t* data, int size, void* user);

struct ScctIceWrapper {
    juice_agent_t* agent = nullptr;
    ScctIceStateCb on_state = nullptr;
    ScctIceCandCb on_cand = nullptr;
    ScctIceGatherDoneCb on_gather_done = nullptr;
    ScctIceRecvCb on_recv = nullptr;
    void* user = nullptr;
};

static void OnJuiceState(juice_agent_t*, juice_state_t state, void* user_ptr) {
    auto* w = static_cast<ScctIceWrapper*>(user_ptr);
    if (w && w->on_state) {
        w->on_state(static_cast<int>(state), w->user);
    }
}

static void OnJuiceCand(juice_agent_t*, const char* sdp, void* user_ptr) {
    auto* w = static_cast<ScctIceWrapper*>(user_ptr);
    if (w && w->on_cand && sdp) {
        w->on_cand(sdp, w->user);
    }
}

static void OnJuiceGatherDone(juice_agent_t*, void* user_ptr) {
    auto* w = static_cast<ScctIceWrapper*>(user_ptr);
    if (w && w->on_gather_done) {
        w->on_gather_done(w->user);
    }
}

static void OnJuiceRecv(juice_agent_t*, const char* data, size_t size, void* user_ptr) {
    auto* w = static_cast<ScctIceWrapper*>(user_ptr);
    if (w && w->on_recv && data && size > 0) {
        w->on_recv(reinterpret_cast<const uint8_t*>(data), static_cast<int>(size), w->user);
    }
}

__declspec(dllexport) void* __stdcall scct_ice_create(const char* stun_host, int stun_port, const char* turn_host,
                                                      int turn_port, const char* turn_user, const char* turn_pass,
                                                      ScctIceStateCb on_state, ScctIceCandCb on_cand,
                                                      ScctIceGatherDoneCb on_gather_done, ScctIceRecvCb on_recv,
                                                      void* user) {
    auto* w = new ScctIceWrapper();
    w->on_state = on_state;
    w->on_cand = on_cand;
    w->on_gather_done = on_gather_done;
    w->on_recv = on_recv;
    w->user = user;

    juice_config_t cfg{};
    cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    cfg.stun_server_host = (stun_host && stun_host[0]) ? stun_host : "stun.cloudflare.com";
    cfg.stun_server_port = (stun_port > 0 && stun_port <= 65535) ? static_cast<uint16_t>(stun_port) : 3478;

    juice_turn_server_t turn_srv{};
    if (turn_host && turn_host[0] && turn_port > 0) {
        turn_srv.host = turn_host;
        turn_srv.port = static_cast<uint16_t>(turn_port);
        turn_srv.username = (turn_user && turn_user[0]) ? turn_user : nullptr;
        turn_srv.password = (turn_pass && turn_pass[0]) ? turn_pass : nullptr;
        cfg.turn_servers = &turn_srv;
        cfg.turn_servers_count = 1;
    }

    cfg.cb_state_changed = OnJuiceState;
    cfg.cb_candidate = OnJuiceCand;
    cfg.cb_gathering_done = OnJuiceGatherDone;
    cfg.cb_recv = OnJuiceRecv;
    cfg.user_ptr = w;

    w->agent = juice_create(&cfg);
    if (!w->agent) {
        delete w;
        return nullptr;
    }
    return w;
}

__declspec(dllexport) void __stdcall scct_ice_destroy(void* handle) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    if (!w) {
        return;
    }
    if (w->agent) {
        juice_destroy(w->agent);
        w->agent = nullptr;
    }
    delete w;
}

__declspec(dllexport) int __stdcall scct_ice_gather(void* handle) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    return (w && w->agent) ? juice_gather_candidates(w->agent) : -1;
}

__declspec(dllexport) int __stdcall scct_ice_get_local_sdp(void* handle, char* buf, int cap) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    if (!w || !w->agent || !buf || cap <= 0) {
        return -1;
    }
    return juice_get_local_description(w->agent, buf, static_cast<size_t>(cap));
}

__declspec(dllexport) int __stdcall scct_ice_set_remote_sdp(void* handle, const char* sdp) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    return (w && w->agent && sdp) ? juice_set_remote_description(w->agent, sdp) : -1;
}

__declspec(dllexport) int __stdcall scct_ice_add_remote_candidate(void* handle, const char* sdp) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    return (w && w->agent && sdp) ? juice_add_remote_candidate(w->agent, sdp) : -1;
}

__declspec(dllexport) int __stdcall scct_ice_set_remote_gathering_done(void* handle) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    return (w && w->agent) ? juice_set_remote_gathering_done(w->agent) : -1;
}

__declspec(dllexport) int __stdcall scct_ice_send(void* handle, const void* data, int len) {
    auto* w = static_cast<ScctIceWrapper*>(handle);
    if (!w || !w->agent || !data || len <= 0) {
        return -1;
    }
    return juice_send(w->agent, static_cast<const char*>(data), static_cast<size_t>(len));
}

}  // extern "C"
