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
 */
#if defined(_WIN32)
#define FD_SETSIZE 1024
#endif

/* select and supporting types is not Posix */
/* #include "PosixSource.h" */
#include "HsBase.h"
#include "Rts.h"
#if !defined(_WIN32)
#include <poll.h>
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
 * This function blocks until either `msecs` have passed, or input is
 * available.
 *
 * Return value:
 *   1 => Input ready
 *   0 => not ready
 *  -1 => error, or interrupted by a signal (then callers should check
 *        errno == EINT and retry depending on how much time is left)
 */
int
fdReady(int fd, int write, int msecs, int isSock)
{
    puts("fdReady called\n");
    fprintf(stderr, "fdReady called with msecs = %d\n", msecs);
    if (msecs < 0) {
        fprintf(stderr, "fdReady: msecs is negative: %d\n", msecs);
        abort();
    }

#if !defined(_WIN32)
    struct pollfd fds[1];

    fds[0].fd = fd;
    fds[0].events = write ? POLLOUT : POLLIN;
    fds[0].revents = 0;

    // res is the number of FDs with events, or -1 on failure
    int res = poll(fds, 1, msecs);

    return res > 0 ? (res > 0) : res;

#else

    if (isSock) {
        int maxfd;
        fd_set rfd, wfd;

        if ((fd >= (int)FD_SETSIZE) || (fd < 0)) {
            fprintf(stderr, "fdReady: fd is too big: %d but FD_SETSIZE is %d\n", fd, (int)FD_SETSIZE);
            abort();
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

        Time remaining = MSToTime(msecs);
        remaining_tv.tv_sec  = TimeToMS(remaining) / 1000;
        remaining_tv.tv_usec = TimeToUS(remaining) % 1000000;

        // res is the number of FDs with events, or -1 on failure
        int res = select(maxfd, &rfd, &wfd, NULL, &remaining_tv);

        return res > 0 ? (res > 0) : res;

    } else {
        DWORD rc;
        HANDLE hFile = (HANDLE)_get_osfhandle(fd);
        DWORD avail = 0;

        Time remaining = MSToTime(msecs);

        switch (GetFileType(hFile)) {

            case FILE_TYPE_CHAR:
                {
                    INPUT_RECORD buf[1];
                    DWORD count;

                    // nightmare.  A Console Handle will appear to be ready
                    // (WaitForSingleObject() returned WAIT_OBJECT_0) when
                    // it has events in its input buffer, but these events might
                    // not be keyboard events, so when we read from the Handle the
                    // read() will block.  So here we try to discard non-keyboard
                    // events from a console handle's input buffer and then try
                    // the WaitForSingleObject() again.

                    // As a result, we have to loop and keep track of `remaining`
                    // time, even though for non-FILE_TYPE_CHAR calls to `fdReady`
                    // the calling Haskell code does this loop.
                    // This is OK because if in the below code a
                    // ERROR_OPERATION_ABORTED comes in, -1 is returned straight
                    // away.

                    while (1) // keep trying until we find a real key event
                    {
                        rc = WaitForSingleObject( hFile, TimeToMS(remaining) );
                        switch (rc) {
                            case WAIT_TIMEOUT: return 0;
                            case WAIT_OBJECT_0: break;
                            default: /* WAIT_FAILED */ maperrno(); return -1;
                        }

                        while (1) // discard non-key events
                        {
                            rc = PeekConsoleInput(hFile, buf, 1, &count);
                            // printf("peek, rc=%d, count=%d, type=%d\n", rc, count, buf[0].EventType);
                            if (rc == 0) {
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
                                rc = ReadConsoleInput(hFile, buf, 1, &count);
                                if (rc == 0) {
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

                        Time now = getProcessElapsedTime();
                        remaining = endTime - now;
                    }
                }

            case FILE_TYPE_DISK:
                // assume that disk files are always ready:
                return 1;

            case FILE_TYPE_PIPE:
                // WaitForMultipleObjects() doesn't work for pipes (it
                // always returns WAIT_OBJECT_0 even when no data is
                // available).  If the HANDLE is a pipe, therefore, we try
                // PeekNamedPipe():
                //
                // PeekNamedPipe() does not block, so if it returns that
                // there is no new data, and we were expected to
                // block (msecs > 0), then we have to sleep, because the
                // calling Haskell code will retry and thus create a busy loop
                // if we didn't sleep.
                rc = PeekNamedPipe( hFile, NULL, 0, NULL, &avail, NULL );
                if (rc != 0) {
                    if (avail != 0) {
                        return 1;
                    } else { // no new data
                        if (msecs > 0) {
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
                /* PeekNamedPipe didn't work - fall through to the general case */

            default:
                rc = WaitForSingleObject( hFile, msecs );

                switch (rc) {
                    case WAIT_TIMEOUT: return 0;
                    case WAIT_OBJECT_0: return 1;
                    default: /* WAIT_FAILED */ maperrno(); return -1;
                }
        }
    }
#endif
}
