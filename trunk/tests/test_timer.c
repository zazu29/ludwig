/*****************************************************************************
 *
 *  test_timer
 *
 *  Test the timing routines.
 *
 *  $Id$
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *  (c) 2010 The University of Edinburgh
 *
 *****************************************************************************/

#include <time.h>
#include <limits.h>

#include "pe.h"
#include "timer.h"
#include "tests.h"

int main(int argc, char ** argv) {

  MPI_Init(&argc, &argv);
  pe_init();

  info("\nTesting timer routines...\n");

  info("sizeof(clock_t) is %d bytes\n", sizeof(clock_t));
  info("CLOCKS_PER_SEC is %d\n", CLOCKS_PER_SEC);
  info("LONG_MAX is %ld\n", LONG_MAX);
  info("Estimate maximum serial realaible times %f seconds\n",
       ((double) (LONG_MAX))/CLOCKS_PER_SEC);

  TIMER_init();
  TIMER_start(TIMER_TOTAL);
  TIMER_stop(TIMER_TOTAL);
  TIMER_statistics();

  pe_finalise();
  MPI_Finalize();

  return 0;
}
