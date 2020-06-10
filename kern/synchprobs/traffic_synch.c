#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
static struct cv * CVS[4];
static struct lock * intersection;
static volatile int waited[4] = {0,0,0,0};


/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  for (int i = 0; i < 4; i++){
    CVS[i] = cv_create("a");
    DEBUGASSERT(CVS[i] != NULL);
  }
  intersection = lock_create("intersection");
  DEBUGASSERT(intersection != NULL);
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  for (int i = 0; i < 4; i++){
    DEBUGASSERT(CVS[i] != NULL);
    kfree(CVS[i]);
  }
  DEBUGASSERT(intersection != NULL);
  kfree(intersection);
  return;
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  // (void)origin;  /* avoid compiler complaint about unused parameter */
  (void)destination; /* avoid compiler complaint about unused parameter */
  lock_acquire(intersection);
  bool mustWait = false;
  for (unsigned i = 0; i < 4; i++){
    if (waited[i] != 0 && i != origin) mustWait = true;
  }
  while(mustWait){
    cv_wait(CVS[origin],intersection);
    mustWait = false;
    for (unsigned i = 0; i < 4; i++){
      if (waited[i] != 0 && i != origin) mustWait = true;
    }
  }
  waited[origin]++;
  lock_release(intersection);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  // (void)origin;  /* avoid compiler complaint about unused parameter */
  (void)destination; /* avoid compiler complaint about unused parameter */
  lock_acquire(intersection);
  waited[origin]--;
  for(unsigned i = 0; i < 4; i++){
    if (i != origin) cv_broadcast(CVS[i],intersection);
  }
  lock_release(intersection);
}
