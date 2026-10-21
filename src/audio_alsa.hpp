// audio_alsa.hpp - full-duplex ALSA loop (capture -> Engine -> playback).
#pragma once
#include <atomic>
#include <string>

#include "dsp.hpp"
#include "params.hpp"

namespace imp {

// Runs until `running` goes false (returns 0) or the device fails (returns <0,
// e.g. the interface was unplugged). The caller retries.
int run_alsa(const StartupSettings& s, Engine& eng, Status& S, std::atomic<bool>& running,
             std::string& err);

// Prints the sound cards ALSA can see (to find the right `device`).
void list_alsa_devices();

}  // namespace imp
