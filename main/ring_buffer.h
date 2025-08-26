// ring_buffer.h  — simple C11 ring buffer "template" via macro
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Usage:
   RING_BUFFER_DEFINE(prevTemp_rb, int, 16);
   static prevTemp_rb temps;                 // one buffer
   static prevTemp_rb RBuffer[3];            // array of buffers

   prevTemp_rb_clear(&temps);
   prevTemp_rb_push(&temps, 123);
   int newest = prevTemp_rb_recent(&temps, 0);
   double avg = prevTemp_rb_average(&temps);
*/

#define RING_BUFFER_DEFINE(NAME, TYPE, CAP)                                           \
  _Static_assert((CAP) > 0, "capacity must be > 0");                                   \
  typedef struct {                                                                    \
    TYPE       buf[(CAP)];                                                            \
    size_t     head;        /* next write index */                                    \
    size_t     count;       /* number of valid items (<= CAP) */                      \
    long long  sum;         /* running sum for O(1) average */                        \
  } NAME;                                                                             \
                                                                                      \
  static inline void NAME##_clear(NAME *rb) {                                         \
    rb->head = 0; rb->count = 0; rb->sum = 0;                                         \
  }                                                                                   \
                                                                                      \
  /* wrap index (portable; modulo). If CAP is a power of two and you                    \
     care about speed, you can replace with: (i & ((CAP)-1)). */                      \
  static inline size_t NAME##_wrap(size_t i) { return (i % (CAP)); }                  \
                                                                                      \
  /* Push newest value; overwrites oldest when full */                                \
  static inline void NAME##_push(NAME *rb, TYPE v) {                                  \
    size_t idx = NAME##_wrap(rb->head);                                               \
    if (rb->count < (CAP)) {                                                          \
      rb->sum += (long long)v;                                                        \
      rb->buf[idx] = v;                                                               \
      rb->head = NAME##_wrap(rb->head + 1);                                           \
      rb->count++;                                                                    \
    } else {                                                                          \
      long long old = (long long)rb->buf[idx];                                        \
      rb->sum += (long long)v - old;                                                  \
      rb->buf[idx] = v;                                                               \
      rb->head = NAME##_wrap(rb->head + 1);                                           \
    }                                                                                 \
  }                                                                                   \
                                                                                      \
  /* i=0 → newest, i=1 → 2nd-newest ... i=size()-1 → oldest                           \
     Caller must ensure i < rb->count */                                              \
  static inline TYPE NAME##_recent(const NAME *rb, size_t i) {                        \
    size_t newest = NAME##_wrap(rb->head + (CAP) - 1);                                \
    return rb->buf[NAME##_wrap(newest - i)];                                          \
  }                                                                                   \
                                                                                      \
  static inline size_t     NAME##_size(const NAME *rb)   { return rb->count; }        \
  static inline int        NAME##_full(const NAME *rb)   { return rb->count == (CAP); }\
  static inline long long  NAME##_sum(const NAME *rb)    { return rb->sum; }          \
  static inline double     NAME##_average(const NAME *rb){                            \
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
  /* Valid only if size() > 0 */                                                      \
  static inline TYPE NAME##_oldest(const NAME *rb) {                                  \
    size_t oldest_idx = (rb->count < (CAP)) ? 0 : rb->head;                           \
    return rb->buf[NAME##_wrap(oldest_idx)];                                          \
  }                                                                                   \
  static inline TYPE NAME##_newest(const NAME *rb) {                                  \
    return rb->buf[NAME##_wrap(rb->head + (CAP) - 1)];                                \
  }
