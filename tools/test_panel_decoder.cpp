// Replay a J1-A Logic 8 capture (digital.csv, CHn = J1-A pin n+1) through
// components/jacuzzi_panel/panel_decoder.h, as the Pico 2 W would see it
// (timestamps truncated to whole microseconds like time_us_32()).
//
//   c++ -std=c++17 -O2 -I components/jacuzzi_panel tools/test_panel_decoder.cpp -o /tmp/tpd
//   /tmp/tpd captures/31-j1a-buttons/digital.csv [press_pos press_at_s]
//
// Prints every frame change, then (optionally) arms a press at `press_at_s` and
// checks that each drive window lands exactly on wide pulse `press_pos` of a frame.
#include "panel_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using jacuzzi_bus::PanelDecoder;

struct Edge { uint32_t t; bool rise; bool data; };

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s digital.csv [press_pos press_at_s]\n", argv[0]); return 2; }
  FILE *f = std::fopen(argv[1], "r");
  if (!f) { std::perror(argv[1]); return 1; }
  const int press_pos = argc > 3 ? std::atoi(argv[2]) : 0;
  const double press_at = argc > 3 ? std::atof(argv[3]) : 0;

  char line[256];
  std::fgets(line, sizeof line, f);  // header
  std::vector<Edge> edges;
  int prev_clk = -1, data = 0;
  while (std::fgets(line, sizeof line, f)) {
    double t; int c[8];
    if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d,%d,%d", &t, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]) != 9) continue;
    data = c[4];  // J1-A pin 5
    const int clk = c[5];  // J1-A pin 6
    if (prev_clk >= 0 && clk != prev_clk)
      edges.push_back({static_cast<uint32_t>(std::floor(t * 1e6)), clk == 1, data == 1});
    prev_clk = clk;
  }
  std::fclose(f);

  // Ground truth from the full stream: rise/fall times of each frame's wide pulse k.
  std::vector<std::pair<uint32_t, uint32_t>> truth;
  {
    int sh = 0, wi = 0; uint32_t r = 0, lf = 0;
    for (const Edge &e : edges) {
      if (e.rise) { if (e.t - lf > 1000) { sh = 0; wi = 0; } r = e.t; continue; }
      const uint32_t w = e.t - r;
      if (w < 1) continue;
      lf = e.t;
      if (w <= 4) { if (wi) { sh = 0; wi = 0; } sh++; }
      else { wi++; if (sh == 24 && wi == press_pos) truth.push_back({r, e.t}); }
    }
  }

  // DROP_PCT=n randomly drops n% of clock edges (simulates missed interrupts).
  const char *dp = std::getenv("DROP_PCT");
  if (dp) {
    const int pct = std::atoi(dp);
    std::srand(12345);
    std::vector<Edge> kept;
    for (const Edge &e : edges) if (std::rand() % 100 >= pct) kept.push_back(e);
    edges.swap(kept);
  }

  PanelDecoder dec;
  dec.set_enabled(press_pos != 0);
  uint32_t seen_seq = 0, last_frame = 0xFFFFFFFF;
  bool armed = false;
  uint32_t drive_start = 0;
  int windows = 0, misplaced = 0;

  for (const Edge &e : edges) {
    if (press_pos && !armed && e.t / 1e6 >= press_at) armed = dec.arm_press(press_pos, 3);
    PanelDecoder::Drive d = e.rise ? dec.on_rise(e.t, e.data) : dec.on_fall(e.t);
    if (d == PanelDecoder::Drive::START) drive_start = e.t;
    if (d == PanelDecoder::Drive::STOP) {
      windows++;
      // The window must lie inside a real wide pulse `press_pos` of a real frame.
      const uint32_t w = e.t - drive_start;
      bool ok = false;
      for (auto &tp : truth) if (drive_start >= tp.first && e.t <= tp.second + 1) { ok = true; break; }
      if (!ok) misplaced++;
      std::printf("  drive %.6fs +%uus%s\n", drive_start / 1e6, w, ok ? "" : "  <-- MISPLACED");
    }
    if (dec.frame_seq() != seen_seq) {
      seen_seq = dec.frame_seq();
      const uint32_t fr = dec.frame();
      if (fr != last_frame) {
        last_frame = fr;
        const uint32_t s = fr & 0x7FFFFF;  // slots 1..23, slot k at bit 23-k
        auto slot = [&](int k) { return (s >> (23 - k)) & 1; };
        std::printf("%8.3fs frame %06X heat=%u slot5=%u j1=%u j2=%u disp=%u%u%u\n", e.t / 1e6, fr, slot(4), slot(5), slot(7),
                    slot(11), (s >> 8) & 0xF, (s >> 4) & 0xF, s & 0xF);
      }
    }
  }
  std::printf("edges=%zu frames=%u bad_groups=%u timing_rejects=%u", edges.size(), dec.frame_seq(), dec.bad_groups(),
              dec.timing_rejects());
  if (press_pos) std::printf(" drive_windows=%d misplaced=%d press_done=%s", windows, misplaced, dec.press_active() ? "no" : "yes");
  std::printf("\n");
  return misplaced ? 1 : 0;
}
