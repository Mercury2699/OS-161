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
#include <kern/fcntl.h>
#include <vfs.h>
#endif

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
#if OPT_A2
  lock_acquire(destroyLock);
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

  for(unsigned i = 0; i < array_num(p->children); i++){
   struct proc * currChild = array_get(p->children, i);
   if (currChild != NULL){
    currChild->parent = NULL;
    if(currChild->exited == 1){
      proc_destroy(currChild);
    }
   }
  }

  if (p->parent != NULL){
    lock_acquire(p->plock);
    cv_signal(p->p_cv, p->plock);
    lock_release(p->plock);
  } else {
    proc_destroy(p);
  }
  p->exited = true;
  lock_release(destroyLock);
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
     array_remove(curproc->children, i);
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

int copyoutargs(int argc, char * argv[], vaddr_t * stackptr){
  *stackptr = ROUNDUP(*stackptr,8);
  vaddr_t argvptrs[argc+1];
  for(int i = argc; i >= 0; i--){
    if (i == argc) {
      argvptrs[i] = 0; continue;
    }
    int arglen = strlen(argv[i]) + 1;
    *stackptr -= arglen;
    int result = copyoutstr(argv[i], (userptr_t)*stackptr, arglen, NULL);
    if (result) return result;
    argvptrs[i] = *stackptr;
  }
  for(;(*stackptr) % 4; (*stackptr)--);
  for(int i = argc ; i >= 0; i--){
    int padding = ROUNDUP(sizeof(*stackptr), 4);
    *stackptr -= padding;
    int result = copyout(&argvptrs[i], (userptr_t) *stackptr, sizeof(vaddr_t));
    if (result) return result;
  }
  return 0;
}

int
sys_execv(const char * progname, char * args[])
{
  // Copied from runprogram
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
  // end copy

  if (progname == NULL || args == NULL) return EFAULT;

  /* Count arguments */
  int argc;
  for(argc = 0; args[argc] != NULL; argc++);
  // kprintf("I counted argc = %d\n", argc);

  /* Copy arguments to kernel */
  char ** kargv = kmalloc((argc+1)*(sizeof(char*)));
  for (int i = 0; i < argc; i++){
    kargv[i] = kmalloc((strlen(args[i])+1)*sizeof(char));
    if (kargv[i] == NULL) {
      kfree(kargv);
      return ENOMEM;
    }
    int err = copyinstr((const_userptr_t) args[i], kargv[i], (strlen(args[i])+1)*sizeof(char), NULL);
    if (err) {
      for(;i >= 0; i--) kfree(kargv[i]);
      kfree(kargv);
      return err;
    }
  }
  kargv[argc] = NULL;

  // for (int i = 0; i < argc; i++){
  //   kprintf("I copied arg: %s\n", kargv[i]);
  // }

  /* Copy program path to kernel */
  char * kprogname = kmalloc((strlen(progname)+1) * sizeof(char));
  if (kprogname == NULL){
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    return ENOMEM;
  }
  int err = copyinstr((const_userptr_t) progname, kprogname, strlen(progname)+1, NULL);
  if (err) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
    return err;
  }
  // kprintf("I copied progname to kernel: %s\n", kprogname);

  // Copied from runprogram
	/* Open the file. */
	result = vfs_open(kprogname, O_RDONLY, 0, &v);
	if (result) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
		return result;
	}

	/* We should NOT be a new process. */
	// KASSERT(curproc_getas() == NULL);

  // Swap a new addrspace
  struct addrspace * old_as = curproc_getas();
  as = as_create();
  if (as == NULL){
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
		vfs_close(v);
		return ENOMEM;
  }
  curproc_setas(as);
  as_activate();

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
    curproc_setas(old_as);
    as_destroy(as);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
		/* p_addrspace will go away when curproc is destroyed */
    curproc_setas(old_as);
    as_destroy(as);
		return result;
	}

  result = copyoutargs(argc, kargv, &stackptr);
  if (result) {
    for(int i = 0; i < argc; i++) kfree(kargv[i]);
    kfree(kargv);
    kfree(kprogname);
    curproc_setas(old_as);
    as_destroy(as);
    return result;
  }

  for(int i = 0; i < argc; i++) kfree(kargv[i]);
  kfree(kargv);
  kfree(kprogname);
  as_destroy(old_as);

	/* Warp to user mode. */
	enter_new_process(argc, (userptr_t) stackptr, stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

#endif // OPT_A2
