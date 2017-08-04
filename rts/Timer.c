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

          // Special help for Windows to context-switch in the presence
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
          // For the non-threaded RTS on Unix (nb: where we don't use `timerfd`
          // because we use `timerfd` only in -threaded), that enforcing happens
          // automatically as a side effect of the timer signal:
          // The timer signal is a POSIX signal here, and POSIX signals interrupt
          // blocking syscalls on Unix (they return -1 and set EINTR).
          //
          // But on Windows, not all blocking syscalls can be interrupted with
          // POSIX signals.
          // To interrupt those, we call `interruptWorkerTask()` here, to make
          // context-switching work on the non-threaded RTS on Windows.
          //
          // See also:
          //   - `interruptWorkerTask()` in `Task.h`
          //   - `interruptOSThreadEvent` in `struct Task` in `Task.h`
          //   - `rts_getInterruptOSThreadEvent()` in `Task.h`
        #if defined(mingw32_HOST_OS) && !defined(THREADED_RTS)
          interruptWorkerTask();
        #endif
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
