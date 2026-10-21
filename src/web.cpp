#include "web.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "config.hpp"
#include "webui.hpp"

namespace imp {

namespace {

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

std::string num(double v, int prec = 2) {
    if (!std::isfinite(v)) v = -120;
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

void send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

void respond(int fd, int code, const char* type, const std::string& body) {
    const char* msg = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(fd, h + body);
}

}  // namespace

bool WebServer::start(std::string& err) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { err = "socket failed"; return false; }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)ST.web_port);
    if (inet_pton(AF_INET, ST.web_bind.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = INADDR_ANY;
    if (::bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0 || ::listen(listen_fd_, 8) < 0) {
        err = "cannot listen on port " + std::to_string(ST.web_port) + ": " + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void WebServer::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    // let in-flight requests finish (each has a hard deadline of ~1.5 s)
    for (int i = 0; i < 150 && active_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
}

void WebServer::loop() {
    while (run_) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        if (active_.load() >= 16) { ::close(fd); continue; }  // phones open spare sockets; don't pile up
        timeval tv{0, 500000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        // One short-lived thread per connection, so an idle or slow client can
        // never hold up another request (like MUTE).
        ++active_;
        std::thread([this, fd] {
            handle(fd);
            ::close(fd);
            --active_;
        }).detach();
    }
}

void WebServer::handle(int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    auto expired = [&] { return std::chrono::steady_clock::now() > deadline; };
    std::string req;
    char buf[4096];
    size_t hdr_end = std::string::npos;
    while (req.size() < 65536 && !expired()) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return;
        req.append(buf, (size_t)n);
        if ((hdr_end = req.find("\r\n\r\n")) != std::string::npos) break;
    }
    if (hdr_end == std::string::npos) return;
    std::istringstream rl(req.substr(0, req.find("\r\n")));
    std::string method, path;
    rl >> method >> path;
    size_t clen = 0;
    {
        std::string lower = req.substr(0, hdr_end);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t p = lower.find("content-length:");
        if (p != std::string::npos) clen = (size_t)std::strtoul(lower.c_str() + p + 15, nullptr, 10);
    }
    std::string body = req.substr(hdr_end + 4);
    while (body.size() < clen && body.size() < 65536 && !expired()) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", kWebUiHtml);
    } else if (method == "GET" && path == "/api/status") {
        respond(fd, 200, "application/json", status_json());
    } else if (method == "GET" && path == "/api/params") {
        respond(fd, 200, "application/json", params_json());
    } else if (method == "POST" && path == "/api/set") {
        std::istringstream ss(body);
        std::string kv;
        bool ok = true;
        while (std::getline(ss, kv, '&')) {
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            int id = Params::find(url_decode(kv.substr(0, eq)));
            std::string v = url_decode(kv.substr(eq + 1));
            char* end = nullptr;
            float f = std::strtof(v.c_str(), &end);
            if (id < 0 || v.empty() || *end != '\0' || !P.set((ParamId)id, f)) ok = false;  // rejects nan/inf/junk
        }
        respond(fd, ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/api/action") {
        std::string what;
        size_t p = body.find("do=");
        if (p != std::string::npos) what = url_decode(body.substr(p + 3, body.find('&', p) - (p + 3)));
        bool ok = false;
        std::string msg = do_action(what, ok);
        respond(fd, 200, "application/json",
                std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + json_escape(msg) + "\"}");
    } else {
        respond(fd, 404, "text/plain", "not found");
    }
}

std::string WebServer::do_action(const std::string& what, bool& ok) {
    std::lock_guard<std::mutex> lock(action_mu_);
    ok = true;
    if (what == "save") {
        ok = save_config(config_path_, P, ST);
        return ok ? "Settings saved" : "Could not write " + config_path_;
    }
    if (what == "clear_dynamic") { D.clear_dynamic(); return "Automatic notches cleared"; }
    if (what == "commit_notches") {
        int left = 0;
        int n = D.commit_to_fixed(&left);
        return "Kept " + std::to_string(n) + " notch(es) as fixed" +
               (left ? " (" + std::to_string(left) + " did not fit; clear fixed notches first)" : "");
    }
    if (what == "clear_fixed") { D.clear_fixed(); return "Fixed notches cleared"; }
    if (what == "start_ringout") {
        if (!P.on(P_ringout_mode)) {
            // Start from the gain actually in effect (fader + AGC, capped, minus any
            // safety drop) so entering ring-out never jumps the level.
            pre_ring_gain_ = P.get(P_gain_db);
            have_pre_ring_ = true;
            P.set(P_gain_db, std::round(S.total_gain_db.load() * 2.f) / 2.f);
            P.set(P_ringout_mode, 1);
        }
        return "Ring-out mode: raise Gain slowly until it rings";
    }
    if (what == "cancel_ringout") {
        if (have_pre_ring_) P.set(P_gain_db, pre_ring_gain_);
        have_pre_ring_ = false;
        P.set(P_ringout_mode, 0);
        return "Ring-out cancelled, gain restored";
    }
    if (what == "mark_ring") {
        float ring = P.get(P_gain_db);
        P.set(P_ceiling_db, ring - 6.f);
        P.set(P_gain_db, ring - 6.f - P.get(P_agc_max_boost_db));
        int left = 0;
        int n = D.commit_to_fixed(&left);
        P.set(P_ringout_mode, 0);
        have_pre_ring_ = false;
        char m[200];
        std::snprintf(m, sizeof m, "Ring point %.1f dB. Ceiling %.1f dB, gain %.1f dB, %d notch(es) kept%s. Press Save.",
                      ring, P.get(P_ceiling_db), P.get(P_gain_db), n,
                      left ? " (fixed list full: clear fixed notches and redo ring-out)" : "");
        return m;
    }
    if (what == "set_reference") {
        float lvl = S.speech_dbfs.load();
        if (lvl < -90) { ok = false; return "No speech level measured yet"; }
        P.set(P_agc_ref_dbfs, lvl);
        char m[96];
        std::snprintf(m, sizeof m, "AGC reference set to %.1f dBFS", lvl);
        return m;
    }
    ok = false;
    return "unknown action '" + what + "'";
}

std::string WebServer::status_json() {
    std::string j = "{";
    auto f = [&](const char* k, double v, int prec = 1) { j += "\""; j += k; j += "\":" + num(v, prec) + ","; };
    f("ok", S.device_ok.load(), 0);
    f("lat", S.latency_ms.load());
    f("cpu", S.cpu_pct.load());
    f("xruns", S.xruns.load(), 0);
    f("in_peak", S.in_peak_dbfs.load());
    f("in_lvl", S.in_level_dbfs.load());
    f("noise", S.noise_dbfs.load());
    f("speech_lvl", S.speech_dbfs.load());
    f("speech", S.speech.load(), 0);
    f("agc", S.agc_db.load());
    f("total", S.total_gain_db.load());
    f("exp", S.exp_db.load());
    f("lim", S.lim_db.load());
    f("out_peak", S.out_peak_dbfs.load());
    f("interlock", S.interlock_db.load());
    f("suspect", S.howl_suspect.load(), 0);
    f("events", S.howl_events.load(), 0);
    f("runaways", S.runaway_events.load(), 0);
    f("dsp_resets", S.dsp_resets.load(), 0);
    f("rec", S.recording.load(), 0);
    f("rec_s", S.rec_seconds.load(), 0);
    f("rec_drop", S.rec_dropped.load(), 0);
    j += "\"dyn\":[";
    bool first = true;
    for (int i = 0; i < kMaxDynNotches; ++i) {
        float g = P.dyn[i].gain_db.load();
        if (g > -0.01f) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"slot\":" + std::to_string(i) + ",\"hz\":" + num(P.dyn[i].hz.load(), 0) + ",\"db\":" + num(g, 1) + "}";
    }
    j += "],\"fixed\":[";
    int nf = P.fixed_count.load();
    for (int i = 0; i < nf; ++i) {
        if (i) j += ",";
        j += "{\"hz\":" + num(P.fixed[i].hz.load(), 0) + ",\"db\":" + num(P.fixed[i].gain_db.load(), 1) +
             ",\"q\":" + num(P.fixed[i].q.load(), 1) + "}";
    }
    j += "],\"log\":[";
    auto ev = D.recent_events();
    size_t start = ev.size() > 8 ? ev.size() - 8 : 0;
    for (size_t i = start; i < ev.size(); ++i) {
        if (i > start) j += ",";
        j += "{\"t\":" + num(ev[i].t_s, 1) + ",\"hz\":" + num(ev[i].hz, 0) + ",\"db\":" + num(ev[i].gain_db, 1) +
             ",\"deep\":" + (ev[i].deepened ? "1" : "0") + ",\"ext\":" + (ev[i].external ? "1" : "0") + "}";
    }
    j += "],\"p\":{";
    for (int i = 0; i < P_COUNT; ++i) {
        if (i) j += ",";
        j += "\"" + std::string(kParamInfo[i].name) + "\":" + num(P.get((ParamId)i), 3);
    }
    j += "}}";
    return j;
}

std::string WebServer::params_json() {
    std::string j = "{";
    for (int i = 0; i < P_COUNT; ++i) {
        const ParamInfo& pi = kParamInfo[i];
        if (i) j += ",";
        j += "\"" + std::string(pi.name) + "\":{\"lo\":" + num(pi.lo, 3) + ",\"hi\":" + num(pi.hi, 3) +
             ",\"def\":" + num(pi.def, 3) + ",\"help\":\"" + json_escape(pi.help) + "\"}";
    }
    return j + "}";
}

}  // namespace imp
