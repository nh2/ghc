/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1995-2005
 *
 * Interval timer service for profiling and pre-emptive scheduling.
 *
 * ---------------------------------------------------------------------------*/

/*
 * The interval timer is used for profiling and for context switching in the
 * threaded build.
 *
 * This file defines the platform-independent view of interval timing, relying
 * on platform-specific services to install and run the timers.
 *
 */

#include "PosixSource.h"
#include "Rts.h"
#include "Task.h"

#include "Timer.h"
#include "Proftimer.h"
#include "Schedule.h"
#include "Ticker.h"
#include "Capability.h"
#include "RtsSignals.h"
#include "Itimer.h"

/* ticks left before next pre-emptive context switch */
static int ticks_to_ctxt_switch = 0;

/* idle ticks left before we perform a GC */
static int ticks_to_gc = 0;

/*
 * Function: handle_tick()
 *
 * At each occurrence of a tick, the OS timer will invoke
 * handle_tick().
 */
static
void
handle_tick(int unused STG_UNUSED)
{
  handleProfTick();
  if (RtsFlags.ConcFlags.ctxtSwitchTicks > 0) {
      ticks_to_ctxt_switch--;
      if (ticks_to_ctxt_switch <= 0) {
          ticks_to_ctxt_switch = RtsFlags.ConcFlags.ctxtSwitchTicks;
          contextSwitchAllCapabilities(); /* schedule a context switch */

          // Special help for context-switching in the presence
          // of blocking IO in the non-threaded runtime:
          //
          // Consider code like `timeout ... (hWaitForInput myFd ...)`,
          // where `timeout` is implemented with some form of
          // `forkIO (threadDelay ... >> throwTo)`, and `hWaitForInput` will
          // call some form of `poll()`/`select()` syscall.
          // Here we have some conceptually-blocking IO action `hWaitForInput`
          // that is to be cancelled by a Haskell cooperative thread producing
          // an exception eventually.
          // In order for that to work, we need to enforce context-switching
          // between the cooperative thread that implements `timeout` and the
          // between the thread that does the blocking syscall.
          // If we did not enforce this, we'd be stuck in the blocking syscall
          // and the `timeout` Haskell code would never get a chance to run
          // and produce its exception with `throwTo`.
          //
          // For the -threaded RTS, we don't need to enforce anything, because
          // there the `timeout` code and the blocking syscall can run
          // non-cooperatively in two different OS threads
          // (as long as the blocking syscall is made via a `safe` or
          // `interruptible` `ccall`, not an `unsafe` one, but wrapping blocking
          // syscalls in `unsafe` FFI calls is wrong anyway).
          //
          // For the non-threaded RTS, we enforce it by calling
          // `interruptOSThreadTimer()` to interrupt the (single) thread on which
          // Haskell, and thus blocking FFI calls, are running.
          //
          // Note we don't have to do this on those Unix platforms where
          // we don't use a pthread to implement the timer signal
          // (yes, on some platforms we use pthreads for the timer signal
          // even in the non-threaded RTS, see `Itimer.h`):
          // On such platforms, that enforcing happens
          // automatically as a side effect of the timer signal:
          // The timer signal is a POSIX signal to the whole process (and thus
          // single thread) here, and POSIX signals interrupt
          // blocking syscalls on Unix (they return -1 and set EINTR).
          //
          // Extra work has to be done on Windows, where not all blocking
          // syscalls can be interrupted with a POSIX signal; specifically
          // POSIX signals don't interrupt `WaitForMultipleObjects()`.
          // To interrupt such, signal the `interruptOSThreadEvent`, to make
          // context-switching work on the non-threaded RTS on Windows.
          // Note that for this to have an effect, the `interruptOSThreadEvent`
          // must have been one of the objects passed to
          // `WaitForMultipleObjects()`; that is, the C library must be designed
          // to specifically handle the Haskell RTS waking it up. If that is not
          // the case, all bets are off and the call will result in Haskell RTS
          // context switching not happening during the call's duration.
          //
          // See also:
          //   - How the choice of timer signal implementation is made
          //     in `Itimer.h`
          //   - `interruptOSThreadTimer()` in `win32/OSThreads.c`
          //   - `interruptOSThreadEvent` in `struct Task` in `Task.h`
          //   - `rts_getInterruptOSThreadEvent()` in `Task.h`
#if !defined(THREADED_RTS)

#if defined(mingw32_HOST_OS)
          SetEvent(rts_getInterruptOSThreadEvent());
#endif /* defined(mingw32_HOST_OS) */

#if USE_PTHREAD_FOR_ITIMER || defined(mingw32_HOST_OS)
          // Because
          //   * on platforms where we `USE_PTHREAD_FOR_ITIMER`, or
          //   * on Windows the timer signal is set up with
          //     `CreateTimerQueueTimer(... , WT_EXECUTEINTIMERTHREAD, ...)`,
          // `handle_tick()` runs in its own thread.
          // We want to interrupt the (single/only) thread that runs Haskell
          // and may be stuck in FFI calls; that is `mainThreadId`.
          ASSERT(mainThreadId != NULL);
          interruptOSThreadTimer(*mainThreadId);
#endif /* USE_PTHREAD_FOR_ITIMER || defined(mingw32_HOST_OS) */

#endif /* !defined(THREADED_RTS) */
      }
  }

  /*
   * If we've been inactive for idleGCDelayTime (set by +RTS
   * -I), tell the scheduler to wake up and do a GC, to check
   * for threads that are deadlocked.
   */
  switch (recent_activity) {
  case ACTIVITY_YES:
      recent_activity = ACTIVITY_MAYBE_NO;
      ticks_to_gc = RtsFlags.GcFlags.idleGCDelayTime /
                    RtsFlags.MiscFlags.tickInterval;
      break;
  case ACTIVITY_MAYBE_NO:
      if (ticks_to_gc == 0) {
          if (RtsFlags.GcFlags.doIdleGC) {
              recent_activity = ACTIVITY_INACTIVE;
#if defined(THREADED_RTS)
              wakeUpRts();
              // The scheduler will call stopTimer() when it has done
              // the GC.
#endif
          } else {
              recent_activity = ACTIVITY_DONE_GC;
              // disable timer signals (see #1623, #5991, #9105)
              // but only if we're not profiling (e.g. passed -h or -p RTS
              // flags). If we are profiling we need to keep the timer active
              // so that samples continue to be collected.
#if defined(PROFILING)
              if (!(RtsFlags.ProfFlags.doHeapProfile
                    || RtsFlags.CcFlags.doCostCentres)) {
                  stopTimer();
              }
#else
              stopTimer();
#endif
          }
      } else {
          ticks_to_gc--;
      }
      break;
  default:
      break;
  }
}

// This global counter is used to allow multiple threads to stop the
// timer temporarily with a stopTimer()/startTimer() pair.  If
//      timer_enabled  == 0          timer is enabled
//      timer_disabled == N, N > 0   timer is disabled by N threads
// When timer_enabled makes a transition to 0, we enable the timer,
// and when it makes a transition to non-0 we disable it.

static StgWord timer_disabled;

void
initTimer(void)
{
    initProfTimer();
    if (RtsFlags.MiscFlags.tickInterval != 0) {
        initTicker(RtsFlags.MiscFlags.tickInterval, handle_tick);
    }
    timer_disabled = 1;
}

void
startTimer(void)
{
    if (atomic_dec(&timer_disabled) == 0) {
        if (RtsFlags.MiscFlags.tickInterval != 0) {
            startTicker();
        }
    }
}

void
stopTimer(void)
{
    if (atomic_inc(&timer_disabled, 1) == 1) {
        if (RtsFlags.MiscFlags.tickInterval != 0) {
            stopTicker();
        }
    }
}

void
exitTimer (bool wait)
{
    if (RtsFlags.MiscFlags.tickInterval != 0) {
        exitTicker(wait);
    }
}
