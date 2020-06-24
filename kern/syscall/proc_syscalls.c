#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"

#if OPT_A2
#include <mips/trapframe.h>
#endif

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
#if OPT_A2
  p->exitCode = exitcode;
#else
  /* for now, just include this to keep the compiler from complaining about
   an unused variable */
  (void)exitcode;
#endif

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

#if OPT_A2
  if (p->parent != NULL){
    lock_acquire(p->plock);
    cv_signal(p->p_cv, p->plock);
    lock_release(p->plock);
  } else {
    proc_destroy(p);
  }
#else
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
#endif  

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
#if OPT_A2
  KASSERT(retval);
  *retval = curproc->PID;
  return 0;
#else
  *retval = 1;
  return(0);
#endif
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
#if OPT_A2
  KASSERT(retval);
#endif
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
#if OPT_A2
  struct proc * targetChild = NULL;
  for(unsigned i = 0; i < array_num(curproc->children); i++){
   struct proc * currChild = array_get(curproc->children, i);
   if (currChild->PID == pid) {
     targetChild = currChild;
     break;
   }
  }
  
  if (targetChild == NULL){
    DEBUG(DB_SYSCALL, "sys_waitpid failed to find child");
    return ECHILD;
  }

  KASSERT(targetChild->parent == curproc);

  lock_acquire(targetChild->plock);
  if (targetChild->exitCode == -1){
    DEBUG(DB_SYSCALL, "sys_waitpid sleeping for child");
    cv_wait(targetChild->p_cv, targetChild->plock);
  }
  lock_release(targetChild->plock);

  if(status == NULL){
    DEBUG(DB_SYSCALL, "sys_waitpid status parameter is NULL");
	  return EFAULT;
  }

  exitstatus = _MKWAIT_EXIT(targetChild->exitCode);

#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
#if OPT_A2
  proc_destroy(targetChild);
#endif
  return(0);
}

#if OPT_A2
int
sys_fork(struct trapframe *tf, 
          pid_t *retval)
{
  KASSERT(tf);
  KASSERT(retval);
  struct proc * forked = proc_create_runprogram("forked");
  if (forked == NULL) {
    DEBUG(DB_SYSCALL, "sys_fork cannot create process structure, ENOMEM");
    return ENOMEM;
  }

  struct addrspace * as_cpy;
  as_copy(curproc_getas(), &as_cpy);
  if (as_cpy == NULL){
    proc_destroy(forked);
    DEBUG(DB_SYSCALL, "sys_fork cannot create addrspace, ENOMEM");
    return ENOMEM;
  }

  struct trapframe * tf_copy = kmalloc(sizeof(struct trapframe));
  if (tf_copy == NULL){
    proc_destroy(forked);
    as_destroy(as_cpy);
    DEBUG(DB_SYSCALL, "sys_fork cannot create trapframe, ENOMEM");
    return ENOMEM;
  }

  *tf_copy = *tf;

  // curproc_setas
  lock_acquire(forked->plock);
  forked->p_addrspace = as_cpy;
  forked->parent = curproc;
	lock_release(forked->plock);

  lock_acquire(curproc->plock);
  array_add(curproc->children, forked, NULL);
  lock_release(curproc->plock);

  if(thread_fork("forkedt", forked, enter_forked_process, tf_copy, 65536) != 0){
    kfree(tf_copy);
    as_destroy(forked->p_addrspace);
    proc_destroy(forked);
    DEBUG(DB_SYSCALL, "sys_fork cannot thread_fork, ENOMEM");
    return ENOMEM;
  }

  KASSERT(forked->PID > 0);
  *retval = forked->PID;

  return 0;
}
#endif // OPT_A2
