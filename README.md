# improvdsp

A speech-reinforcement processor for one floor (boundary) mic and two powered
speakers, running on a Raspberry Pi 5 with a Behringer UMC22 USB interface.
It's built for unmiked improv in a hard, echoey room. You control it from a
phone over Wi-Fi.

```
mic ─► high-pass 120 Hz ─► rung-out notches ─► automatic feedback notches ─► frequency shift (+4 Hz)
    ─► gain (your fader + speech-gated AGC, capped by a ceiling, minus the safety drop) × room expander
    ─► mute / fade ─┬─► LEFT  out: front speaker (delay, trim, peak limiter)
                    └─► RIGHT out: fill speaker  (delay, trim, peak limiter)
side-chain: 2048-point FFT every 10.7 ms → feedback detector → notches + safety drop
```

- **Frequency shifter.** Every frequency is moved up 4 Hz, so feedback can't
  build on one pitch. This is the biggest win in a reverberant room: about
  +6 dB of gain before feedback in simulation. Voices sound normal; it would
  make sung music slightly out of tune.
- **Automatic feedback notches.** These catch a ring as it starts: a peak that
  is far above the rest of the spectrum, narrow, has no harmonics (voices
  do), and lasts about 75 ms without fading.
  - About 0.3 s after placing a notch, the detector checks whether the tone
    died away. If it didn't, the tone was something outside the loop (a fridge
    whine, a sustained note), so the notch is undone and that pitch is ignored.
- **Safety drop.** Each catch lowers the gain by 3 dB, up to 9 dB, for 30 s,
  then it recovers slowly.
- **Last-resort guard.** It acts if the output limiter keeps working for most
  of a second, or if catches keep coming after the safety drop is already at
  its maximum. It takes 3 dB off every half second, up to 30 dB, holds for
  60 s, then recovers at 0.1 dB/s. It stays active during ring-out.
- **Built-in protections.**
  - Each speaker output has its own peak limiter at −3 dBFS, after its trim.
  - Gain can rise at most 20 dB per second, so no setting change can jump the
    level. Cuts happen instantly.
  - After an audio dropout the output fades back in over 20 ms; after a USB
    replug or a restart, over 1 s.
  - A bad value such as `nan` is rejected, and if anything ever goes
    non-finite, the path resets itself instead of going silent.
- **Speech-gated AGC.** It raises quiet performers by up to +6 dB and lowers
  loud ones. Gain only goes up while someone is talking, and it can never
  exceed the ceiling you set during ring-out.
- **Room expander.** Turns the system down 10 dB or less when nobody is
  talking, so the PA doesn't amplify the crowd or creep toward feedback in the
  pauses.
- **Two outputs.** The right output can be delayed to feed a fill speaker
  partway down the room.

## What to expect

These are measured in a closed-loop simulation, not in your room. The model
is a speaker → reverberant room → mic loop with an 8 ms audio buffer plus
1.5 ms of converter delay, using real speech recordings. "0 dB" is the gain
where an unprocessed system starts to feed back.

| Test | Result |
|---|---|
| No processing | feeds back above 0 to +1 dB |
| Frequency shifter only | stable up to +6 dB |
| Automatic notches only | stable up to +2 dB ("a couple of dB", as the literature says) |
| Shifter + notches, 9 simulated rooms (reverb time 0.8 / 1.3 / 2.0 s) | stable up to **+5 to +7 dB** |
| Everything on, gain pushed to +8 … +14 dB | settles after 1–2 catches (3–6 dB safety drop) |
| Gain pushed to +16 … +34 dB (no ring-out done, or the mic kicked) | settles: the safety drop and last-resort guard take 15–27 dB off |
| False triggers: 57 s of speech (3 talkers), 16 s of singing, 40 s of clattering dishes | **0** |
| Steady 2.2 kHz whine under speech | caught, then recognised as an outside tone and undone |
| AGC: performers spread over 18 dB | output spread about 7 dB, steady within ±1 dB for one talker |
| Latency | 8 ms buffering at the defaults (period 128, prefill 2), plus about 1–2 ms in the converters |

A real room changes as people move, so expect somewhat less. Turn on
recording at a show and you can check and tune against your actual room (see
**Tuning with recordings** below).

## Parts

- Raspberry Pi 5 with its 27 W USB-C power supply, and a microSD card
- Behringer UMC22 (about $53). It supplies the 48 V phantom power and the mic preamp.
- Two cables, 1/4" TRS (balanced) to XLR male. One must reach the fill speaker if you use one.
- Your Shure boundary mic, an XLR cable, and the two Alto TX308s
- Optional: a USB stick for recordings
- A phone with a browser

## Wiring and knob settings

```
Shure boundary mic ──XLR──► UMC22 input 1 (front combo jack)
                            UMC22 ── USB ── Raspberry Pi 5
UMC22 OUT L ──TRS→XLR──► TX308 #1 (front)  XLR in, volume knob in the LINE (outer) band
UMC22 OUT R ──TRS→XLR──► TX308 #2 (fill)   XLR in, volume knob in the LINE (outer) band
```

- **UMC22**
  - Rear **+48V switch ON**.
  - **Direct Monitor OFF**, so no unprocessed mic reaches the speakers.
  - **Output** knob at about 2 o'clock.
  - **Input gain**: set at sound check (see below).
- **TX308s**: start at the "0 dB" mark of the Line band. Don't use the TX308
  link output for the second speaker; give each speaker its own UMC22 output so
  the fill can be delayed. If you run both speakers up front, set Fill delay to
  0 and use the left/right outputs anyway.
- **Turn-on order**: Pi and UMC22 first, speakers last. Turn the speakers off
  first. The program also fades in over 1 s and has a mute button.

## Install on the Pi

1. Use **Raspberry Pi Imager** to flash **Raspberry Pi OS Lite (64-bit)**. In
   the Imager settings, set a hostname (for example `improv`), a user and
   password, your home Wi-Fi, and turn on SSH.
   - The Desktop version runs PipeWire, which grabs the UMC22. You'd see
     "Device or resource busy". Use Lite.
2. Copy this folder to the Pi, for example `scp -r improvdsp improv.local:`,
   then run:
   ```
   cd improvdsp
   ./deploy/install.sh
   ```
   This installs the compiler and ALSA headers, builds the program, runs the
   unit tests, writes the default settings to `/boot/firmware/improvdsp.conf`,
   and starts the `improvdsp` service at every boot.
3. Plug in the UMC22. `improvdsp --list-devices` should list
   `id=CODEC ... USB Audio CODEC`. If yours shows a different id, edit
   `device = hw:<id>` in `/boot/firmware/improvdsp.conf` and run
   `sudo systemctl restart improvdsp`.
4. Open the control page at `http://improv.local:8080/`, or
   `http://<pi-ip>:8080/`. Logs: `journalctl -u improvdsp -f`.

### Phone control at the brewery (Pi as its own Wi-Fi hotspot)

```
sudo apt install dnsmasq-base
sudo nmcli device wifi hotspot ifname wlan0 ssid "ImprovDSP" password "choose-a-password"
nmcli con show                                  # note the hotspot's connection name
sudo nmcli con modify "<that name>" connection.autoconnect yes
```

- Join "ImprovDSP" from your phone and open `http://10.42.0.1:8080/`.
- The control page has **no password**. The Wi-Fi password is what keeps the
  audience off it.
- To reach the internet again for updates, run
  `sudo nmcli con up "<your home wifi>"`.

### Optional: protect the SD card from power cuts

Run `sudo raspi-config`, then **Performance Options → Overlay File System**.

- Enable the overlay.
- When it asks whether to write-protect the **boot** partition, answer **No**.
  The Save button writes `/boot/firmware/improvdsp.conf`, so that partition
  must stay writable.
- With the overlay on, anything written elsewhere is lost at reboot. Point
  `record_dir` at a USB stick (for example `/media/usb/recordings`) if you
  want recordings.

## Show-day procedure

1. **Placement.** This matters more than any processing.
   - **Mic**: on the floor at the downstage edge, pickup facing the
     performers.
   - **Front speakers**: on stands, well in front of the mic line, horns
     above head height, aimed down the length of the room and tilted slightly
     down.
   - **Fill speaker**, if used: partway down the room, aimed at the back.
   - Keep scenes downstage. Every halving of the performer-to-mic distance is
     worth 6 dB.
2. **Mic level.** Have someone shout from the stage edge. Set the UMC22
   **input gain** so the "Mic in" meter peaks near its white line (−12 dBFS).
3. **Ring-out.** Open **Ring-out** on the page and press **Start ring-out**.
   1. Someone talks steadily from the farthest spot on stage.
   2. Press **+0.5** repeatedly. When it starts to ring, the automatic notches
      catch the first few rings.
   3. When it rings again and won't settle, press **Mark ring point**. This
      sets **Ceiling = ring point − 6 dB** and **Gain = ceiling − AGC max
      boost** (6 dB), leaving room for the AGC to lift quiet performers. It also keeps the
      notches it found as fixed ones.
   4. Press **Save settings**.
   - Do this with the speakers and mic exactly where they'll be for the show.
     A full room rings less than an empty one, so a sound-check ring-out is
     conservative.
4. **AGC reference.** Have a performer talk at a normal level from mid-stage,
   then press **Use the current voice as reference**. Performers quieter than
   this get lifted, up to the AGC max boost; louder ones are turned down.
5. **Fill speaker.**
   - Set **Fill delay** to its distance from the stage in feet ÷ 1.13 (see the
     table below). The processor's own ~10 ms latency already adds the extra
     few ms that keep voices sounding like they come from the stage.
   - Set **Fill trim** a few dB below the front speaker.
   - Check by ear while standing near the fill. Voices should still seem to
     come from the stage, just clearer. If they seem to come from the fill,
     turn it down or add a little delay. If you hear an echo, the delay is too
     long.
6. **Save settings.** During the show, watch the green dot, the **FEEDBACK?**
   chip and the Events count. The big **MUTE** button is at the top.

| Fill distance from stage | 20 ft | 25 ft | 30 ft | 35 ft | 40 ft |
|---|---|---|---|---|---|
| Fill delay | 18 ms | 22 ms | 27 ms | 31 ms | 35 ms |

## Recording

Turn on **Record** on the phone page.
- **Format:** 2-channel WAV, 16-bit, 48 kHz. The left channel is the raw mic,
  the right is the processed front-speaker signal. It takes about 11.5 MB per
  minute.
- **Split into parts:** a new file starts every 10 minutes
  (`record_segment_min`; 0 = one file), with no gap:
  `improv-20261010-203015-part01.wav`, `-part02.wav`, …
- **Saved every 5 s:** every 5 seconds the header is updated and the data is
  forced onto the card or stick, so a crash or power cut loses at most about
  the last 5 seconds. Earlier parts are already closed and safe.
- **Repairing a cut-off file:** run `improvdsp --repair cut.wav fixed.wav`. It
  rewrites the header to cover all the audio actually on disk. `--offline`
  also reads cut-off files directly. On a computer without improvdsp,
  `ffmpeg -ignore_length 1 -i cut.wav fixed.wav` does the same.
- **With the read-only SD card protection on**, record to a USB stick (set
  `record_dir`). Otherwise recordings live only in memory and vanish at power
  off.
- **To copy recordings off:** `scp improv.local:/var/lib/improvdsp/recordings/*.wav .`

## Tuning with recordings

Turn **Record** on during a show. Each WAV has the **raw mic on the left** and
the **processed output on the right**. Copy it to any Linux machine or the Pi
and replay it through the processor:

```
improvdsp --offline show.wav out.wav --channel 0
improvdsp --offline show.wav out.wav --channel 0 --set afs_phpr_db=34 --set afs_persist=9
```

It prints every feedback catch with its time, frequency and level, so you can
see whether a catch was real or a false alarm. Adjust the thresholds until
only real feedback gets caught. Note that a recording has no feedback loop, so
this tests false alarms, not catching. `improvdsp --params` lists every
setting with its range.

## Main settings

| Setting | Default | What it does |
|---|---|---|
| `gain_db` | 0 | Your fader (digital). Real mic gain comes from the UMC22 knob. |
| `ceiling_db` | 12 | Gain + AGC can never go above this. Ring-out sets it to ring point − 6. |
| `agc_max_boost_db` / `agc_max_cut_db` | 6 / 12 | How far the AGC may raise quiet performers or lower loud ones. |
| `agc_ref_dbfs` | −38 | Voice level that gets no AGC change. Set it with the button. |
| `shift_hz` | 4 | Frequency shift. 3–5 Hz is typical; above about 8 Hz voices start to sound odd. |
| `afs_slots` | 8 | Number of automatic notches. The oldest is reused when full. |
| `afs_phpr_db` / `afs_persist` | 30 / 7 | Detector strictness. Raise them if it catches things that aren't feedback. |
| `afs_interlock_db` / `_max_db` / `afs_hold_s` | 3 / 9 / 30 | Safety drop per catch, its maximum, and how long it holds. |
| `exp_range_db` | 10 | How far the expander turns the room down between lines. |
| `hpf_hz` | 120 | Removes footsteps, rumble and boom. |
| `limiter_dbfs` | −3 | Peak limiter on each speaker output (after its trim). |
| `period` / `prefill` | 128 / 2 | Audio buffer. Latency ≈ (prefill + 1) × period ÷ 48 kHz. Try `period = 64` for about 4 ms if you get no dropouts. |

## Troubleshooting

- **Red dot / "no audio device".**
  - Run `improvdsp --list-devices` and check the `device` line in the config.
  - "Device or resource busy" means something else (PipeWire) holds the
    UMC22.
  - The program retries every 2 s, so unplugging and replugging is fine.
- **Clicks, or the xruns count climbing.** Set `period = 256`, or `prefill = 3`.
  Both add latency.
- **Hum.**
  - Plug the Pi, UMC22 and speakers into the same power strip.
  - Use the balanced TRS cables.
  - Don't lift the safety ground on the speakers.
- **Warbly or chirpy sound** means you're near the limit with the shifter on.
  Lower the gain, or redo the ring-out.
- **Voices pumping up and down.** Lower `agc_max_boost_db`, or raise
  `speech_margin_db`.
- **First syllables clipped.** Lower `exp_range_db`, or turn the expander off.
- **Notches piling up during songs.** Raise `afs_phpr_db` and `afs_persist`.
  For musical improv you can also turn the shifter off for that scene: songs
  are where a 4 Hz shift would be audible.

## How it was tested

- `make test` runs unit tests:
  - The filters are within 0.1 dB of their design.
  - The shifter moves a 1 kHz tone to exactly 1004.00 Hz, with its mirror
    image 45–50 dB down.
  - The detector catches a steady tone but ignores a gliding voice-like
    signal.
  - An outside tone gets its notch undone, while feedback that dies once
    notched keeps its notch.
- `make sim` builds the closed-loop simulator in `tools/sim.cpp`. Example:
  ```
  ./sim --src speech.wav --sweep 0:10:1 --set agc_on=0 --set exp_on=0 --set afs_interlock_max_db=0 --set guard_on=0
  ```
  The room model is a speaker → room → mic loop with a reverberant tail,
  normalised so 0 dB is the raw feedback point. The `--set` options switch off
  the level-changing features so the shifter and notches are measured on their
  own.
- The live audio loop and control page were exercised through ALSA file
  devices. The output matched offline processing sample for sample, apart
  from the expected 256-frame buffer delay.
- An independent code review covered real-time safety, ALSA handling, the
  HTTP parser and failure modes. Its findings were fixed and have regression
  tests in `make test`.
- **Not yet done:** running on an actual Pi 5 with a UMC22. First checks once
  it's on the Pi:
  1. Run it for 30 minutes or more at the default `period = 128`,
     `prefill = 2`, with the page open and recording on, and watch the xruns
     count. If it climbs, use `prefill = 3` or `period = 256`.
     `journalctl -u improvdsp` also warns if there are 10 dropouts in a minute.
  2. Run `alsamixer -c CODEC` and make sure the UMC22's capture and playback
     levels are up and not muted.
  3. Pull and replug the USB cable while it's running. It should come back on
     its own within a few seconds, fading in.
  4. Do the first ring-out with the speakers at a modest level.
- After a restart it comes up **unmuted** with a 1 s fade-in. The rung-out
  (fixed) notches and saved settings persist; the automatic notches and any
  safety drop start fresh.

## Files

```
src/dsp.*        filters, frequency shifter, AGC, expander, limiter, delays (the audio path)
src/detector.*   feedback detector, notch manager, outside-tone check
src/audio_alsa.* full-duplex ALSA loop with dropout recovery and auto-reconnect
src/web.*        HTTP server; src/webui.hpp is the phone page
src/config.*     settings file; src/params.hpp lists every setting
src/recorder.hpp background WAV recorder
tools/           simulator and unit tests
deploy/          default config, systemd service, install script
```

The 90° phase network in the frequency shifter uses Olli Niemitalo's published
IIR coefficients. The feedback detector's features follow van Waterschoot &
Moonen, "Fifty Years of Acoustic Feedback Control" (Proc. IEEE, 2011).
