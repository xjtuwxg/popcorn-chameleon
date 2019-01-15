#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>

#include "log.h"
#include "transform.h"
#include "utils.h"

using namespace chameleon;

///////////////////////////////////////////////////////////////////////////////
// Fault handling
///////////////////////////////////////////////////////////////////////////////

static volatile bool faultHandlerExit = false;

/**
 * Set up signal handler for SIGINT.  Required because the calls to read() in
 * handleFaultsAsync are blocking.
 * @return true if successfully initialized or false otherwise
 */
static inline bool setSignalHandler() {
  auto sigHandler = [](int signal) {};
  struct sigaction handler;
  handler.sa_handler = sigHandler;
  if(sigaction(SIGINT, &handler, nullptr) == -1) return false;
  return true;
}

/**
 * Handle a fault, including mapping in the correct data and randomizing any
 * code pieces.
 *
 * @param CT code transformer
 * @param uffd userfaultfd file descriptor for user-space fault handling
 * @param msg description of faulting region
 * @return a return code describing the outcome
 */
static std::vector<char> pageBuf(PAGESZ);
static inline ret_t
handleFault(CodeTransformer *CT, int uffd, const struct uffd_msg &msg) {
  uintptr_t pageAddr = PAGE_DOWN(msg.arg.pagefault.address), data;
  ret_t code = ret_t::Success;

  assert(msg.event == UFFD_EVENT_PAGEFAULT && "Invalid message type");
  DEBUGMSG("handling fault @ 0x" << std::hex << msg.arg.pagefault.address
           << ", flags=" << msg.arg.pagefault.flags << ", ptid=" << std::dec
           << msg.arg.pagefault.feat.ptid << std::endl);

  if(!(data = CT->zeroCopy(pageAddr))) {
    if((code = CT->project(pageAddr, pageBuf)) != ret_t::Success) return code;
    data = (uintptr_t)&pageBuf[0];
  }

  if(!uffd::copy(uffd, data, pageAddr)) code = ret_t::UffdCopyFailed;

  return code;
}

/**
 * Fault handling event loop.  Runs asynchronously to main application.
 * @param arg pointer to CodeTransformer object
 * @return nullptr always
 */
static void *handleFaultsAsync(void *arg) {
  CodeTransformer *CT = (CodeTransformer *)arg;
  int uffd = CT->getUserfaultfd();
  size_t nfaults = CT->getNumFaultsBatched(), toHandle, i, handled = 0;
  ssize_t bytesRead;
  pid_t me = syscall(SYS_gettid);
  struct uffd_msg *msg = new struct uffd_msg[nfaults];
  Timer t;

  assert(CT && "Invalid CodeTransformer object");
  assert(uffd >= 0 && "Invalid userfaultfd file descriptor");
  assert(msg && "Page fault message buffer allocation failed");

  if(!setSignalHandler())
    ERROR("could not initialize cleanup signal handler" << std::endl);
  CT->setFaultHandlerPid(me);

  DEBUGMSG("fault handler " << me << ": reading from uffd=" << uffd
           << ", batching " << nfaults << " fault(s)" << std::endl);

  while(!faultHandlerExit) {
    bytesRead = read(uffd, msg, sizeof(struct uffd_msg) * nfaults);
    if(bytesRead >= 0) {
      t.start();
      toHandle = bytesRead / sizeof(struct uffd_msg);
      for(i = 0; i < toHandle; i++) {
        // TODO for Linux 4.11+, handle UFFD_EVENT_FORK, UFFD_EVENT_REMAP,
        // UFFD_EVENT_REMOVE, UFFD_EVENT_UNMAP
        if(msg[i].event != UFFD_EVENT_PAGEFAULT) continue;
        if(handleFault(CT, uffd, msg[i]) != ret_t::Success) {
          INFO("could not handle fault, limping ahead..." << std::endl);
          continue;
        }
        handled++;
      }
      t.end(true);
      DEBUGMSG_VERBOSE("fault handling time: " << t.elapsed(Timer::Micro)
                       << " us for " << toHandle << " fault(s)" << std::endl);
    }
    else if(errno != EINTR) DEBUGMSG("read failed (return=" << bytesRead
                                     << "), trying again..." << std::endl);
  }
  delete [] msg;

  DEBUGMSG("fault handler " << me << " exiting" << std::endl);
  INFO("Total fault handling time: " << t.totalElapsed(Timer::Micro)
       << " us for " << handled << " fault(s)" << std::endl);

  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// CodeTransformer implementation
///////////////////////////////////////////////////////////////////////////////

CodeTransformer::~CodeTransformer() {
  faultHandlerExit = true;
  proc.detach(); // detaching closes the userfaultfd file descriptor
  if(faultHandlerPid > 0) {
    // Interrupt the fault handling thread if the thread was already blocking
    // on a read before closing the userfaultfd file descriptor
    syscall(SYS_tgkill, masterPID, faultHandlerPid, SIGINT);
    pthread_join(faultHandler, nullptr);
  }
}

ret_t CodeTransformer::initialize(bool randomize) {
  ret_t retcode;

  if(batchedFaults != 1) {
    DEBUGMSG("currently can only handle 1 fault at a time" << std::endl);
    return ret_t::InvalidTransformConfig;
  }

  // Try to give the user some warning for excessive stack padding
  if(slotPadding >= PAGESZ)
    WARN("Large padding added between slots: " << slotPadding << std::endl);

  if((retcode = binary.initialize()) != ret_t::Success) return retcode;
  if((retcode = arch::initDisassembler()) != ret_t::Success)
    return retcode;
  const Binary::Section &codeSec = binary.getCodeSection();
  const Binary::Segment &codeSeg = binary.getCodeSegment();

  // Initialize code & randomize (if requested) to serve initial faults
  retcode = populateCodeWindow(codeSec, codeSeg);
  if(retcode != ret_t::Success) return retcode;
  if(randomize) {
    retcode = analyzeFunctions();
    if(retcode != ret_t::Success) return retcode;
    retcode = randomizeFunctions();
    if(retcode != ret_t::Success) return retcode;
  }

  // Prepare the code region inside the binary by setting up correct page
  // permissions and registering the code with the userfaultfd file descriptor
  parasite = parasite::initialize(proc.getPid());
  if(!parasite) return ret_t::CompelInitFailed;
  retcode = remapCodeSegment(codeSec.address(), codeSec.size());
  if(retcode != ret_t::Success) return retcode;
  retcode = proc.stealUserfaultfd(parasite);
  if(retcode != ret_t::Success) return retcode;
  if(!uffd::api(proc.getUserfaultfd(), nullptr, nullptr))
    return ret_t::UffdHandshakeFailed;
  if(!uffd::registerRegion(proc.getUserfaultfd(),
                           codeSec.address(),
                           codeSec.size()))
    return ret_t::UffdRegisterFailed;

  // Initialize thread for handling faults
  if(pthread_create(&faultHandler, nullptr, handleFaultsAsync, this))
    return ret_t::FaultHandlerFailed;

  return ret_t::Success;
}

/* Adjust offset based on stack growth direction */
#if STACK_DIRECTION == DOWN
# define DIRECTION( offset ) (-(offset))
#else
# define DIRECTION( offset ) (offset)
#endif

int32_t CodeTransformer::canonicalizeSlotOffset(uint32_t frameSize,
                                                arch::RegType reg,
                                                int16_t offset) {
  switch(reg) {
  case arch::RegType::FramePointer:
    return DIRECTION(arch::framePointerOffset() + offset);
  case arch::RegType::StackPointer:
    return (int32_t)frameSize + DIRECTION(offset);
  default: return INT32_MAX;
  }
}

int32_t CodeTransformer::slotOffsetFromRegister(uint32_t frameSize,
                                                arch::RegType reg,
                                                int16_t offset) {
  switch(reg) {
  case arch::RegType::FramePointer:
    return DIRECTION(offset) - arch::framePointerOffset();
  case arch::RegType::StackPointer:
    return DIRECTION(offset - (int32_t)frameSize);
  default: return INT32_MAX;
  }
}

ret_t CodeTransformer::remapCodeSegment(uintptr_t start, uint64_t len) {
  uintptr_t pageStart;
  size_t roundedLen;
  int prot, flags;
  long mmapRet;

  // TODO: for some reason we need to do a dummy syscall to get the child
  // into a state controllable by us/compel.  Maybe related to not getting into
  // the correct ptrace-stop mode before running the parasite? ¯\_(ツ)_/¯
  if(parasite::syscall(parasite, SYS_getpid, mmapRet) != ret_t::Success)
    return ret_t::CompelActionFailed;

  DEBUGMSG("changing child's code section anonymous private mapping for "
           "userfaultfd" << std::endl);

  // For some reason the kernel returns a SIGSEGV to compel as a result of the
  // mmap and thus compel returns an error code, but the mmap call succeeds and
  // compel restores the thread correctly; just mask the error.
  pageStart = PAGE_DOWN(start);
  roundedLen = PAGE_ALIGN_LEN(start, len);
  prot = PROT_EXEC | PROT_READ;
  flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
  parasite::syscall(parasite, SYS_mmap, mmapRet, pageStart, roundedLen, prot,
                    flags, -1, 0);
  if((uintptr_t)mmapRet != pageStart) return ret_t::RemapCodeFailed;

  DEBUGMSG("remapped 0x" << std::hex << pageStart << " - 0x"
           << (pageStart + roundedLen) << std::endl);

  return ret_t::Success;
}

ret_t CodeTransformer::populateCodeWindow(const Binary::Section &codeSection,
                                          const Binary::Segment &codeSegment) {
  uintptr_t segStart, curAddr;
  ssize_t len, filelen;
  byte_iterator data;
  MemoryRegionPtr r;
  Timer t;
  t.start();

  // Note: by construction of how we're adding regions we don't need to call
  // codeWindow.sort() to sort the regions within the window.

  // Calculate the first address we care about. Note that we *only* care about
  // pages with code, i.e., the code segment may contain other sections that
  // are on different pages that don't concern us.
  codeWindow.clear();
  segStart = codeSegment.address();
  codeStart = codeSection.address();
  curAddr = std::max<uintptr_t>(PAGE_DOWN(codeStart), segStart);

  // First, check if the segment contains data before the code section.  Note
  // that the region must be entirely contained on-disk (i.e., no zero-filled
  // region so file length = memory length) because segments can't have holes
  // and we know the subsequent code section *must* be on-disk.
  len = codeStart - curAddr;
  if(len > 0) {
    if(binary.getRemainingFileSize(curAddr, codeSegment) <= (size_t)len) {
      WARN("Invalid file format - found holes in segment" << std::endl);
      return ret_t::InvalidElf;
    }
    data = binary.getData(curAddr, codeSegment);
    if(!data) return ret_t::MarshalDataFailed;
    r.reset(new FileRegion(curAddr, len, len, data));
    codeWindow.insert(r);
  }
  else if(len != 0) {
    WARN("Invalid file format - segment start address is after code section "
         "start address" << std::endl);
    return ret_t::InvalidElf;
  }

  // Now, add a region for the code section
  len = codeSection.size();
  filelen = binary.getRemainingFileSize(codeStart, codeSegment);
  if(filelen < len)
    WARN("Code section on-disk smaller than in-memory representation ("
         << filelen << " vs " << codeSection.size() << " bytes)" << std::endl);
  data = binary.getData(codeStart, codeSegment);
  if(!data) return ret_t::MarshalDataFailed;
  r.reset(new BufferedRegion(codeStart, len, filelen, data));
  codeWindow.insert(r);

  // Finally, add any segment data/zeroed memory after the code section
  codeEnd = codeStart + len;
  curAddr = PAGE_UP(codeEnd);
  len = curAddr - codeEnd;
  filelen = binary.getRemainingFileSize(codeEnd, codeSegment);
  data = binary.getData(codeEnd, codeSegment);
  r.reset(new FileRegion(codeEnd, len, filelen, data));
  codeWindow.insert(r);

  t.end();
  INFO("Code window setup time: " << t.elapsed(Timer::Micro) << " us"
       << std::endl);

  return ret_t::Success;
}

/**
 * Return the canonicalized stack offset of an operand if it's a base +
 * displacement memory reference into the stack.  If it's not a memory
 * reference or not a reference to the stack, return 0.
 *
 * @param frameSize current frame size as we're walking through the function
 * @param op a DynamoRIO operand
 * @param type output operand specifying base register type
 * @return the canonicalized offset or 0 if not a stack slot reference
 */
static inline int getStackOffset(uint32_t frameSize,
                                 opnd_t op,
                                 arch::RegType &type) {
  // TODO Note: from dr_ir_opnd.h about opnd_get_disp():
  //   "On ARM, the displacement is always a non-negative value, and the
  //   presence or absence of #DR_OPND_NEGATED in opnd_get_flags() determines
  //   whether to add or subtract from the base register"
  int offset = 0;
  if(opnd_is_base_disp(op)) {
    type = arch::getRegTypeDR(opnd_get_base(op));
    offset = opnd_get_disp(op);
    offset = CodeTransformer::canonicalizeSlotOffset(frameSize, type, offset);
    if(offset <= 0 || offset > (int)frameSize) offset = 0;
  }
  return offset;
}

template<int (*NumOp)(instr_t *),
         opnd_t (*GetOp)(instr_t *, unsigned)>
ret_t CodeTransformer::analyzeOperands(RandomizedFunctionPtr &info,
                                       uint32_t frameSize,
                                       instr_t *instr) {
  int i, offset;
  opnd_t op;
  arch::RegType type;
  RandRestriction res;
  ret_t code = ret_t::Success;

  for(i = 0; i < NumOp(instr); i++) {
    op = GetOp(instr, i);
    offset = getStackOffset(frameSize, op, type);
    if(offset && arch::getRestriction(instr, op, res)) {
      res.offset = offset;
      if((code = info->addRestriction(res)) != ret_t::Success) return code;
    }
  }

  return ret_t::Success;
}

ret_t CodeTransformer::analyzeFunction(RandomizedFunctionPtr &info) {
  int32_t update, offset;
  uint32_t frameSize = arch::initialFrameSize();
  size_t instrSize;
  const function_record *func = info->getFunctionRecord();
  byte_iterator funcData = codeWindow.getData(func->addr);
  byte *real = (byte *)func->addr, *cur = funcData[0], *prev,
       *end = cur + func->code_size;
  instrlist_t *instrs;
  instr_t *instr;
  reg_id_t drsp;
  RandRestriction res;
  ret_t code = ret_t::Success;

  if(funcData.getLength() < func->code_size) {
    DEBUGMSG("code length encoded in metadata larger than available size: "
             << funcData.getLength() << " vs. " << func->code_size
             << std::endl);
    return ret_t::BadMetadata;
  }

  if(!cur) {
    DEBUGMSG("invalid code iterator" << std::endl);
    return ret_t::RandomizeFailed;
  }

  // Construct a list of instructions & analyze for restrictions.
  // instr_create() allocates the instruction on DynamoRIO's heap; the info
  // object will be given ownership of the instructions after analysis and will
  // free them as needed.
  instrs = instrlist_create(GLOBAL_DCONTEXT);
  drsp = arch::getDRRegType(arch::RegType::StackPointer);
  while(cur < end) {
    prev = cur;
    instr = instr_create(GLOBAL_DCONTEXT);
    instr_init(GLOBAL_DCONTEXT, instr);
    cur = decode_from_copy(GLOBAL_DCONTEXT, cur, real, instr);
    if(!cur) {
      code = ret_t::AnalysisFailed;
      goto out;
    }
    instrSize = cur - prev;
    instr_set_raw_bits(instr, prev, instrSize);
    real += instrSize;
    instrlist_append(instrs, instr);

    DEBUG_VERBOSE(DEBUGMSG_INSTR("size = " << instrSize << ": ", instr);)

    code = analyzeOperands<instr_num_srcs, instr_get_src>
                          (info, frameSize, instr);
    if(code != ret_t::Success) goto out;
    code = analyzeOperands<instr_num_dsts, instr_get_dst>
                          (info, frameSize, instr);
    if(code != ret_t::Success) goto out;

    // Check if possible to rewrite frame allocation instructions with a random
    // size; if not, mark the associated stack slot as immovable.  To determine
    // the associated slot, keep track of the frame's size as it's expanded
    // (prologue) and shrunk (epilogue) to canonicalize stack slot references.
    if(instr_writes_to_reg(instr, drsp, DR_QUERY_DEFAULT)) {
      update = arch::getFrameUpdateSize(instr);
      if(update) {
        DEBUGMSG_VERBOSE(" -> stack pointer update: " << update
                         << " (current size = " << frameSize + update << ")"
                         << std::endl);

        if(arch::getRestriction(instr, res)) {
          // If growing the frame, the referenced slot includes the update
          // whereas if we're shrinking the frame it doesn't.
          offset = (update > 0) ? update : 0;
          offset = canonicalizeSlotOffset(frameSize + offset,
                                          arch::RegType::StackPointer, 0);
          res.offset = offset;
          res.alignment = res.size = abs(update);
          if((code = info->addRestriction(res)) != ret_t::Success) goto out;
        }
        frameSize += update;
      }
    }
  }

  // Add the remaining slots, i.e., those that don't have any restrictions
  code = info->populateSlots();

  DEBUG_VERBOSE(
    if(frameSize != arch::initialFrameSize())
      DEBUGMSG(" -> function does not clean up frame (not intended to return?)"
               << std::endl);
  )

out:
  if(code == ret_t::Success) info->setInstructions(instrs);
  else instrlist_clear_and_destroy(GLOBAL_DCONTEXT, instrs);
  return code;
}

ret_t CodeTransformer::analyzeFunctions() {
  Timer t;
  ret_t code;

  // Analyze every function for which we have transformation metadata
  Binary::func_iterator it = binary.getFunctions(codeStart, codeEnd);
  for(; !it.end(); ++it) {
    const function_record *func = *it;

    DEBUGMSG("analyzing function @ " << std::hex << func->addr << ", size = "
             << std::dec << func->code_size << std::endl);
    t.start();

    RandomizedFunctionPtr info = arch::getRandomizedFunction(binary, func);
    RandomizedFunctionMap::iterator it =
      funcMaps.emplace(func->addr, std::move(info)).first;
    code = analyzeFunction(it->second);
    if(code != ret_t::Success) return code;

    t.end(true);
    DEBUGMSG_VERBOSE("analyzing function took " << t.elapsed(Timer::Micro)
                     << " us" << std::endl);
  }

  INFO("Total analyze time: " << t.totalElapsed(Timer::Micro) << " us"
       << std::endl);

  return ret_t::Success;
}

// TODO 1: we currently assume the compiler generates references to stack slots
// independently of other stack slots, i.e., it doesn't generate intermediate
// values which are then used to generate references to 2 or more slots.  This
// may not be true with increasing optimization levels, see X86OptimizeLEAs.cpp
// in newer versions of LLVM.
// TODO 2: references into arrays & structs also include an index and scale
// operand.  But we're only changing the beginning offset of the slot, so those
// operands should be okay as-is.  Verify this is true.
template<int (*NumOp)(instr_t *),
         opnd_t (*GetOp)(instr_t *, unsigned),
         void (*SetOp)(instr_t *, unsigned, opnd_t)>
ret_t CodeTransformer::randomizeOperands(const RandomizedFunctionPtr &info,
                                         uint32_t frameSize,
                                         uint32_t randFrameSize,
                                         instr_t *instr,
                                         bool &changed) {
  int i, offset, randOffset, randRegOffset;
  opnd_t op;
  enum arch::RegType type;

  for(i = 0; i < NumOp(instr); i++) {
    op = GetOp(instr, i);
    offset = getStackOffset(frameSize, op, type);
    if(offset && info->transformOffset(offset)) {
      randOffset = info->getRandomizedOffset(offset);
      if(randOffset == INT32_MAX) {
        DEBUGMSG_INSTR("couldn't find slot for offset " << offset << " in ",
                       instr);
        return ret_t::BadMetadata;
      }
      randRegOffset = slotOffsetFromRegister(randFrameSize, type, randOffset);
      opnd_set_disp_ex(&op, randRegOffset, false, false, false);
      SetOp(instr, i, op);
      changed = true;

      DEBUGMSG_VERBOSE(" -> remap stack offset " << offset << " -> "
                       << randOffset << std::endl);
    }
  }

  return ret_t::Success;
}

#ifdef DEBUG_BUILD
/**
 * Compare original instructions to transformed version and print any size
 * differences.
 *
 * @param pointer to real address of original instructions
 * @param pointer to starting address of original instructions
 * @param pointer to ending address of original instructions
 * @param transformedInstrs transformed instructions
 */
static void compareInstructions(byte *real,
                                byte *start,
                                byte *end,
                                instrlist_t *transformedInstrs) {
  int origLen, transLen;
  instr_t orig, *trans;
  byte *prev;

  instr_init(GLOBAL_DCONTEXT, &orig);
  trans = instrlist_first(transformedInstrs);
  while(start < end) {
    prev = start;
    instr_reset(GLOBAL_DCONTEXT, &orig);
    start = decode_from_copy(GLOBAL_DCONTEXT, start, real, &orig);
    if(!start) {
      DEBUGMSG("couldn't decode compareInstructions()" << std::endl);
      return;
    }
    origLen = start - prev;
    instr_set_raw_bits(&orig, prev, origLen);
    real += origLen;
    transLen = instr_length(GLOBAL_DCONTEXT, trans);
    if(transLen != origLen) {
      DEBUGMSG_INSTR("Changed size: " << transLen << " bytes, ", trans);
      DEBUGMSG_INSTR("              " << origLen << " bytes, ", &orig);
    }
    trans = instr_get_next(trans);
  }
}
#endif

ret_t CodeTransformer::randomizeFunction(RandomizedFunctionPtr &info) {
  bool changed;
  int32_t update, offset, instrSize;
  uint32_t frameSize = arch::initialFrameSize(),
           randFrameSize = arch::initialFrameSize(),
           count = 0;
  const function_record *func = info->getFunctionRecord();
  byte_iterator funcData = codeWindow.getData(func->addr);
  byte *real = (byte *)func->addr, *cur = funcData[0];
  instrlist_t *instrs = info->getInstructions();
  instr_t *instr;
  reg_id_t drsp;
  ret_t code;
#ifdef DEBUG_BUILD
  byte *prev;
#endif

  // Randomize the function's layout according to the metadata
  code = info->randomize(rng(), slotPadding);
  if(code != ret_t::Success) return code;

  // Apply the randomization by rewriting instructions
  instr = instrlist_first(instrs);
  drsp = arch::getDRRegType(arch::RegType::StackPointer);
  while(instr) {
    changed = false;
    instrSize = instr_length(GLOBAL_DCONTEXT, instr);

    DEBUG_VERBOSE(DEBUGMSG_INSTR("size = " << instrSize << ": ", instr);)

    // Rewrite stack slot reference operands to their randomized locations
    code = randomizeOperands<instr_num_srcs, instr_get_src, instr_set_src>
                          (info, frameSize, randFrameSize, instr, changed);
    if(code != ret_t::Success) return code;
    code = randomizeOperands<instr_num_dsts, instr_get_dst, instr_set_dst>
                          (info, frameSize, randFrameSize, instr, changed);
    if(code != ret_t::Success) return code;

    // Allow each ISA-specific randomized function to have its way
    code = info->transformInstr(frameSize, randFrameSize, instr, changed);
    if(code != ret_t::Success) return code;

    // Keep track of stack pointer updates & rewrite frame update instructions
    // with randomized size
    if(instr_writes_to_reg(instr, drsp, DR_QUERY_DEFAULT)) {
      update = arch::getFrameUpdateSize(instr);
      if(update) {
        offset = (update > 0) ? update : 0;
        offset = canonicalizeSlotOffset(frameSize + offset,
                                        arch::RegType::StackPointer, 0);
        if(info->transformBulkFrameUpdate(offset) &&
           offset <= (int)func->frame_size) {
          offset = info->getRandomizedBulkFrameUpdate();
          offset = update > 0 ? offset : -offset;
          code = arch::rewriteFrameUpdate(instr, offset, changed);
          if(code != ret_t::Success) return code;
          randFrameSize += offset;

          DEBUGMSG_VERBOSE(" -> rewrite frame update: " << update << " -> "
                           << offset << std::endl);
        }
        else randFrameSize += update;
        frameSize += update;
      }
    }

    // If we changed anything, re-encode the instruction.  Note that
    // randomization *may* change the size of individual instructions; the net
    // code size *must* be identical.
    if(changed) {
      DEBUG_VERBOSE(prev = cur);
      cur = instr_encode_to_copy(GLOBAL_DCONTEXT, instr, cur, real);
      if(!cur) return ret_t::RandomizeFailed;

      count++;
      DEBUG_VERBOSE(
        if(instrSize != (cur - prev))
          DEBUGMSG_VERBOSE(" -> changed size of instruction: " << instrSize
                           << " vs. " << (cur - prev) << std::endl);
        DEBUGMSG_INSTR(" -> rewrote: ", instr)
      );
    }
    else cur += instrSize;
    real += instrSize;

    instr = instr_get_next(instr);
  }

  if((uintptr_t)real != (func->addr + func->code_size)) {
    WARN("changed size of function's instructions" << std::endl);
    DEBUG(compareInstructions((byte *)func->addr, funcData[0],
                              funcData[0] + func->code_size, instrs));
    return ret_t::RandomizeFailed;
  }

  DEBUGMSG("rewrote " << count << " instruction(s)" << std::endl);

  return ret_t::Success;
}

ret_t CodeTransformer::randomizeFunctions() {
  Timer t;
  ret_t code;

  for(auto &it : funcMaps) {
    RandomizedFunctionPtr &info = it.second;

    DEBUG(
      const function_record *func = info->getFunctionRecord();
      DEBUGMSG("randomizing function @ " << std::hex << func->addr
               << ", size = " << std::dec << func->code_size << std::endl);
    )
    t.start();

    code = randomizeFunction(info);
    if(code != ret_t::Success) return code;

    t.end(true);
    DEBUGMSG_VERBOSE("randomizing function took " << t.elapsed(Timer::Micro)
                     << " us" << std::endl);
  }

  INFO("Randomization time: " << t.totalElapsed(Timer::Micro) << " us"
       << std::endl);

  return ret_t::Success;
}

