// web.hpp - tiny HTTP server for the phone control page.
//   GET  /             control page
//   GET  /api/status   meters, notches, recent feedback events, all parameter values
//   GET  /api/params   parameter ranges and help text
//   POST /api/set      body: name=value[&name=value...]
//   POST /api/action   body: do=save|clear_dynamic|commit_notches|clear_fixed|
//                            mark_ring|set_reference|start_ringout|cancel_ringout
// No authentication: keep the Pi on its own hotspot or a trusted network.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "detector.hpp"
#include "params.hpp"

namespace imp {

class Recorder;

class WebServer {
public:
    WebServer(Params& p, Status& s, FeedbackDetector& d, StartupSettings& st, std::string config_path)
        : P(p), S(s), D(d), ST(st), config_path_(std::move(config_path)) {}
    ~WebServer() { stop(); }
    bool start(std::string& err);
    void stop();

    // Shared by the web handlers and the command line.
    std::string do_action(const std::string& what, bool& ok);

private:
    void loop();
    void handle(int fd);
    std::string status_json();
    std::string params_json();

    Params& P;
    Status& S;
    FeedbackDetector& D;
    StartupSettings& ST;
    std::string config_path_;
    int listen_fd_ = -1;
    std::atomic<bool> run_{false};
    std::atomic<int> active_{0};
    std::thread th_;
    std::mutex action_mu_;
    float pre_ring_gain_ = 0;
    bool have_pre_ring_ = false;
};

}  // namespace imp
