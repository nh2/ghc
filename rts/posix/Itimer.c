/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1995-2007
 *
 * Interval timer for profiling and pre-emptive scheduling.
 *
 * ---------------------------------------------------------------------------*/

#include "Itimer.h"

// Select the variant to use
#if defined(USE_PTHREAD_FOR_ITIMER)
#include "itimer/Pthread.c"
#elif defined(USE_TIMER_CREATE)
#include "itimer/TimerCreate.c"
#else
#include "itimer/Setitimer.c"
#endif
