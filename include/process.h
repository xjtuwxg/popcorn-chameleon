/**
 * class Process
 *
 * Implements ability to fork and attach to new processes, introspect and
 * manipulate child processes and clean up when they have finished.  A Process
 * object should only ever have 1 forked child under its control, although it
 * may control multiple threads within that process.
 *
 * Author: Rob Lyerly <rlyerly@vt.edu>
 * Date: 10/11/2018
 */

#ifndef _PROCESS_H
#define _PROCESS_H

#include <semaphore.h>
#include <sys/signal.h>
#include <sys/types.h>
#include "trace.h"
#include "types.h"

struct parasite_ctl;

namespace chameleon {

class Process {
public:
  enum status_t {
    Ready = 0,   /* child is ready to be run */
    Running,     /* child is running */
    Exited,      /* child exited */
    SignalExit,  /* child terminated due to signal */
    Stopped,     /* child is stopped */
    Interrupted, /* child was interrupted by chameleon */
    Unknown      /* child has some other status */
  };

  /* Size of stacks allocated by the OS by default */
  static size_t defaultStackSize;

  /**
   * Construct a process object.  Initialize the process' command-line but
   * nothing else; users must call forkAndExec() to start the process.
   *
   * Note: argv[0] *must* be the binary to execute
   *
   * @param argc number of arguments
   * @param argv arguments for child process to be executed
   */
  Process(int argc, char **argv) : argc(argc), argv(argv), pid(-1), memFD(-1),
                                   stackBounds(0, 0), newTaskPid(-1),
                                   status(Ready), exit(0),
                                   stopReason(stop_t::Other),
                                   reinjectSignal(false), uffd(-1),
                                   nthreads(0), parasite(nullptr) {}

  /**
   * Construct a process object from an existing task.  Should be called when
   * another traced child calls fork() and the new child is trace-stopped.
   *
   * Note: the child isn't yet in a usable state; users should call
   * initForkedChild() to finish initialization.
   *
   * @param pid the pid of the new task
   * @param argc (optional) number of arguments
   * @param argv (optional) arguments for child process to be executed
   */
  Process(pid_t pid, int argc = 0, char **argv = nullptr)
    : argc(argc), argv(argv), pid(pid), newTaskPid(-1), status(Running),
      exit(0), stopReason(stop_t::Other), reinjectSignal(false), uffd(-1),
      nthreads(0), parasite(nullptr) {}
  Process() = delete;

  /////////////////////////////////////////////////////////////////////////////
  // Execution control
  /////////////////////////////////////////////////////////////////////////////

  /**
   * Fork a child process to execute the application and set up ptrace.  The
   * call will wait for the forked tracee's initialization and application
   * execution.  After the tracee receives SIGTRAP (before executing any
   * application code) the tracer will initialize and return with the tracee in
   * the stopped state.
   *
   * Note: the call returns with the child process in the stopped state.  Users
   * should call resume() or continue convenience functions to start the child
   * process.
   *
   * @return a return code describing the outcome
   */
  ret_t forkAndExec();

  /**
   * Initialize the Process object for a newly-forked child process.  The child
   * should be trace-stopped at the fork event.
   * @return a return code describing the outcome
   */
  ret_t initForkedChild();

  /**
   * Trace a newly created thread.
   * @param pid the PID of the new thread
   */
  ret_t traceThread(pid_t pid);

  /**
   * Wait for a child event and update the process' status, which can be
   * queried via getStatus() after returning.
   *
   * Note: if wait() is interrupted (i.e., if calling getStatus() after wait()
   * returns status_t::Interrupted), the process object will return from wait()
   * with SIGINT masked in the calling thread so that the caller can perform
   * processing without further interrupts.  The caller must subsequently call
   * restoreInterrupt() to restore normal function.
   *
   * Note: users should *not* attempt to interrupt child processes using libc
   * calls (i.e., kill()) on their own, but should *only* interact with the
   * child through the APIs exposed by Process; circumventing Process' APIs
   * will interfere with child signal delivery.
   *
   * @return a return code describing the outcome
   */
  ret_t wait();

  /**
   * If wait() with the child's status as status_t::Interrupted, perform
   * processing necessary to restore normal execution for the caller.
   *
   * @return a return code describing the outcome
   */
  ret_t restoreInterrupt();

  /**
   * Interrupt a child.  Users must call wait() in order to synchronize with
   * the kernel's interrupt of the child.
   *
   * @return a return code describing the outcome
   */
  ret_t interrupt();

  /**
   * Send a signal to the child process.  If it is multithreaded, it is
   * non-deterministic which threads gets the signal.
   *
   * @param signo the signal number
   * @return a return code describing the outcome
   */
  ret_t signalProcess(int signo) const;

  /**
   * Resume a child.  If type = Continue, tell ptrace to stop at the next
   * signal delivery to the child.  If type = Syscall, tell ptrace to stop at
   * either the next signal delivery or system call boundary (either going into
   * or coming out of kernel) by the child.  If type = SingleStep, execute a
   * single instruction.  Users must call wait() (even for single-stepping) in
   * order to synchronize with child's next event.
   *
   * @param type the type of continuation
   * @return a return code describing the outcome
   */
  ret_t resume(trace::resume_t type);

  /**
   * Convenience function to resume child execution and wait until the next
   * signal.  Equivalent to calling resume(Syscall) followed by wait().
   *
   * @return a return code describing the outcome
   */
  ret_t continueToNextSignal();

  /**
   * Convenience function to resume child execution and wait until either the
   * next signal or syscall boundary (either going into or coming out of
   * kernel).  Equivalent to calling resume(Continue) followed by wait().
   *
   * @return a return code describing the outcome
   */
  ret_t continueToNextSignalOrSyscall();

  /**
   * Convenience function to single-step the child and wait until it stops.
   * Equivalent to calling resume(SingleStep) followed by wait().
   *
   * @return a return code describing the outcome
   */
  ret_t singleStep();

  /**
   * Attach to a child for tracing.  Does not stop the child.
   * @return a return code describing the outcome
   */
  ret_t attach();

  /**
   * Attach to a child for tracing which was previously detached via
   * detachHandoff().  Upon successfully attaching, child is in stopped state.
   * @return a return code describing the outcome
   */
  ret_t attachHandoff();

  /**
   * Detach from a child & clean up internal state; the process object can be
   * re-used with the same arguments if needed.  If the child process is still
   * running, it will continue untraced.  This *always* succeeds from the the
   * tracer's viewpoint.
   *
   * @return a return code describing the outcome
   */
  ret_t detach();

  /**
   * Detach from child in preparation to hand off tracing to another thread.
   * @return a return code describing the outcome
   */
  ret_t detachHandoff();

  /////////////////////////////////////////////////////////////////////////////
  // Inspect & modify process state
  /////////////////////////////////////////////////////////////////////////////

  /**
   * Process information - return what you ask for.
   */
  int getArgc() const { return argc; }
  char **getArgv() const { return argv; }
  pid_t getPid() const { return pid; }
  const urange_t &getStackBounds() const { return stackBounds; }
  status_t getStatus() const { return status; }
  void setStatus(status_t status) { this->status = status; }
  int getUserfaultfd() const { return uffd; }
  size_t getNumThreads() const { return nthreads; }
  struct parasite_ctl *getParasiteCtl() { return parasite; }

  /**
   * Convenience function to return whether the process is in a traceable
   * state, i.e., it is at a trace-stop and ptrace calls will succeed.
   *
   * @return true if the process is in a traceable state or false otherwise
   */
  bool traceable() const { return status == Stopped || status == Interrupted; }

  /**
   * Get the PID of the newly forked/cloned task if child previously stopped on
   * a clone() or fork() event.
   * @return PID of the new task, or INT32_MAX if not stopped at a task
   *         creation event
   */
  pid_t getNewTaskPid() const;

  /* Note: the following APIs may only be called when the process is stopped */

  /**
   * Get the exit code after the child exits normally.
   * @return exit code if status == Exited, INT32_MAX otherwise
   */
  int getExitCode() const;

  /**
   * Get the signal that caused the child to stop or terminate.
   * @return signal number if status == SignalExit/Stopped, INT32_MAX otherwise
   */
  int getSignal() const;

  /**
   * Get the type of event that caused the child to stop.
   * @return the type of event that caused the child to stop
   */
  stop_t getStopReason() const;

  /**
   * Return whether the child stopped at a system call boundary or not.
   * @return true if the process is stopped at a system call boundary or false
   *         if stopped for another reason (or is not stopped)
   */
  bool stoppedAtSyscall() const { return getSignal() == SIGTRAP; }

  /**
   * Read general purpose registers.
   * @param regs a register set to be populated with the child's registers
   * @return a return code describing the outcome
   */
  ret_t readRegs(struct user_regs_struct &regs) const;

  /**
   * Read floating-point registers.
   * @param regs a floating-point register set to be populated with the child's
   *             registers
   * @return a return code describing the outcome
   */
  ret_t readFPRegs(struct user_fpregs_struct &regs) const;

  /**
   * Write general purpose registers.
   * @param regs a register set with which to set the child's registers
   * @return a return code describing the outcome
   */
  ret_t writeRegs(struct user_regs_struct &regs) const;

  /**
   * Write floating-point registers.
   * @param regs a floating-point register set with which to set the child's
   *             float-point registers
   * @return a return code describing the outcome
   */
  ret_t writeFPRegs(struct user_fpregs_struct &regs) const;

  /**
   * Get the process' current program counter.
   * @return the program counter or 0 if it could not be retrieved
   */
  uintptr_t getPC() const;

  /**
   * Set the process' current program counter.
   * @return a return code describing the outcome
   */
  ret_t setPC(uintptr_t newPC) const;

  /**
   * Get the process' current stack pointer.
   * @return the stack pointer of 0 if it could not be retrieved
   */
  uintptr_t getSP() const;

  /**
   * Set the process' current stack pointer.
   * @return a return code describing the outcome
   */
  ret_t setSP(uintptr_t newSP) const;

  /**
   * Marshal a set of arguments into registers to invoke a function call
   * according to the ISA-specific calling convention.
   * @param a1-6 arguments to the system call
   * @return a return code describing the outcome
   */
  ret_t setFuncCallRegs(long a1 = 0, long a2 = 0, long a3 = 0,
                        long a4 = 0, long a5 = 0, long a6 = 0) const;

  /**
   * Read 8 bytes of data from the child's memory.
   * @param addr the address to read
   * @param data output argument to which bytes will be written
   * @return a return code describing the outcome
   */
  ret_t read(uintptr_t addr, uint64_t &data) const;

  /**
   * Read a region of memory into a buffer.
   * @param addr virtual address to begin reading
   * @param buffer data buffer into which data is read
   * @return a return code describing the outcome
   */
  ret_t readRegion(uintptr_t addr, byte_iterator &buffer) const;

  /**
   * Print 8 bytes of data from child's memory.
   * @param addr the address to read
   */
  void dumpMem(uintptr_t addr) const;

  /**
   * Write 8 bytes of data to the child's memory.
   * @param addr the address to write
   * @param data bytes to write to the address
   * @return a return code describing the outcome
   */
  ret_t write(uintptr_t addr, uint64_t data) const;

  /**
   * Write a region of memory from a buffer to child memory.
   * @param addr virtual address to begin writing
   * @param buffer data buffer of data to be written
   * @return a return code describing the outcome
   */
  ret_t writeRegion(uintptr_t addr, const byte_iterator &buffer) const;

  /**
   * Get the system call number.  Caller must ensure Process trace-stopped.  If
   * not stopped at syscall-enter-stop (i.e., entering the kernel for the
   * system call), returns garbage data.
   *
   * @param data output argument set to the system call number
   * @param a return code describing the outcome
   */
  ret_t getSyscallNumber(long &data) const;

  /**
   * Dump register contents to an output stream.
   * @param os an output stream
   */
  void dumpRegs(std::ostream &os) const;

  /**
   * Initialize the userfaultfd in the context of the child and send it to
   * Chameleon.  After calling, users can query the file descriptor using
   * getUserfaultfd().
   *
   * @return a return code describing the outcome
   */
  ret_t stealUserfaultfd();

private:
  /* Arguments */
  int argc;
  char **argv;

  /* Process information */
  pid_t pid;
  int memFD; /* File descriptor for child's memory (/proc/<PID>/mem) */
  urange_t stackBounds;
  pid_t newTaskPid; /* new PID of the cloned/forked child */
  status_t status;
  union {
    int exit;   /* exit code if status == Exited */
    int signal; /* exit/stop signal if status == SignalExit or Stopped */
  };
  stop_t stopReason; /* if status == Stopped, why we stopped */
  bool reinjectSignal; /* whether to re-inject signal into tracee */
  sigset_t intSet; /* signal mask when chameleon's thread was interrupted */
  int uffd; /* userfaultfd file descriptor */
  size_t nthreads; /* number of threads in the process */
  struct parasite_ctl *parasite; /* libcompel handle for child parasite */
  sem_t handoff; /* coordinate handing off tracing */

  /**
   * Internal wait implementation used to save relevant information depending
   * on whether we need to forward a signal to the child.  If we're waiting for
   * a signal to be delivered that we sent (either directly or indirectly) then
   * we know we don't need to forward it to the child.  In the general case
   * though, when we restart the child we want to forward the signal we
   * intercepted.
   *
   * @param reinject whether or not to reinject a signal
   * @return a return code describing the outcome
   */
  ret_t waitInternal(bool reinject);

  /**
   * Initialize the child's stack by pre-touching all stack pages in
   * preparation for re-randomization.  The child's stack bounds are stored in
   * the stackBounds field.
   *
   * @return a return code describing the outcome
   */
  ret_t initializeStack();

  /**
   * Open a file descriptor which can be used to directly read/write the child
   * process' address space.
   * @return a return code describing the outcome
   */
  ret_t initializeMemFD();

  /**
   * Cure the compel parasite and initialize another for the next compel
   * action.  compel splits parasite setup into initialization & infection, but
   * couples curing & free.  In order to always have a parasite ready to go,
   * whenever we cure go ahead and initialize the parasite again.
   *
   * @return a return code describing the outcome
   */
  ret_t cureAndInitParasite();
};

}

#endif /* _PROCESS_H */

