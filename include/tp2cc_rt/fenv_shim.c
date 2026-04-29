#define _GNU_SOURCE 1

#include <fenv.h>
#include <stdint.h>

enum {
  TP2CC_EX_INVALID      = 1u << 0,
  TP2CC_EX_DENORMAL     = 1u << 1,
  TP2CC_EX_DIVBYZERO    = 1u << 2,
  TP2CC_EX_OVERFLOW     = 1u << 3,
  TP2CC_EX_UNDERFLOW    = 1u << 4,
  TP2CC_EX_PRECISION    = 1u << 5
};

// Keep a process-local Pascal view of the six exception-mask bits. GNU
// `fenv` exposes only the standard five traps portably, so the denormal bit
// remains software-tracked even when the host has no separate hardware flag.
static uint8_t tp2cc_exception_mask_bits;
static int tp2cc_exception_mask_initialized;

static int tp2cc_mask_bits_to_fenv(uint8_t bits) {
  int excepts = 0;

  if (bits & TP2CC_EX_INVALID) excepts |= FE_INVALID;
  if (bits & TP2CC_EX_DIVBYZERO) excepts |= FE_DIVBYZERO;
  if (bits & TP2CC_EX_OVERFLOW) excepts |= FE_OVERFLOW;
  if (bits & TP2CC_EX_UNDERFLOW) excepts |= FE_UNDERFLOW;
  if (bits & TP2CC_EX_PRECISION) excepts |= FE_INEXACT;
  return excepts;
}

static uint8_t tp2cc_fenv_to_mask_bits(int excepts) {
  uint8_t bits = 0;

  if (excepts & FE_INVALID) bits |= TP2CC_EX_INVALID;
  if (excepts & FE_DIVBYZERO) bits |= TP2CC_EX_DIVBYZERO;
  if (excepts & FE_OVERFLOW) bits |= TP2CC_EX_OVERFLOW;
  if (excepts & FE_UNDERFLOW) bits |= TP2CC_EX_UNDERFLOW;
  if (excepts & FE_INEXACT) bits |= TP2CC_EX_PRECISION;
  return bits;
}

static void tp2cc_sync_host_exception_mask(uint8_t bits) {
#if defined(__linux__) && defined(__GLIBC__)
  const int masked = tp2cc_mask_bits_to_fenv(bits);
  const int enabled = FE_ALL_EXCEPT & ~masked;

  // `SetExceptionMask` masks the exceptions in the Pascal set and enables the
  // remaining standard traps. Drive that directly through glibc's GNU `fenv`
  // extension hooks instead of poking host-specific control registers here.
  fedisableexcept(FE_ALL_EXCEPT);
  if (enabled != 0) feenableexcept(enabled);
#else
  (void)bits;
#endif
}

static uint8_t tp2cc_read_initial_exception_mask(void) {
  // Treat the denormal trap as masked by default. That matches the x87/Linux
  // startup state the bootstrap compiler expects, while other hosts simply
  // keep it as a software-only bit.
  uint8_t bits = TP2CC_EX_DENORMAL;

#if defined(__linux__) && defined(__GLIBC__)
  const int enabled = fegetexcept();
  bits |= tp2cc_fenv_to_mask_bits(FE_ALL_EXCEPT & ~enabled);
#endif
  return bits;
}

static void tp2cc_ensure_exception_mask_initialized(void) {
  if (tp2cc_exception_mask_initialized) return;
  tp2cc_exception_mask_bits = tp2cc_read_initial_exception_mask();
  tp2cc_exception_mask_initialized = 1;
}

uint8_t tp2cc_get_exception_mask_bits(void) {
  tp2cc_ensure_exception_mask_initialized();
  return tp2cc_exception_mask_bits;
}

uint8_t tp2cc_set_exception_mask_bits(uint8_t bits) {
  const uint8_t masked_bits = bits & 0x3Fu;
  const uint8_t previous = tp2cc_get_exception_mask_bits();

  tp2cc_exception_mask_bits = masked_bits;
  tp2cc_sync_host_exception_mask(masked_bits);
  return previous;
}

uint16_t tp2cc_get_8087_control_word(void) {
  // The translated compiler still spells its startup helper in terms of
  // `Get8087CW` / `Set8087CW`, but it only edits the low exception-mask bits.
  // Synthesize a stable control-word value around the authoritative Pascal
  // mask instead of pretending to model the full x87 rounding/precision state.
  const uint16_t default_high_bits = 0x0340u;
  return (uint16_t)(default_high_bits | tp2cc_get_exception_mask_bits());
}

void tp2cc_set_8087_control_word(uint16_t cw) {
  (void)tp2cc_set_exception_mask_bits((uint8_t)(cw & 0x3Fu));
}
