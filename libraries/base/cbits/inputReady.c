/*
 * (c) The GRASP/AQUA Project, Glasgow University, 1994-2002
 *
 * hWaitForInput Runtime Support
 */

/* FD_SETSIZE defaults to 64 on Windows, which makes even the most basic
 * programs break that use select() on a socket FD.
 * Thus we raise it here (before any #include of network-related headers)
 * to 1024 so that at least those programs would work that would work on
 * Linux if that used select() (luckily it uses poll() by now).
 * See https://ghc.haskell.org/trac/ghc/ticket/13497#comment:23
 * The real solution would be to remove all uses of select()
 * on Windows, too, and use IO Completion Ports instead.
 * Note that on Windows, one can simply define FD_SETSIZE to the desired
 * size before including Winsock2.h, as described here:
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/ms740141(v=vs.85).aspx
 */
#if defined(_WIN32)
#define FD_SETSIZE 1024
#endif

/* select and supporting types is not Posix */
/* #include "PosixSource.h" */
#include <limits.h>
#include <stdbool.h>
#include "HsBase.h"
#include "Rts.h"
#if !defined(_WIN32)
#include <poll.h>
#endif

/*
 * Returns a timeout suitable to be passed into poll().
 *
 * If `remaining` contains a fractional milliseconds part that cannot be passed
 * to poll(), this function will return the next larger value that can, so
 * that the timeout passed to poll() would always be `>= remaining`.
 *
 * If `infinite`, `remaining` is ignored.
 */
static inline
int
compute_poll_timeout(bool infinite, Time remaining)
{
    if (infinite) return -1;

    if (remaining < 0) return 0;

    if (remaining > MSToTime(INT_MAX)) return INT_MAX;

    int remaining_ms = TimeToMS(remaining);

    if (remaining != MSToTime(remaining_ms)) return remaining_ms + 1;

    return remaining_ms;
}

#if defined(_WIN32)
/*
 * Returns a timeout suitable to be passed into select() on Windows.
 *
 * The given `remaining_tv` serves as a storage for the timeout
 * when needed, but callers should use the returned value instead
 * as it will not be filled in all cases.
 *
 * If `infinite`, `remaining` is ignored and `remaining_tv` not touched
 * (and may be passed as NULL in that case).
 */
static inline
struct timeval *
compute_windows_select_timeout(bool infinite, Time remaining,
                               /* out */ struct timeval * remaining_tv)
{
    if (infinite) {
        return NULL;
    }

    ASSERT(remaining_tv);

    if (remaining < 0) {
        remaining_tv->tv_sec = 0;
        remaining_tv->tv_usec = 0;
    } else if (remaining > MSToTime(LONG_MAX)) {
        remaining_tv->tv_sec = LONG_MAX;
        remaining_tv->tv_usec = LONG_MAX;
    } else {
        remaining_tv->tv_sec  = TimeToMS(remaining) / 1000;
        remaining_tv->tv_usec = TimeToUS(remaining) % 1000000;
    }

    return remaining_tv;
}

/*
 * Returns a timeout suitable to be passed into WaitForSingleObject(),
 * WaitForMultipleObjects(), etc, on Windows.
 *
 * If `remaining` contains a fractional milliseconds part that cannot be passed
 * to such wait function, this function will return the next larger value
 * that can, so that the timeout passed to such wait function would
 * always be `>= remaining`.
 *
 * If `infinite`, `remaining` is ignored.
 */
static inline
DWORD
compute_WaitForObject_timeout(bool infinite, Time remaining)
{
    // The WaitFor*Object() functions have the fascinating delicacy behaviour
    // that they waits indefinitely if the `DWORD dwMilliseconds`
    // is set to 0xFFFFFFFF (the maximum DWORD value), which is
    // 4294967295 seconds == ~49.71 days
    // (the Windows API calls this constant INFINITE...).
    //   https://msdn.microsoft.com/en-us/library/windows/desktop/ms687032(v=vs.85).aspx
    //
    // We ensure that if accidentally `remaining == 4294967295`, it does
    // NOT wait forever, by never passing that value to
    // WaitFor*Object() (so, never returning it from this function),
    // unless `infinite`.

    if (infinite) return INFINITE;

    if (remaining < 0) return 0;

    if (remaining >= MSToTime(INFINITE)) return INFINITE - 1;

    DWORD remaining_ms = TimeToMS(remaining);

    if (remaining != MSToTime(remaining_ms)) return remaining_ms + 1;

    return remaining_ms;
}

/*
 * Special case of `WaitForMultipleObjects()` that waits for the given HANDLE
 * or the GHC RTS's per-thread `rts_getInterruptOSThreadEvent()`.
 *
 * Returns the same values as `WaitForMultipleObjects`;
 * the `WAIT_OBJECT_0 + 0` index is the one for the given HANDLE, and
 * the `WAIT_OBJECT_0 + 1` index is the one for the interrupt event.
 */
static inline
DWORD
WaitForObjectOrThreadInterrupt(HANDLE hHandle, DWORD dwMilliseconds)
{
    enum nCount { nCount = 2 }; // to use it as a constant
    HANDLE hWaits[nCount];
    hWaits[0] = hHandle;
    hWaits[1] = rts_getInterruptOSThreadEvent();
    return WaitForMultipleObjects(
        nCount,
        hWaits,
        false, // wait for any of the HANDLEs to signal
        dwMilliseconds);
}
#endif

/*
 * inputReady(fd) checks to see whether input is available on the file
 * descriptor 'fd' within 'msecs' milliseconds (or indefinitely if 'msecs' is
 * negative). "Input is available" is defined as 'can I safely read at least a
 * *character* from this file object without blocking?' (this does not work
 * reliably on Linux when the fd is a not-O_NONBLOCK socket, so if you pass
 * socket fds to this function, ensure they have O_NONBLOCK;
 * see `man 2 poll` and `man 2 select`, and
 * https://ghc.haskell.org/trac/ghc/ticket/13497#comment:26).
 *
 * This function blocks until either:
 *   -  `msecs` have passed, or
 *   -  input is available, or
 *   -  it has been interrupted, e.g. by the timer signal,
 *      or by an exception if it is called via `InterruptibleFFI`
 *
 * Return value:
 *   1 => Input ready
 *   0 => not ready
 *  -1 => error, or interrupted by a signal (then callers should check
 *        errno == EINTR and retry depending on how much time is left)
 */
int
fdReady(int fd, bool write, int64_t msecs, bool isSock)
{
    bool infinite = msecs < 0;

    // if we need to track the time then record the end time in case we are
    // interrupted.
    Time endTime = 0;
    if (msecs > 0) {
        endTime = getProcessElapsedTime() + MSToTime(msecs);
    }

    // Invariant of all code below:
    // If `infinite`, then `remaining` and `endTime` are never used.

    Time remaining = MSToTime(msecs);

    // Note [Guaranteed syscall time spent]
    //
    // The implementation ensures that if fdReady() is called with N `msecs`,
    // it will not return before an FD-polling syscall *returns*
    // with `endTime` having passed.
    //
    // Consider the following scenario:
    //
    //     1 int ready = poll(..., msecs);
    //     2 if (EINTR happened) {
    //     3   Time now = getProcessElapsedTime();
    //     4   if (now >= endTime) return 0;
    //     5   remaining = endTime - now;
    //     6 }
    //
    // If `msecs` is 5 seconds, but in line 1 poll() returns with EINTR after
    // only 10 ms due to a signal, and if at line 2 the machine starts
    // swapping for 10 seconds, then line 4 will return that there's no
    // data ready, even though by now there may be data ready now, and we have
    // not actually checked after up to `msecs` = 5 seconds whether there's
    // data ready as promised.
    //
    // Why is this important?
    // Assume you call the pizza man to bring you a pizza.
    // You arrange that you won't pay if he doesn't ring your doorbell
    // in under 10 minutes delivery time.
    // At 9:58 fdReady() gets woken by EINTR and then your computer swaps
    // for 3 seconds.
    // At 9:59 the pizza man rings.
    // At 10:01 fdReady() will incorrectly tell you that the pizza man hasn't
    // rung within 10 minutes, when in fact he has.
    //
    // If the pizza man is some watchdog service or dead man's switch program,
    // this is problematic.
    //
    // To avoid it, we ensure that in the timeline diagram:
    //
    //                      endTime
    //                         |
    //     time ----+----------+-------+---->
    //              |                  |
    //       syscall starts     syscall returns
    //
    // the "syscall returns" event is always >= the "endTime" time.
    //
    // In the code this means that we never check whether to `return 0`
    // after a `Time now = getProcessElapsedTime();`, and instead always
    // let the branch marked [we waited the full msecs] handle that case.

#if !defined(_WIN32)
    struct pollfd fds[1];

    fds[0].fd = fd;
    fds[0].events = write ? POLLOUT : POLLIN;
    fds[0].revents = 0;

    // We need to wait in a loop because poll() accepts `int` but `msecs` is
    // `int64_t`.
    // We only retry within C when poll() timed out because of this type
    // difference; in all other cases we return to Haskell.

    while (true) {
        int res = poll(fds, 1, compute_poll_timeout(infinite, remaining));

        if (res == 0 && !infinite && remaining > MSToTime(INT_MAX)) {
            Time now = getProcessElapsedTime();
            remaining = endTime - now;
            continue;
        }

        return (res > 0) ? 1 : res;
    }

#else

    if (isSock) {
        int maxfd;
        fd_set rfd, wfd;
        struct timeval remaining_tv;

        if ((fd >= (int)FD_SETSIZE) || (fd < 0)) {
            barf("fdReady: fd is too big: %d but FD_SETSIZE is %d", fd, (int)FD_SETSIZE);
        }
        FD_ZERO(&rfd);
        FD_ZERO(&wfd);
        if (write) {
            FD_SET(fd, &wfd);
        } else {
            FD_SET(fd, &rfd);
        }

        /* select() will consider the descriptor set in the range of 0 to
         * (maxfd-1)
         */
        maxfd = fd + 1;

        // We need to wait in a loop because the `timeval` `tv_*` members
        // passed into select() accept are `long` (which is 32 bits on 32-bit
        // and 64-bit Windows), but `msecs` is `int64_t`.
        //   https://msdn.microsoft.com/en-us/library/windows/desktop/ms740560(v=vs.85).aspx
        //   https://stackoverflow.com/questions/384502/what-is-the-bit-size-of-long-on-64-bit-windows#384672
        // We only retry within C when poll() timed out because of this type
        // difference; in all other cases we return to Haskell.

        while (true) {
            int res = select(maxfd, &rfd, &wfd, NULL,
                             compute_windows_select_timeout(infinite, remaining,
                                                            &remaining_tv));

            if (res == 0 && !infinite && remaining > MSToTime(INT_MAX)) {
                Time now = getProcessElapsedTime();
                remaining = endTime - now;
                continue;
            }

            return (res > 0) ? 1 : res;
        }

    } else {
        DWORD rc;
        HANDLE hFile = (HANDLE)_get_osfhandle(fd);
        DWORD avail = 0;

        // Note that in older versions of this code, we tried to
        // have a `WaitForSingleObject()` and observe
        // `ERROR_OPERATION_ABORTED` when a `CancelSynchronousIo()`
        // came in to interrupt it. This did not work.
        // See:
        //   https://ghc.haskell.org/trac/ghc/ticket/8684#comment:25
        //   https://stackoverflow.com/questions/47336755/how-to-cancelsynchronousio-on-waitforsingleobject-waiting-on-stdin
        //
        // Instead, we wait for any of 2 objects (whichever returns
        // earlier): the actual file HANDLE and the
        // `rts_getInterruptOSThreadEvent()` event HANDLE for the
        // current thread, which gets signalled when GHC wants
        // to interrupt the thread.

        switch (GetFileType(hFile)) {

            case FILE_TYPE_CHAR:
                {
                    INPUT_RECORD buf[1];
                    DWORD count;

                    // nightmare.  A Console Handle will appear to be ready
                    // (WaitForMultipleObjects() returned a WAIT_OBJECT_0 index) when
                    // it has events in its input buffer, but these events might
                    // not be keyboard events, so when we read from the Handle the
                    // read() will block.  So here we try to discard non-keyboard
                    // events from a console handle's input buffer and then try
                    // the WaitForMultipleObjects() again.

                    // As a result, we have to loop and keep track of `remaining`
                    // time, even though for non-FILE_TYPE_CHAR calls to `fdReady`
                    // the calling Haskell code also has a loop.
                    // This is OK because if in the below code a
                    // the operation was aborted by an event signal to
                    // `rts_getInterruptOSThreadEvent()`, -1 is returned straight
                    // away.

                    while (1) // keep trying until we find a real key event
                    {
                        rc = WaitForObjectOrThreadInterrupt(
                            hFile,
                            compute_WaitForObject_timeout(infinite, remaining)
                            );
                        switch (rc) {
                            case WAIT_FAILED:
                                maperrno();
                                return -1;
                            case WAIT_TIMEOUT:
                                // We need to use < here because if remaining
                                // was INFINITE, we'll have waited for
                                // `INFINITE - 1` as per
                                // compute_WaitForObject_timeout(),
                                // so that's 1 ms too little. Wait again then.
                                if (!infinite && remaining < MSToTime(INFINITE))
                                    return 0; // real complete or [we waited the full msecs]
                                goto waitAgain;
                            default:
                                switch (rc - WAIT_OBJECT_0) {
                                    case 0:
                                        // hFile signaled.
                                        // Continue with the non-key events discarding below.
                                        break;
                                    case 1:
                                        // interruptOSThreadEvent signaled.
                                        // Map this interruption to EINTR so
                                        // that the calling Haskell code
                                        // retries.
                                        errno = EINTR;
                                        return -1;
                                    default:
                                        barf("fdReady: Unexpected WaitForObjectOrThreadInterrupt() return code in FILE_TYPE_CHAR case: %lu", rc);
                                }
                                break;
                        }

                        while (1) // discard non-key events
                        {
                            BOOL success = PeekConsoleInput(hFile, buf, 1, &count);
                            // printf("peek, rc=%d, count=%d, type=%d\n", rc, count, buf[0].EventType);
                            if (!success) {
                                rc = GetLastError();
                                if (rc == ERROR_INVALID_HANDLE || rc == ERROR_INVALID_FUNCTION) {
                                    return 1;
                                } else {
                                    maperrno();
                                    return -1;
                                }
                            }

                            if (count == 0) break; // no more events => wait again

                            // discard console events that are not "key down", because
                            // these will also be discarded by ReadFile().
                            if (buf[0].EventType == KEY_EVENT &&
                                buf[0].Event.KeyEvent.bKeyDown &&
                                buf[0].Event.KeyEvent.uChar.AsciiChar != '\0')
                            {
                                // it's a proper keypress:
                                return 1;
                            }
                            else
                            {
                                // it's a non-key event, a key up event, or a
                                // non-character key (e.g. shift).  discard it.
                                BOOL success = ReadConsoleInput(hFile, buf, 1, &count);
                                if (!success) {
                                    rc = GetLastError();
                                    if (rc == ERROR_INVALID_HANDLE || rc == ERROR_INVALID_FUNCTION) {
                                        return 1;
                                    } else {
                                        maperrno();
                                        return -1;
                                    }
                                }
                            }
                        }

                        Time now;
                    waitAgain:
                        now = getProcessElapsedTime();
                        remaining = endTime - now;
                    }
                }

            case FILE_TYPE_DISK:
                // assume that disk files are always ready:
                return 1;

            case FILE_TYPE_PIPE: {
                // WaitForMultipleObjects() doesn't work for pipes (it
                // always returns WAIT_OBJECT_0 even when no data is
                // available).  If the HANDLE is a pipe, therefore, we try
                // PeekNamedPipe():
                //
                // PeekNamedPipe() does not block, so if it returns that
                // there is no new data, and we were expected to block
                // (i.e. `infinite || msecs > 0`), then we have to sleep,
                // because the calling Haskell code will retry and thus create
                // a busy loop if we didn't sleep.
                BOOL success = PeekNamedPipe( hFile, NULL, 0, NULL, &avail, NULL );
                if (success) {
                    if (avail != 0) {
                        return 1;
                    } else { // no new data
                        if (infinite || remaining > 0) {
                            Sleep(1); // 1 millisecond (smallest possible time on Windows)
                            // Note one can also Sleep(0) on Windows to yield,
                            // but that will still busy loop if the machine
                            // has nothing else to do.
                        }
                        return 0;
                    }
                } else {
                    rc = GetLastError();
                    if (rc == ERROR_BROKEN_PIPE) {
                        return 1; // this is probably what we want
                    }
                    if (rc != ERROR_INVALID_HANDLE && rc != ERROR_INVALID_FUNCTION) {
                        maperrno();
                        return -1;
                    }
                }
            }
            /* PeekNamedPipe didn't work - fall through to the general case */

            default:
                while (true) {
                    rc = WaitForObjectOrThreadInterrupt(
                        hFile,
                        compute_WaitForObject_timeout(infinite, remaining)
                        );

                    switch (rc) {
                        case WAIT_FAILED:
                            maperrno();
                            return -1;
                        case WAIT_TIMEOUT:
                            // We need to use < here because if remaining
                            // was INFINITE, we'll have waited for
                            // `INFINITE - 1` as per
                            // compute_WaitForObject_timeout(),
                            // so that's 1 ms too little. Wait again then.
                            if (!infinite && remaining < MSToTime(INFINITE))
                                return 0; // real complete or [we waited the full msecs]
                            Time now = getProcessElapsedTime();
                            remaining = endTime - now;
                            break;
                        default:
                            switch (rc - WAIT_OBJECT_0) {
                                case 0:
                                    // hFile signaled.
                                    // Continue with the non-key events discarding below.
                                    return 1;
                                case 1:
                                    // interruptOSThreadEvent signaled.
                                    // Map this interruption to EINTR so
                                    // that the calling Haskell code
                                    // retries.
                                    errno = EINTR;
                                    return -1;
                                default:
                                    barf("fdReady: Unexpected WaitForObjectOrThreadInterrupt() return code: %lu", rc);
                            }
                            break;
                    }
                }
        }
    }
#endif
}
