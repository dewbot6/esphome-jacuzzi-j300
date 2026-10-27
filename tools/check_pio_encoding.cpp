// Host check: the hand-encoded PIO words in components/jacuzzi_panel/bus_pio.h must equal
// the Pico SDK's pio_encode_*() output. Build (zsh/bash, from the repo root):
//   F=~/.platformio/packages/framework-arduinopico; SDK=$F/pico-sdk/src
//   c++ -std=c++17 -DPICO_RP2350=1 -DPICO_ON_DEVICE=0 -DPICO_NO_HARDWARE=1 -I tools/sdkhost \
//     -I components/jacuzzi_panel -I $F/include/rp2350/pico_base -I $SDK/host/pico_platform/include \
//     -I $SDK/rp2_common/hardware_pio/include -I $SDK/common/pico_base_headers/include \
//     -I $SDK/rp2350/hardware_regs/include -I $SDK/rp2350/hardware_structs/include \
//     -I $SDK/rp2_common/hardware_base/include -I $SDK/host/hardware_gpio/include \
//     -I $SDK/rp2_common/pico_platform_compiler/include tools/check_pio_encoding.cpp -o /tmp/pioenc && /tmp/pioenc
// (The firmware runs the same comparison at boot and refuses to start on a mismatch.)
#include <cstdio>
#include "bus_pio.h"
#define PARAM_ASSERTIONS_ENABLED_PIO_INSTRUCTIONS 0
#include "hardware/pio_instructions.h"
int main() {
  const unsigned nop = pio_encode_mov(pio_y, pio_y) | pio_encode_delay(19);
  const unsigned cap[] = {pio_encode_wait_pin(true,0), pio_encode_in(pio_pins,2), pio_encode_mov_not(pio_x,pio_null),
    pio_encode_jmp_pin(6), pio_encode_in(pio_x,30), pio_encode_push(false,false), pio_encode_jmp_x_dec(3), pio_encode_jmp(3)};
  const unsigned prs[] = {pio_encode_pull(false,true), pio_encode_out(pio_x,32), pio_encode_jmp_not_x(11),
    pio_encode_wait_pin(true,0), nop, pio_encode_jmp_pin(7), pio_encode_jmp(3), pio_encode_wait_pin(false,0), nop,
    pio_encode_jmp_pin(7), pio_encode_jmp_x_dec(2), pio_encode_wait_pin(true,0), pio_encode_set(pio_pins,1),
    pio_encode_set(pio_pindirs,1), nop, pio_encode_jmp_pin(19), pio_encode_set(pio_pindirs,0), pio_encode_set(pio_pins,0),
    pio_encode_jmp(11), pio_encode_wait_pin(false,0), pio_encode_set(pio_pindirs,0), pio_encode_set(pio_pins,0),
    pio_encode_push(false,false)};
  int bad = 0;
  for (unsigned i = 0; i < sizeof cap / sizeof *cap; i++) if (cap[i] != jacuzzi_bus::CAPTURE_PROGRAM[i]) { printf("capture %u: %04X vs SDK %04X\n", i, jacuzzi_bus::CAPTURE_PROGRAM[i], cap[i]); bad++; }
  for (unsigned i = 0; i < sizeof prs / sizeof *prs; i++) if (prs[i] != jacuzzi_bus::PRESS_PROGRAM[i]) { printf("press %u: %04X vs SDK %04X\n", i, jacuzzi_bus::PRESS_PROGRAM[i], prs[i]); bad++; }
  printf("%s: %zu capture + %zu press words checked against the Pico SDK encoders\n", bad ? "MISMATCH" : "OK", sizeof cap / sizeof *cap, sizeof prs / sizeof *prs);
  return bad;
}
