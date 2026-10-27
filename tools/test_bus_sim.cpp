// End-to-end offline test of the Pico 2 W panel-bus firmware logic.
//
// Runs the exact PIO instruction words from components/jacuzzi_panel/bus_pio.h in a small
// cycle-by-cycle PIO simulator (20 MHz) against a J1-A Logic 8 recording, with a model of
// core 1 polling the FIFOs and feeding components/jacuzzi_panel/bus_assembler.h.
//
//   c++ -std=c++17 -O2 -I components/jacuzzi_panel tools/test_bus_sim.cpp -o /tmp/bussim
//   /tmp/bussim captures/31-j1a-buttons/digital.csv                 # read-only
//   /tmp/bussim captures/31-j1a-buttons/digital.csv 4               # + presses of scan pos 4
//
// Env: PRESS_EVERY_S (default 2) - press interval; DROP_PCT - drop that % of capture words
// at core 1; GLITCH_PER_S - inject that many random 0.1-0.8 us clock glitches per second;
// POLL_NS (default 500) - core-1 polling period; LIMIT_S - simulate only the first N s.
// Exit code 1 if any drive window is misplaced or frames disagree with the recording.
#include "bus_assembler.h"
#include "bus_pio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace jacuzzi_bus;

// ------------------------------------------------------------------ PIO simulator
struct SimSM {
  const uint16_t *prog{};
  int len{}, wrap_target{}, wrap{};
  int in_base{2}, jmp_pin{2}, set_base{4};
  size_t rx_cap{4};
  int pc{0}, delay_left{0};
  uint32_t x{0}, y{0}, isr{0}, osr{0};
  std::deque<uint32_t> rx, tx;
  bool pin_out{false}, pin_dir{false};
  uint64_t rx_dropped{0};

  void reset() {
    pc = 0; delay_left = 0; x = y = isr = osr = 0; rx.clear(); tx.clear(); pin_out = pin_dir = false;
  }
  void step(uint32_t gpio) {
    if (delay_left > 0) { delay_left--; return; }
    const uint16_t ins = prog[pc];
    int next = (pc == wrap) ? wrap_target : pc + 1;
    bool stall = false;
    switch (ins >> 13) {
      case 0: {  // JMP
        const int cond = (ins >> 5) & 7, addr = ins & 31;
        bool take;
        switch (cond) {
          case 0: take = true; break;
          case 1: take = (x == 0); break;
          case 2: take = (x != 0); x--; break;
          case 4: take = (y != 0); y--; break;
          case 6: take = (gpio >> jmp_pin) & 1; break;
          default: std::fprintf(stderr, "unsupported jmp cond %d\n", cond); std::exit(3);
        }
        if (take) next = addr;
        break;
      }
      case 1: {  // WAIT
        const int pol = (ins >> 7) & 1, src = (ins >> 5) & 3, idx = ins & 31;
        int level;
        if (src == 0) level = (gpio >> idx) & 1;
        else if (src == 1) level = (gpio >> (in_base + idx)) & 1;
        else { std::fprintf(stderr, "unsupported wait src %d\n", src); std::exit(3); }
        if (level != pol) stall = true;
        break;
      }
      case 2: {  // IN (shift left)
        const int src = (ins >> 5) & 7; int n = ins & 31; if (n == 0) n = 32;
        uint32_t v;
        switch (src) {
          case 0: v = gpio >> in_base; break;
          case 1: v = x; break;
          case 2: v = y; break;
          case 3: v = 0; break;
          default: std::fprintf(stderr, "unsupported in src %d\n", src); std::exit(3);
        }
        const uint32_t mask = n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
        isr = n == 32 ? v : ((isr << n) | (v & mask));
        break;
      }
      case 3: {  // OUT (shift right)
        const int dst = (ins >> 5) & 7; int n = ins & 31; if (n == 0) n = 32;
        const uint32_t v = n == 32 ? osr : (osr & ((1u << n) - 1));
        osr = n == 32 ? 0 : (osr >> n);
        if (dst == 1) x = v; else if (dst == 2) y = v;
        else { std::fprintf(stderr, "unsupported out dst %d\n", dst); std::exit(3); }
        break;
      }
      case 4: {
        const bool block = (ins >> 5) & 1;
        if ((ins >> 7) & 1) {  // PULL
          if (tx.empty()) { if (block) stall = true; else osr = x; }
          else { osr = tx.front(); tx.pop_front(); }
        } else {  // PUSH
          if (rx.size() >= rx_cap) { if (block) stall = true; else { rx_dropped++; isr = 0; } }
          else { rx.push_back(isr); isr = 0; }
        }
        break;
      }
      case 5: {  // MOV
        const int dst = (ins >> 5) & 7, op = (ins >> 3) & 3, src = ins & 7;
        uint32_t v;
        switch (src) { case 1: v = x; break; case 2: v = y; break; case 3: v = 0; break;
          default: std::fprintf(stderr, "unsupported mov src %d\n", src); std::exit(3); }
        if (op == 1) v = ~v;
        if (dst == 1) x = v; else if (dst == 2) y = v;
        else { std::fprintf(stderr, "unsupported mov dst %d\n", dst); std::exit(3); }
        break;
      }
      case 7: {  // SET
        const int dst = (ins >> 5) & 7, d = ins & 31;
        if (dst == 0) pin_out = d & 1; else if (dst == 4) pin_dir = d & 1;
        else if (dst == 1) x = d; else if (dst == 2) y = d;
        else { std::fprintf(stderr, "unsupported set dst %d\n", dst); std::exit(3); }
        break;
      }
      default: std::fprintf(stderr, "unsupported opcode %04x\n", ins); std::exit(3);
    }
    if (!stall) { pc = next; delay_left = (ins >> 8) & 31; }  // no side-set: all 5 bits are delay
  }
};

// ------------------------------------------------------------------ recording
struct Row { double t; uint32_t gpio; };  // gpio: bit2 = clock (J1-A 6), bit3 = data (J1-A 5)

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s digital.csv [scan_pos]\n", argv[0]); return 2; }
  const int press_pos = argc > 2 ? std::atoi(argv[2]) : 0;
  auto envd = [](const char *k, double d) { const char *v = std::getenv(k); return v ? std::atof(v) : d; };
  const double press_every = envd("PRESS_EVERY_S", 2.0), drop_pct = envd("DROP_PCT", 0);
  const double glitch_per_s = envd("GLITCH_PER_S", 0), limit_s = envd("LIMIT_S", 1e9);
  const uint64_t poll_cycles = static_cast<uint64_t>(envd("POLL_NS", 500) / 50.0 + 0.5);
  // REPLY_BUFFER=diode: the firmware forces GP4's output enable on, so the level is just the
  // PIO's pins latch (pindirs ignored). Default: GP4 drives only when pindirs = 1 and pins = 1.
  const char *rb = std::getenv("REPLY_BUFFER");
  const bool diode = rb != nullptr && std::string(rb) == "diode";

  FILE *f = std::fopen(argv[1], "r");
  if (!f) { std::perror(argv[1]); return 1; }
  char line[256];
  std::fgets(line, sizeof line, f);
  std::vector<Row> rows;
  while (std::fgets(line, sizeof line, f)) {
    double t; int c[8];
    if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d,%d,%d", &t, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]) != 9) continue;
    rows.push_back({t, (uint32_t) ((c[5] ? 1u : 0u) << 2 | (c[4] ? 1u : 0u) << 3)});
  }
  std::fclose(f);
  if (rows.empty()) return 1;
  const double t0 = rows.front().t, t_end = std::min(rows.back().t, t0 + limit_s);

  // Ground truth straight from the recording: per frame its bits and each wide pulse's [rise, fall].
  struct TruthFrame { uint32_t bits; double wide_rise[7], wide_fall[7]; double end; };
  std::vector<TruthFrame> truth;
  {
    int sh = 0, wi = 0; uint32_t bits = 0; double rise = 0, last_fall = -1; bool hi = false, dat = false;
    TruthFrame cur{};
    for (const Row &r : rows) {
      if (r.t > t_end) break;
      const bool clk = r.gpio & 4;
      if (clk && !hi) { hi = true; rise = r.t; dat = r.gpio & 8; if (last_fall >= 0 && r.t - last_fall > 1e-3) { sh = wi = 0; bits = 0; } }
      else if (!clk && hi) {
        hi = false; const double w = r.t - rise;
        if (w < 1e-6) continue;
        last_fall = r.t;
        if (w <= 4e-6) { if (wi) { sh = wi = 0; bits = 0; } bits = bits << 1 | dat; sh++; }
        else { if (sh == 24 && wi < 7) { cur.wide_rise[wi] = rise; cur.wide_fall[wi] = r.t; }
               wi++; if (wi == 7 && sh == 24) { cur.bits = bits; cur.end = r.t; truth.push_back(cur); } }
      }
    }
  }

  // Simulation.
  SimSM cap, prs;
  cap.prog = CAPTURE_PROGRAM; cap.len = sizeof CAPTURE_PROGRAM / 2;
  cap.wrap_target = CAPTURE_WRAP_TARGET; cap.wrap = CAPTURE_WRAP;
  cap.rx_cap = 8;  // RX FIFO joined
  prs.prog = PRESS_PROGRAM; prs.len = sizeof PRESS_PROGRAM / 2;
  prs.wrap_target = PRESS_WRAP_TARGET; prs.wrap = PRESS_WRAP;
  BusAssembler as;
  as.set_enabled(press_pos != 0);
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> U(0, 1);

  size_t ri = 0;
  uint32_t gpio = rows[0].gpio;
  double next_press = t0 + 1.0;
  bool drive = false;
  double drive_start = 0;
  int windows = 0, misplaced = 0, early = 0, cut = 0, aborts = 0;
  double last_abort = -1;
  double lat_max = 0, lat_sum = 0, hold_min = 1, hold_max = 0;
  std::vector<uint32_t> decoded;
  double glitch_until = -1;
  const uint64_t n_cycles = static_cast<uint64_t>((t_end - t0) * SM_CLOCK_HZ);

  for (uint64_t cyc = 0; cyc < n_cycles; cyc++) {
    const double t = t0 + cyc / (double) SM_CLOCK_HZ;
    while (ri + 1 < rows.size() && rows[ri + 1].t <= t) gpio = rows[++ri].gpio;
    uint32_t g = gpio;
    if (glitch_per_s > 0) {
      if (t > glitch_until && U(rng) < glitch_per_s / SM_CLOCK_HZ) glitch_until = t + (0.1 + 0.7 * U(rng)) * 1e-6;
      if (t <= glitch_until) g ^= 4;
    }
    cap.step(g);
    prs.step(g);

    const bool d = diode ? prs.pin_out : (prs.pin_dir && prs.pin_out);
    if (d && !drive) { drive = true; drive_start = t; }
    if (!d && drive) {
      drive = false; windows++;
      bool ok = false, ok_early = false; double lat = 0;
      for (const TruthFrame &tf : truth) {
        const double r = tf.wide_rise[press_pos - 1], fl = tf.wide_fall[press_pos - 1];
        // Like the real panel, the drive must last through the pulse and its ~4 us low gap:
        // release 3.5-5.5 us after the falling edge (the panel releases at 4.7-4.8 us).
        const bool end_ok = t >= fl + 3.5e-6 && t <= fl + 5.5e-6;
        if (drive_start >= r - 1e-9 && drive_start < fl && end_ok) { ok = true; lat = drive_start - r; hold_max = std::max(hold_max, t - fl); hold_min = std::min(hold_min, t - fl); break; }
        // A glitch just before the target pulse can start the drive up to ~1 us early; it
        // then covers the correct pulse. Counted separately, not as misplaced.
        if (drive_start >= r - 1.2e-6 && drive_start < r && end_ok) { ok_early = true; break; }
      }
      // Started on the right pulse but ended early because core 1 aborted (a glitch broke the
      // frame): safe, the press just doesn't count. Not misplaced.
      bool ok_cut = false;
      if (!ok && !ok_early && last_abort >= drive_start && t - last_abort < 0.2e-6)
        for (const TruthFrame &tf : truth) {
          const double r = tf.wide_rise[press_pos - 1], fl = tf.wide_fall[press_pos - 1];
          if (drive_start >= r - 1.2e-6 && drive_start < fl && t <= fl + 5.5e-6) { ok_cut = true; break; }
        }
      if (ok_cut) cut++;
      else if (ok_early) { early++; std::printf("  early start (glitch) at %.6fs, covers the correct pulse\n", drive_start); }
      else if (!ok) {
        misplaced++;
        double best = 1e9; for (const TruthFrame &tf : truth) { const double dd = drive_start - tf.wide_rise[press_pos - 1]; if (std::fabs(dd) < std::fabs(best)) best = dd; }
        std::printf("  MISPLACED drive at %.6fs (+%.2f us), %.2f us from the target rise\n", drive_start, (t - drive_start) * 1e6, best * 1e6);
      }
      else { lat_max = std::max(lat_max, lat); lat_sum += lat; }
    }

    if (cyc % poll_cycles == 0) {  // core 1
      const uint32_t now_us = static_cast<uint32_t>((t - t0) * 1e6);
      while (!prs.rx.empty()) { prs.rx.pop_front(); as.on_injected(); }
      while (!cap.rx.empty()) {
        const uint32_t w = cap.rx.front(); cap.rx.pop_front();
        if (drop_pct > 0 && U(rng) * 100 < drop_pct) continue;
        const bool fresh = cap.rx.empty();
        const BusAssembler::Action a = as.on_pulse(capture_width_x10(w), capture_data(w), now_us, fresh);
        if (a.abort) { prs.reset(); last_abort = t; aborts++; }
        if (a.arm) prs.tx.push_back(a.skip);
        if (as.frame_seq() != decoded.size()) decoded.push_back(as.frame());
      }
      if (press_pos && t >= next_press) { if (as.arm_press(press_pos, 3)) next_press = t + press_every; }
    }
  }

  // Frames: every decoded frame must be one of the recording's frames, in order.
  size_t ti = 0, matched = 0;
  for (uint32_t fr : decoded) {
    while (ti < truth.size() && truth[ti].bits != fr) ti++;
    if (ti < truth.size()) { matched++; ti++; }
  }
  const bool frames_ok = matched == decoded.size();
  std::printf("frames: recording=%zu decoded=%zu (%.1f%%) matched=%s  bad=%u refusals=%u  capture_fifo_overflow=%llu\n",
              truth.size(), decoded.size(), truth.empty() ? 0.0 : 100.0 * decoded.size() / truth.size(),
              frames_ok ? "yes" : "NO", as.bad(), as.refusals(), (unsigned long long) cap.rx_dropped);
  if (press_pos)
    std::printf("presses: drive_windows=%d misplaced=%d early_but_correct=%d cut_by_abort=%d (aborts %d)  drive latency after rise: avg %.0f ns, max %.0f ns\n",
                windows, misplaced, early, cut, aborts, windows - misplaced - early - cut ? 1e9 * lat_sum / (windows - misplaced - early - cut) : 0.0,
                1e9 * lat_max);
  if (press_pos && hold_max > 0)
    std::printf("         release after the falling edge: %.2f-%.2f us (real panel: 4.7-4.8 us)\n", 1e6 * hold_min, 1e6 * hold_max);
  return (misplaced || !frames_ok) ? 1 : 0;
}
