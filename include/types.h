/**
 * Useful types for Popcorn Chameleon.
 *
 * Author: Rob Lyerly <rlyerly@vt.edu>
 * Date: 10/11/2018
 */

#ifndef _TYPES_H
#define _TYPES_H

#include <utility>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace chameleon {

/* Binary file access error codes */
#define BINARY_RETCODES \
  X(ElfFailed, "could not initialize libelf") \
  X(OpenFailed, "could not open binary") \
  X(ElfReadError, "could not read ELF metadata") \
  X(InvalidElf, "invalid ELF file/format") \
  X(NoSuchSection, "could not find ELF section/segment") \
  X(BadMetadata, "invalid metadata encoded in binary")

/* Process control error codes */
#define PROCESS_RETCODES \
  X(ForkFailed, "fork() returned an error") \
  X(RecvUFFDFailed, "could not receive userfaultfd descriptor from child") \
  X(TraceSetupFailed, "setting up tracing of child from parent failed") \
  X(WaitFailed, "wait() returned an error") \
  X(PtraceFailed, "ptrace() returned an error") \
  X(Exists, "process already exists") \
  X(DoesNotExist, "process exited or terminated") \
  X(InvalidState, "operation not allowed in current process state")

/* State transformation error codes */
#define TRANSFORM_RETCODES \
  X(InvalidTransformConfig, "invalid transformation configuration") \
  X(DisasmSetupFailed, "setting up disassembler failed") \
  X(RemapCodeFailed, "could not remap code section for userfaulfd setup") \
  X(AnalysisFailed, "could not analyze code to ensure correctness") \
  X(RandomizeFailed, "could not randomize code section") \
  X(FaultHandlerFailed, "could not start fault handling thread") \
  X(UffdHandshakeFailed, "userfaultfd API handshake failed") \
  X(UffdRegisterFailed, "userfaultfd register region failed") \
  X(UffdCopyFailed, "userfaultfd copy failed") \
  X(EncodeFailed, "re-encoding transformed instruction failed") \
  X(BadFault, "kernel delivered unexpected or unhandled fault") \
  X(MarshalDataFailed, "failed to marshal data to handle fault") \
  X(BadMarshal, "invalid view of memory, found overlapping regions")

/* Other miscellaneous error codes */
#define MISC_RETCODES \
  X(NoTimestamp, "could not get timestamp")

enum ret_t {
  Success = 0,
#define X(code, desc) code, 
  BINARY_RETCODES
  PROCESS_RETCODES
  TRANSFORM_RETCODES
  MISC_RETCODES
#undef X
};

const char *retText(ret_t retcode);

/**
 * Iterate over contiguous entries of a given type.  Useful for providing a
 * sliced view of an array with bounds checking.
 */
template<typename T>
class iterator {
public:
  iterator() : cur(0), len(0), entries(nullptr) {}
  iterator(T *entries, size_t len) : cur(0), len(len), entries(entries) {}

  /**
   * Return a sentinal empty iterator.
   * @return an empty iterator
   */
  static iterator empty() { return iterator(nullptr, 0); }

  size_t getLength() const { return len; }
  bool end() const { return cur >= len; }

  void reset() { cur = 0; }
  void operator++() { cur++; }

  T *operator[](size_t idx) const {
    if(idx < len) return &entries[idx];
    else return nullptr;
  }

  const T *operator*() const {
    if(cur < len) return &this->entries[cur];
    else return nullptr;
  }

  bool operator!() const { return entries == nullptr; }
private:
  size_t cur, len;
  T *entries;
};

typedef iterator<unsigned char> byte_iterator;

/* A range of values.  The first element *must* be smaller than the second. */
typedef std::pair<int64_t, int64_t> range_t;

/**
 * Timer utility for measuring execution times.
 */
class Timer {
public:
  /* Unit of elapsed time. */
  enum Unit {
    Nano,
    Micro,
    Milli,
    Second,
  };

  Timer() : s(0), e(0), accum(0) {}

  /**
   * Convert a struct timespec to nanoseconds.
   * @param ts a struct timespec
   * @return time in nanoseconds
   */
  static uint64_t timespecToNano(struct timespec &ts)
  { return (ts.tv_sec * 1000000000ULL) + ts.tv_nsec; }

  /**
   * Get a timestamp in nanoseconds.
   * @return timestamp in nanoseconds or UINT64_MAX if timestamp API failed
   */
  static uint64_t timestamp() {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1) return UINT64_MAX;
    else return timespecToNano(ts);
  }

  /**
   * Take a starting timestamp.
   * @return a return code describing the outcome
   */
  ret_t start() {
    s = timestamp();
    return s != UINT64_MAX ? ret_t::Success : ret_t::NoTimestamp;
  }

  /**
   * Take an ending timestamp & accumulate elapsed time if requested.  Users
   * should have already called start(), otherwise subsequent calls to the
   * timer (or accumulations) may return garbage.
   *
   * @param doAccumulate whether or not to accumulate elapsed time
   * @return a return code describing the outcome
   */
  ret_t end(bool doAccumulate = false) {
    e = timestamp();
    if(e != UINT64_MAX) {
      if(doAccumulate) accum = e - s;
      return ret_t::Success;
    }
    else return ret_t::NoTimestamp;
  }

  /**
   * Convert elapsed time in nanoseconds to another unit.
   * @param nano elapsed time in nanoseconds
   * @param unit preferred unit type
   * @return elapsed time in new units
   */
  static uint64_t toUnit(uint64_t nano, Unit unit) {
    switch(unit) {
    default: /* fall through */
    case Nano: return nano;
    case Micro: return nano / 1000ULL;
    case Milli: return nano / 1000000ULL;
    case Second: return nano / 1000000000ULL;
    }
  }

  /**
   * Return the time elapased between calls to start() and end().
   * @param u preferred unit type
   * @return elapsed time in nanoseconds
   */
  uint64_t elapsed(Unit u) { return toUnit(e - s, u); }

  /**
   * Return the total elapsed time from all calls to elapsed().
   * @param u preferred unit type
   * @return total elapsed time in nanoseconds
   */
  uint64_t totalElapsed(Unit u) { return toUnit(accum, u); }

private:
  uint64_t s, e;  /* starting & ending timestamps */
  uint64_t accum; /* accumulated time */
};

}

#endif /* _TYPES_H */

