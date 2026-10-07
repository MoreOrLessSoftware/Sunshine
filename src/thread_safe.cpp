/**
 * @file src/thread_safe.cpp
 * @brief Definitions for the Windows mutex and condition variable used by the thread-safe containers.
 */
#ifdef _WIN32
  // this include
  #include "thread_safe.h"

  // platform includes
  #include <windows.h>

namespace safe {
  static_assert(sizeof(SRWLOCK) == sizeof(void *), "safe::mutex stores an SRWLOCK in a pointer");
  static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "safe::condition_variable stores a CONDITION_VARIABLE in a pointer");

  void mutex::lock() {
    AcquireSRWLockExclusive(static_cast<PSRWLOCK>(native_handle()));
  }

  bool mutex::try_lock() {
    return TryAcquireSRWLockExclusive(static_cast<PSRWLOCK>(native_handle())) != FALSE;
  }

  void mutex::unlock() {
    ReleaseSRWLockExclusive(static_cast<PSRWLOCK>(native_handle()));
  }

  void *mutex::native_handle() {
    return &_srw;
  }

  void condition_variable::notify_one() noexcept {
    WakeConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&_cv));
  }

  void condition_variable::notify_all() noexcept {
    WakeAllConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&_cv));
  }

  void condition_variable::wait(std::unique_lock<mutex> &ul) {
    wait_ms(ul, INFINITE);
  }

  void condition_variable::wait_ms(std::unique_lock<mutex> &ul, unsigned long ms) {
    // Ends on its own when the time is up, or with a notification; either way the lock is held again
    SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(&_cv), static_cast<PSRWLOCK>(ul.mutex()->native_handle()), ms, 0);
  }
}  // namespace safe
#endif
