/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "native_stack_dump.h"

#include <memory>
#include <ostream>

#include <stdio.h>

#include "art_method.h"

// For DumpNativeStack.
#include <backtrace/Backtrace.h>
#include <backtrace/BacktraceMap.h>

// See comment around line ~80 for more information.
#define MEMFAULT_DUMP_NATIVE_STACK (1)

#if MEMFAULT_DUMP_NATIVE_STACK
#include <unwindstack/Regs.h>
#include <unwindstack/Unwinder.h>
#endif

#if defined(__linux__)

#include <memory>
#include <vector>

#include <linux/unistd.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>

#if MEMFAULT_DUMP_NATIVE_STACK
#include <semaphore.h>
#include "android-base/threads.h"
#endif
#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "arch/instruction_set.h"
#include "base/aborting.h"
#include "base/bit_utils.h"
#include "base/file_utils.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "class_linker.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "oat_quick_method_header.h"
#include "runtime.h"
#include "thread-current-inl.h"

#endif

namespace art {

#if defined(__linux__)

using android::base::StringPrintf;

#if MEMFAULT_DUMP_NATIVE_STACK

// This patch implements an alternative DumpNativeStack() implementation.
// In contrast to the libart's original code, this will also dump the GNU Build IDs for each frame,
// which is necessary to be able to desymbolicate the frames accurately.
// The implementation is based on code from tombstone.cpp and BacktraceCurrent.cpp
// from the platform/system/core repo.

// The signal used to cause a thread to dump the stack.
#if defined(__GLIBC__)
// In order to run the backtrace_tests on the host, we can't use
// the internal real time signals used by GLIBC. To avoid this,
// use SIGRTMIN for the signal to dump the stack.
#define THREAD_SIGNAL SIGRTMIN
#else
#define THREAD_SIGNAL (__SIGRTMIN+1)
#endif

using unwindstack::Regs;
using unwindstack::UnwinderFromPid;

static pthread_mutex_t g_with_registers_from_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::unique_ptr<Regs> g_registers_from_thread;
static sem_t g_registers_obtained_semph;

// Signal handler which only job is to grab the registers of the top of this thread:
static void SignalHandler(int, siginfo_t*, void* sigcontext) {
  ucontext_t* ucontext_ptr = reinterpret_cast<ucontext_t*>(sigcontext);
  g_registers_from_thread.reset(Regs::CreateFromUcontext(Regs::CurrentArch(), ucontext_ptr));

  // Unblock WithRegistersFromThread:
  sem_post(&g_registers_obtained_semph);
}

static void WithRegistersFromThread(std::ostream& os, pid_t tid, const char* prefix,
    const std::function<void (std::ostream& os, pid_t tid, const char* prefix, Regs *regs)>& func) {

  pthread_mutex_lock(&g_with_registers_from_thread_mutex);

  if (sem_init(&g_registers_obtained_semph, 0, 0) != 0) {
    os << "sem_init failed" << std::endl;
    func(os, tid, prefix, nullptr);
    goto unlock;
  }

  // Install SignalHandler:
  struct sigaction act, oldact;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = SignalHandler;
  act.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&act.sa_mask);
  if (sigaction(THREAD_SIGNAL, &act, &oldact) != 0) {
    os << "sigaction failed" << std::endl;
    func(os, tid, prefix, nullptr);
    goto unlock;
  }

  // Send the signal:
  if (tgkill(getpid(), tid, THREAD_SIGNAL) != 0) {
    os << "tgkill failed" << std::endl;
    func(os, tid, prefix, nullptr);
    goto restore_old_handler;
  }

  // Wait for the semaphore in SignalHandler to be given:
  if (sem_wait(&g_registers_obtained_semph) != 0) {
    os << "sem_wait failed" << std::endl;
    func(os, tid, prefix, nullptr);
    goto restore_old_handler;
  }

  func(os, tid, prefix, g_registers_from_thread.get());
  g_registers_from_thread.reset();

restore_old_handler:
  sigaction(THREAD_SIGNAL, &oldact, nullptr);
unlock:
  pthread_mutex_unlock(&g_with_registers_from_thread_mutex);
}

static void DumpNativeStackWithRegs(std::ostream& os, pid_t tid, const char* prefix, Regs *regs) {
    if (regs == nullptr) {
      os << "Failed to get registers for thread " << tid << std::endl;
      return;
    }

    constexpr size_t kMaxFrames = 256;
    UnwinderFromPid unwinder(kMaxFrames, getpid());
    if (!unwinder.Init(Regs::CurrentArch())) {
      os << "Failed to init unwinder object." << std::endl;
      return;
    }
    unwinder.SetRegs(regs);
    unwinder.Unwind();
    if (unwinder.NumFrames() == 0) {
      os << prefix << "(Unwind failed for thread " << tid << ")" << std::endl;
      return;
    }

    unwinder.SetDisplayBuildID(true);
    for (size_t i = 0; i < unwinder.NumFrames(); i++) {
      os << prefix << unwinder.FormatFrame(i) << std::endl;
    }
}

void DumpNativeStack(std::ostream& os,
                     pid_t tid,
                     BacktraceMap* existing_map ATTRIBUTE_UNUSED,
                     const char* prefix,
                     ArtMethod* current_method ATTRIBUTE_UNUSED,
                     void* ucontext_ptr,
                     bool skip_frames ATTRIBUTE_UNUSED) {
  if (ucontext_ptr) {
    std::unique_ptr<Regs> regs(Regs::CreateFromUcontext(Regs::CurrentArch(), ucontext_ptr));
    DumpNativeStackWithRegs(os, tid, prefix, regs.get());
    return;
  }

  WithRegistersFromThread(os, tid, prefix, &DumpNativeStackWithRegs);
}

#else // MEMFAULT_DUMP_NATIVE_STACK

static constexpr bool kUseAddr2line = !kIsTargetBuild;

std::string FindAddr2line() {
  if (!kIsTargetBuild) {
    constexpr const char* kAddr2linePrebuiltPath =
      "/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/bin/x86_64-linux-addr2line";
    const char* env_value = getenv("ANDROID_BUILD_TOP");
    if (env_value != nullptr) {
      return std::string(env_value) + kAddr2linePrebuiltPath;
    }
  }
  return std::string("/usr/bin/addr2line");
}

ALWAYS_INLINE
static inline void WritePrefix(std::ostream& os, const char* prefix, bool odd) {
  if (prefix != nullptr) {
    os << prefix;
  }
  os << "  ";
  if (!odd) {
    os << " ";
  }
}

// The state of an open pipe to addr2line. In "server" mode, addr2line takes input on stdin
// and prints the result to stdout. This struct keeps the state of the open connection.
struct Addr2linePipe {
  Addr2linePipe(int in_fd, int out_fd, const std::string& file_name, pid_t pid)
      : in(in_fd, false), out(out_fd, false), file(file_name), child_pid(pid), odd(true) {}

  ~Addr2linePipe() {
    kill(child_pid, SIGKILL);
  }

  File in;      // The file descriptor that is connected to the output of addr2line.
  File out;     // The file descriptor that is connected to the input of addr2line.

  const std::string file;     // The file addr2line is working on, so that we know when to close
                              // and restart.
  const pid_t child_pid;      // The pid of the child, which we should kill when we're done.
  bool odd;                   // Print state for indentation of lines.
};

static std::unique_ptr<Addr2linePipe> Connect(const std::string& name, const char* args[]) {
  int caller_to_addr2line[2];
  int addr2line_to_caller[2];

  if (pipe(caller_to_addr2line) == -1) {
    return nullptr;
  }
  if (pipe(addr2line_to_caller) == -1) {
    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    return nullptr;
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    close(addr2line_to_caller[0]);
    close(addr2line_to_caller[1]);
    return nullptr;
  }

  if (pid == 0) {
    dup2(caller_to_addr2line[0], STDIN_FILENO);
    dup2(addr2line_to_caller[1], STDOUT_FILENO);

    close(caller_to_addr2line[0]);
    close(caller_to_addr2line[1]);
    close(addr2line_to_caller[0]);
    close(addr2line_to_caller[1]);

    execv(args[0], const_cast<char* const*>(args));
    exit(1);
  } else {
    close(caller_to_addr2line[0]);
    close(addr2line_to_caller[1]);
    return std::make_unique<Addr2linePipe>(addr2line_to_caller[0],
                                           caller_to_addr2line[1],
                                           name,
                                           pid);
  }
}

static void Drain(size_t expected,
                  const char* prefix,
                  std::unique_ptr<Addr2linePipe>* pipe /* inout */,
                  std::ostream& os) {
  DCHECK(pipe != nullptr);
  DCHECK(pipe->get() != nullptr);
  int in = pipe->get()->in.Fd();
  DCHECK_GE(in, 0);

  bool prefix_written = false;

  for (;;) {
    constexpr uint32_t kWaitTimeExpectedMilli = 500;
    constexpr uint32_t kWaitTimeUnexpectedMilli = 50;

    int timeout = expected > 0 ? kWaitTimeExpectedMilli : kWaitTimeUnexpectedMilli;
    struct pollfd read_fd{in, POLLIN, 0};
    int retval = TEMP_FAILURE_RETRY(poll(&read_fd, 1, timeout));
    if (retval == -1) {
      // An error occurred.
      pipe->reset();
      return;
    }

    if (retval == 0) {
      // Timeout.
      return;
    }

    if (!(read_fd.revents & POLLIN)) {
      // addr2line call exited.
      pipe->reset();
      return;
    }

    constexpr size_t kMaxBuffer = 128;  // Relatively small buffer. Should be OK as we're on an
    // alt stack, but just to be sure...
    char buffer[kMaxBuffer];
    memset(buffer, 0, kMaxBuffer);
    int bytes_read = TEMP_FAILURE_RETRY(read(in, buffer, kMaxBuffer - 1));
    if (bytes_read <= 0) {
      // This should not really happen...
      pipe->reset();
      return;
    }
    buffer[bytes_read] = '\0';

    char* tmp = buffer;
    while (*tmp != 0) {
      if (!prefix_written) {
        WritePrefix(os, prefix, (*pipe)->odd);
        prefix_written = true;
      }
      char* new_line = strchr(tmp, '\n');
      if (new_line == nullptr) {
        os << tmp;

        break;
      } else {
        char saved = *(new_line + 1);
        *(new_line + 1) = 0;
        os << tmp;
        *(new_line + 1) = saved;

        tmp = new_line + 1;
        prefix_written = false;
        (*pipe)->odd = !(*pipe)->odd;

        if (expected > 0) {
          expected--;
        }
      }
    }
  }
}

static void Addr2line(const std::string& map_src,
                      uintptr_t offset,
                      std::ostream& os,
                      const char* prefix,
                      std::unique_ptr<Addr2linePipe>* pipe /* inout */) {
  DCHECK(pipe != nullptr);

  if (map_src == "[vdso]" || android::base::EndsWith(map_src, ".vdex")) {
    // addr2line will not work on the vdso.
    // vdex files are special frames injected for the interpreter
    // so they don't have any line number information available.
    return;
  }

  if (*pipe == nullptr || (*pipe)->file != map_src) {
    if (*pipe != nullptr) {
      Drain(0, prefix, pipe, os);
    }
    pipe->reset();  // Close early.

    std::string addr2linePath = FindAddr2line();
    const char* args[7] = {
        addr2linePath.c_str(),
        "--functions",
        "--inlines",
        "--demangle",
        "-e",
        map_src.c_str(),
        nullptr
    };
    *pipe = Connect(map_src, args);
  }

  Addr2linePipe* pipe_ptr = pipe->get();
  if (pipe_ptr == nullptr) {
    // Failed...
    return;
  }

  // Send the offset.
  const std::string hex_offset = StringPrintf("%zx\n", offset);

  if (!pipe_ptr->out.WriteFully(hex_offset.data(), hex_offset.length())) {
    // Error. :-(
    pipe->reset();
    return;
  }

  // Now drain (expecting two lines).
  Drain(2U, prefix, pipe, os);
}

static bool RunCommand(const std::string& cmd) {
  FILE* stream = popen(cmd.c_str(), "r");
  if (stream) {
    pclose(stream);
    return true;
  } else {
    return false;
  }
}

static bool PcIsWithinQuickCode(ArtMethod* method, uintptr_t pc) NO_THREAD_SAFETY_ANALYSIS {
  const void* entry_point = method->GetEntryPointFromQuickCompiledCode();
  if (entry_point == nullptr) {
    return pc == 0;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->IsQuickGenericJniStub(entry_point) ||
      class_linker->IsQuickResolutionStub(entry_point) ||
      class_linker->IsQuickToInterpreterBridge(entry_point)) {
    return false;
  }
  // The backtrace library might have heuristically subracted instruction
  // size from the pc, to pretend the pc is at the calling instruction.
  if (reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc()) - pc <= 4) {
    return false;
  }
  uintptr_t code = reinterpret_cast<uintptr_t>(EntryPointToCodePointer(entry_point));
  uintptr_t code_size = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].GetCodeSize();
  return code <= pc && pc <= (code + code_size);
}

void DumpNativeStack(std::ostream& os,
                     pid_t tid,
                     BacktraceMap* existing_map,
                     const char* prefix,
                     ArtMethod* current_method,
                     void* ucontext_ptr,
                     bool skip_frames) {
  // Historical note: This was disabled when running under Valgrind (b/18119146).

  BacktraceMap* map = existing_map;
  std::unique_ptr<BacktraceMap> tmp_map;
  if (map == nullptr) {
    tmp_map.reset(BacktraceMap::Create(getpid()));
    map = tmp_map.get();
  }
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(BACKTRACE_CURRENT_PROCESS, tid, map));
  backtrace->SetSkipFrames(skip_frames);
  if (!backtrace->Unwind(0, reinterpret_cast<ucontext*>(ucontext_ptr))) {
    os << prefix << "(backtrace::Unwind failed for thread " << tid
       << ": " <<  backtrace->GetErrorString(backtrace->GetError()) << ")" << std::endl;
    return;
  } else if (backtrace->NumFrames() == 0) {
    os << prefix << "(no native stack frames for thread " << tid << ")" << std::endl;
    return;
  }

  // Check whether we have and should use addr2line.
  bool use_addr2line;
  if (kUseAddr2line) {
    // Try to run it to see whether we have it. Push an argument so that it doesn't assume a.out
    // and print to stderr.
    use_addr2line = (gAborting > 0) && RunCommand(FindAddr2line() + " -h");
  } else {
    use_addr2line = false;
  }

  std::unique_ptr<Addr2linePipe> addr2line_state;

  for (Backtrace::const_iterator it = backtrace->begin();
       it != backtrace->end(); ++it) {
    // We produce output like this:
    // ]    #00 pc 000075bb8  /system/lib/libc.so (unwind_backtrace_thread+536)
    // In order for parsing tools to continue to function, the stack dump
    // format must at least adhere to this format:
    //  #XX pc <RELATIVE_ADDR>  <FULL_PATH_TO_SHARED_LIBRARY> ...
    // The parsers require a single space before and after pc, and two spaces
    // after the <RELATIVE_ADDR>. There can be any prefix data before the
    // #XX. <RELATIVE_ADDR> has to be a hex number but with no 0x prefix.
    os << prefix << StringPrintf("#%02zu pc ", it->num);
    bool try_addr2line = false;
    if (!BacktraceMap::IsValid(it->map)) {
      os << StringPrintf(Is64BitInstructionSet(kRuntimeISA) ? "%016" PRIx64 "  ???"
                                                            : "%08" PRIx64 "  ???",
                         it->pc);
    } else {
      os << StringPrintf(Is64BitInstructionSet(kRuntimeISA) ? "%016" PRIx64 "  "
                                                            : "%08" PRIx64 "  ",
                         it->rel_pc);
      if (it->map.name.empty()) {
        os << StringPrintf("<anonymous:%" PRIx64 ">", it->map.start);
      } else {
        os << it->map.name;
      }
      if (it->map.offset != 0) {
        os << StringPrintf(" (offset %" PRIx64 ")", it->map.offset);
      }
      os << " (";
      if (!it->func_name.empty()) {
        os << it->func_name;
        if (it->func_offset != 0) {
          os << "+" << it->func_offset;
        }
        // Functions found using the gdb jit interface will be in an empty
        // map that cannot be found using addr2line.
        if (!it->map.name.empty()) {
          try_addr2line = true;
        }
      } else if (current_method != nullptr &&
          Locks::mutator_lock_->IsSharedHeld(Thread::Current()) &&
          PcIsWithinQuickCode(current_method, it->pc)) {
        const void* start_of_code = current_method->GetEntryPointFromQuickCompiledCode();
        os << current_method->JniLongName() << "+"
           << (it->pc - reinterpret_cast<uint64_t>(start_of_code));
      } else {
        os << "???";
      }
      os << ")";
    }
    os << std::endl;
    if (try_addr2line && use_addr2line) {
      Addr2line(it->map.name, it->rel_pc, os, prefix, &addr2line_state);
    }
  }

  if (addr2line_state != nullptr) {
    Drain(0, prefix, &addr2line_state, os);
  }
}

#endif // MEMFAULT_DUMP_NATIVE_STACK

void DumpKernelStack(std::ostream& os, pid_t tid, const char* prefix, bool include_count) {
  if (tid == GetTid()) {
    // There's no point showing that we're reading our stack out of /proc!
    return;
  }

  std::string kernel_stack_filename(StringPrintf("/proc/self/task/%d/stack", tid));
  std::string kernel_stack;
  if (!ReadFileToString(kernel_stack_filename, &kernel_stack)) {
    os << prefix << "(couldn't read " << kernel_stack_filename << ")\n";
    return;
  }

  std::vector<std::string> kernel_stack_frames;
  Split(kernel_stack, '\n', &kernel_stack_frames);
  if (kernel_stack_frames.empty()) {
    os << prefix << "(" << kernel_stack_filename << " is empty)\n";
    return;
  }
  // We skip the last stack frame because it's always equivalent to "[<ffffffff>] 0xffffffff",
  // which looking at the source appears to be the kernel's way of saying "that's all, folks!".
  kernel_stack_frames.pop_back();
  for (size_t i = 0; i < kernel_stack_frames.size(); ++i) {
    // Turn "[<ffffffff8109156d>] futex_wait_queue_me+0xcd/0x110"
    // into "futex_wait_queue_me+0xcd/0x110".
    const char* text = kernel_stack_frames[i].c_str();
    const char* close_bracket = strchr(text, ']');
    if (close_bracket != nullptr) {
      text = close_bracket + 2;
    }
    os << prefix;
    if (include_count) {
      os << StringPrintf("#%02zd ", i);
    }
    os << text << std::endl;
  }
}

#elif defined(__APPLE__)

void DumpNativeStack(std::ostream& os ATTRIBUTE_UNUSED,
                     pid_t tid ATTRIBUTE_UNUSED,
                     BacktraceMap* existing_map ATTRIBUTE_UNUSED,
                     const char* prefix ATTRIBUTE_UNUSED,
                     ArtMethod* current_method ATTRIBUTE_UNUSED,
                     void* ucontext_ptr ATTRIBUTE_UNUSED,
                     bool skip_frames ATTRIBUTE_UNUSED) {
}

void DumpKernelStack(std::ostream& os ATTRIBUTE_UNUSED,
                     pid_t tid ATTRIBUTE_UNUSED,
                     const char* prefix ATTRIBUTE_UNUSED,
                     bool include_count ATTRIBUTE_UNUSED) {
}

#else
#error "Unsupported architecture for native stack dumps."
#endif

}  // namespace art
