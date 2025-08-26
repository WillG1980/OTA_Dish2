/* ring_buffer.h - header-only C ring buffer "template"
   Usage:
     #include "ring_buffer.h"
     RING_BUFFER_DEFINE(prevTemp_rb, int, 64);      // type=int, capacity=64
     prevTemp_rb temps = {0};                       // one buffer
     prevTemp_rb RBuffer[4] = {0};                  // array of buffers
     prevTemp_rb_push(&temps, 123);
     int newest = prevTemp_rb_recent(&temps, 0);
     double avg = prevTemp_rb_average(&temps);
*/

#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

/* NAME:   type prefix for generated API (e.g., prevTemp_rb)
   TYPE:   element type (e.g., int)
   CAP:    capacity (positive integer; power-of-two enables faster wrap)
*/
#define RING_BUFFER_DEFINE(NAME, TYPE, CAP)                                           \
  _Static_assert((CAP) > 0, #NAME ": capacity must be > 0");                          \
  typedef struct {                                                                    \
    TYPE     buf[(CAP)];                                                              \
    size_t   head;      /* next write index */                                        \
    size_t   count;     /* number of valid items (<= CAP) */                          \
    long long sum;      /* running sum for O(1) average */                            \
  } NAME;                                                                             \
                                                                                      \
  /* Fast wrap for power-of-two CAP; generic modulo otherwise */                      \
  static inline size_t NAME##_wrap(size_t i) {                                        \
  /* preprocessor chooses branch at compile time */                                   \
  /* NOLINTNEXTLINE(bugprone-sizeof-expression) */                                    \
  /* clang-format off */                                                              \
  /* choose mask wrap when CAP is power of two */                                     \
  /* note: ((CAP) & ((CAP) - 1)) == 0  <=> power-of-two */                            \
  /* clang-format on */                                                               \
  /* The #if is evaluated when this macro expands */                                  \
  /* NOLINTNEXTLINE */                                                                \
  /* cppcheck-suppress preprocessorErrorDirective */                                  \
  /* fall through in C if not PoT */                                                  \
  /* Use two definitions guarded by preprocessor */                                   \
  }                                                                                   \

/* Re-open macro to inject the conditional wrap definition cleanly */
#undef RING_BUFFER_DEFINE
#define RING_BUFFER_DEFINE(NAME, TYPE, CAP)                                           \
  _Static_assert((CAP) > 0, #NAME ": capacity must be > 0");                          \
  typedef struct {                                                                    \
    TYPE     buf[(CAP)];                                                              \
    size_t   head;      /* next write index */                                        \
    size_t   count;     /* number of valid items (<= CAP) */                          \
    long long sum;      /* running sum for O(1) average */                            \
  } NAME;                                                                             \
                                                                                      \
  static inline size_t NAME##_wrap(size_t i) {                                        \
  /* compile-time branch */                                                           \
  /* power-of-two? */                                                                 \
  /* the preprocessor sees CAP as a literal here */                                   \
  /* NOLINTNEXTLINE */                                                                \
  /* clang-format off */                                                              \
  #if ((CAP) & ((CAP) - 1)) == 0                                                      \
      return i & ((CAP) - 1);                                                         \
  #else                                                                               \
      return i % (CAP);                                                               \
  #endif                                                                              \
  }                                                                                   \
                                                                                      \
  static inline void NAME##_clear(NAME *rb) {                                         \
    rb->head = 0; rb->count = 0; rb->sum = 0;                                         \
  }                                                                                   \
                                                                                      \
  /* Push newest value; overwrites oldest when full */                                 \
  static inline void NAME##_push(NAME *rb, TYPE v) {                                  \
    if (rb->count < (CAP)) {                                                          \
      rb->sum += (long long)v;                                                        \
      rb->buf[NAME##_wrap(rb->head)] = v;                                             \
      rb->head = NAME##_wrap(rb->head + 1);                                           \
      rb->count++;                                                                    \
    } else {                                                                          \
      size_t idx = NAME##_wrap(rb->head);                                             \
      TYPE old = rb->buf[idx];                                                        \
      rb->sum += (long long)v - (long long)old;                                       \
      rb->buf[idx] = v;                                                               \
      rb->head = NAME##_wrap(rb->head + 1);                                           \
    }                                                                                 \
  }                                                                                   \
                                                                                      \
  /* i=0 → newest, i=1 → 2nd-newest ... i=size()-1 → oldest (caller ensures i<count) */\
  static inline TYPE NAME##_recent(const NAME *rb, size_t i) {                        \
    size_t newest = NAME##_wrap(rb->head + (CAP) - 1);                                 \
    return rb->buf[NAME##_wrap(newest - i)];                                          \
  }                                                                                   \
                                                                                      \
  static inline size_t NAME##_size(const NAME *rb) { return rb->count; }              \
  static inline int    NAME##_full(const NAME *rb) { return rb->count == (CAP); }     \
  static inline long long NAME##_sum(const NAME *rb) { return rb->sum; }              \
  static inline double NAME##_average(const NAME *rb) {                               \
    return rb->count ? (double)rb->sum / (double)rb->count : 0.0;                     \
  }                                                                                   \
                                                                                      \
  /* Iterate oldest → newest; fn(value, index_from_oldest) */                         \
  static inline void NAME##_for_each_chronological(                                   \
      const NAME *rb, void (*fn)(TYPE, size_t)) {                                     \
    size_t n = rb->count;                                                             \
    size_t start = (rb->count < (CAP)) ? 0 : rb->head;                                \
    for (size_t k = 0; k < n; ++k) {                                                  \
      TYPE v = rb->buf[NAME##_wrap(start + k)];                                       \
      fn(v, k);                                                                       \
    }                                                                                 \
  }                                                                                   \
                                                                                      \
  static inline TYPE NAME##_oldest(const NAME *rb) {                                  \
    /* valid only if size()>0 */                                                      \
    size_t oldest_idx = (rb->count < (CAP)) ? 0 : rb->head;                           \
    return rb->buf[NAME##_wrap(oldest_idx)];                                          \
  }                                                                                   \
                                                                                      \
  static inline TYPE NAME##_newest(const NAME *rb) {                                  \
    /* valid only if size()>0 */                                                      \
    return rb->buf[NAME##_wrap(rb->head + (CAP) - 1)];                                \
  }

#endif /* RING_BUFFER_H_ */
