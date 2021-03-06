//
// Copyright (c) 2008 Advanced Micro Devices, Inc. All rights reserved.
//

#if !defined(_WIN32) && !defined(__CYGWIN__)

#include "os/os.hpp"
#include "thread/thread.hpp"
#include "utils/util.hpp"

#include <iostream>
#include <stdarg.h>

#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>

#include <sys/prctl.h>

#include <link.h>
#include <time.h>
#include <elf.h>
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif  // DT_GNU_HASH

#include <atomic>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>  // for strncmp
#include <cstdlib>
#include <cstdio>  // for tempnam
#include <limits.h>
#include <memory>
#include <algorithm>


namespace amd {

static struct sigaction oldSigAction;

static bool callOldSignalHandler(int sig, siginfo_t* info, void* ptr) {
  if (oldSigAction.sa_handler == SIG_DFL) {
    // no signal handler was previously installed.
    return false;
  } else if (oldSigAction.sa_handler != SIG_IGN) {
    if ((oldSigAction.sa_flags & SA_NODEFER) == 0) {
      sigaddset(&oldSigAction.sa_mask, sig);
    }

    void (*handler)(int) = oldSigAction.sa_handler;
    if (oldSigAction.sa_flags & SA_RESETHAND) {
      oldSigAction.sa_handler = SIG_DFL;
    }

    sigset_t savedSigSet;
    pthread_sigmask(SIG_SETMASK, &oldSigAction.sa_mask, &savedSigSet);

    if (oldSigAction.sa_flags & SA_SIGINFO) {
      oldSigAction.sa_sigaction(sig, info, ptr);
    } else {
      handler(sig);
    }

    pthread_sigmask(SIG_SETMASK, &savedSigSet, NULL);
  }

  return true;
}

static void divisionErrorHandler(int sig, siginfo_t* info, void* ptr) {
  assert(info != NULL && ptr != NULL && "just checking");
  ucontext_t* uc = (ucontext_t*)ptr;
  address insn;

#if defined(ATI_ARCH_X86)
  insn = (address)uc->uc_mcontext.gregs[LP64_SWITCH(REG_EIP, REG_RIP)];
#else
  assert(!"Unimplemented");
#endif

  if (Thread::current()->isWorkerThread()) {
    if (Os::skipIDIV(insn)) {
#if defined(ATI_ARCH_X86)
      uc->uc_mcontext.gregs[LP64_SWITCH(REG_EIP, REG_RIP)] = (greg_t)insn;
#else
      assert(!"Unimplemented");
#endif
      return;
    }
  }

  // Call the chained signal handler
  if (callOldSignalHandler(sig, info, ptr)) {
    return;
  }


  std::cerr << "Unhandled signal in divisionErrorHandler()" << std::endl;
  ::abort();
}

typedef int (*pthread_setaffinity_fn)(pthread_t, size_t, const cpu_set_t*);
static pthread_setaffinity_fn pthread_setaffinity_fptr;

static void init() __attribute__((constructor(101)));
static void init() { Os::init(); }

bool Os::installSigfpeHandler() {
  // Install a SIGFPE signal handler @todo: Chain the handlers
  struct sigaction sa;
  sigfillset(&sa.sa_mask);
  sa.sa_handler = SIG_DFL;
  sa.sa_sigaction = divisionErrorHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;

  if (sigaction(SIGFPE, &sa, &oldSigAction) != 0) {
    return false;
  }
  return true;
}

void Os::uninstallSigfpeHandler() {}

bool Os::init() {
  static bool initialized_ = false;

  // We could use pthread_once here:
  if (initialized_) {
    return true;
  }
  initialized_ = true;

  pageSize_ = (size_t)::sysconf(_SC_PAGESIZE);
  processorCount_ = ::sysconf(_SC_NPROCESSORS_CONF);

  pthread_setaffinity_fptr = (pthread_setaffinity_fn)dlsym(RTLD_NEXT, "pthread_setaffinity_np");

  return Thread::init();
}

static void __exit() __attribute__((destructor(101)));
static void __exit() { Os::tearDown(); }

void Os::tearDown() { Thread::tearDown(); }

bool Os::iterateSymbols(void* handle, Os::SymbolCallback callback, void* data) {
  const char magic[] = "__OpenCL_";
  const size_t len = sizeof(magic) - 1;

  struct link_map* link_map = NULL;
  if (::dlinfo(handle, RTLD_DI_LINKMAP, &link_map) != 0) {
    return false;
  }

  assert(link_map != NULL && "just checking");
  const ElfW(Dyn)* dyn = (ElfW(Dyn)*)(link_map->l_ld);

  const Elf32_Word* gnuhash = NULL;
  const Elf_Symndx* hash = NULL;
  const ElfW(Sym)* symbols = NULL;
  const char* stringTable = NULL;
  size_t tableSize = 0;

  // Search for the string table address and size.
  while (dyn->d_tag != DT_NULL) {
    switch (dyn->d_tag) {
      case DT_HASH:
        hash = (Elf_Symndx*)dyn->d_un.d_ptr;
        break;
      case DT_GNU_HASH:
        gnuhash = (Elf32_Word*)dyn->d_un.d_ptr;
        break;
      case DT_SYMTAB:
        symbols = (ElfW(Sym)*)dyn->d_un.d_ptr;
        break;
      case DT_STRTAB:
        stringTable = (const char*)dyn->d_un.d_ptr;
        break;
      case DT_STRSZ:
        tableSize = dyn->d_un.d_val;
        break;
      default:
        break;
    }
    ++dyn;
  }
  if (stringTable == NULL || tableSize == 0 || symbols == NULL ||
      (hash == NULL && gnuhash == NULL)) {
    // Could not find the string table
    return false;
  }

  if (gnuhash == NULL) {
    // Read the defined symbols out of the classic SYSV hashtable.

    Elf_Symndx nbuckets = hash[1];
    for (Elf_Symndx i = 0; i < nbuckets; ++i) {
      if (symbols[i].st_shndx == SHN_UNDEF && symbols[i].st_value == 0) {
        continue;
      }

      const char* name = &stringTable[symbols[i].st_name];
      if (::strncmp(name, magic, len) == 0) {
        callback(name, (const void*)(link_map->l_addr + symbols[i].st_value), data);
      }
    }
    return true;
  }

  // Read the defined symbols out of the GNU hashtable.

  Elf_Symndx nbuckets = gnuhash[0];
  Elf32_Word bias = gnuhash[1];
  Elf32_Word nwords = gnuhash[2];
  const Elf32_Word* buckets = &gnuhash[4 + __ELF_NATIVE_CLASS / 32 * nwords];
  const Elf32_Word* chain0 = &buckets[nbuckets] - bias;

  for (Elf_Symndx i = 0; i < nbuckets; ++i) {
    size_t index = buckets[i];
    const Elf32_Word* hasharr = &chain0[index];
    do {
      if (symbols[index].st_shndx != SHN_UNDEF || symbols[index].st_value != 0) {
        const char* name = &stringTable[symbols[index].st_name];
        if (::strncmp(name, magic, len) == 0) {
          callback(name, (const void*)(link_map->l_addr + symbols[index].st_value), data);
        }
      }
      ++index;
    } while ((*hasharr++ & 1) == 0);
  }

  return true;
}

void* Os::loadLibrary_(const char* filename) {
  return (*filename == '\0') ? NULL : ::dlopen(filename, RTLD_LAZY);
}

void Os::unloadLibrary(void* handle) { ::dlclose(handle); }

void* Os::getSymbol(void* handle, const char* name) { return ::dlsym(handle, name); }

static inline int memProtToOsProt(Os::MemProt prot) {
  switch (prot) {
    case Os::MEM_PROT_NONE:
      return PROT_NONE;
    case Os::MEM_PROT_READ:
      return PROT_READ;
    case Os::MEM_PROT_RW:
      return PROT_READ | PROT_WRITE;
    case Os::MEM_PROT_RWX:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      break;
  }
  ShouldNotReachHere();
  return -1;
}

address Os::reserveMemory(address start, size_t size, size_t alignment, MemProt prot) {
  size = alignUp(size, pageSize());
  alignment = std::max(pageSize(), alignUp(alignment, pageSize()));
  assert(isPowerOfTwo(alignment) && "not a power of 2");

  size_t requested = size + alignment - pageSize();
  address mem = (address)::mmap(start, requested, memProtToOsProt(prot),
                                MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS, 0, 0);

  // check for out of memory
  if (mem == NULL) return NULL;

  address aligned = alignUp(mem, alignment);

  // return the unused leading pages to the free state
  if (&aligned[0] != &mem[0]) {
    assert(&aligned[0] > &mem[0] && "check this code");
    if (::munmap(&mem[0], &aligned[0] - &mem[0]) != 0) {
      assert(!"::munmap failed");
    }
  }
  // return the unused trailing pages to the free state
  if (&aligned[size] != &mem[requested]) {
    assert(&aligned[size] < &mem[requested] && "check this code");
    if (::munmap(&aligned[size], &mem[requested] - &aligned[size]) != 0) {
      assert(!"::munmap failed");
    }
  }

  return aligned;
}

bool Os::releaseMemory(void* addr, size_t size) {
  assert(isMultipleOf(addr, pageSize()) && "not page aligned!");
  size = alignUp(size, pageSize());

  return 0 == ::munmap(addr, size);
}

bool Os::commitMemory(void* addr, size_t size, MemProt prot) {
  assert(isMultipleOf(addr, pageSize()) && "not page aligned!");
  size = alignUp(size, pageSize());

  return ::mmap(addr, size, memProtToOsProt(prot), MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1,
                0) != MAP_FAILED;
}

bool Os::uncommitMemory(void* addr, size_t size) {
  assert(isMultipleOf(addr, pageSize()) && "not page aligned!");
  size = alignUp(size, pageSize());

  return ::mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE | MAP_ANONYMOUS, -1,
                0) != MAP_FAILED;
}

bool Os::protectMemory(void* addr, size_t size, MemProt prot) {
  assert(isMultipleOf(addr, pageSize()) && "not page aligned!");
  size = alignUp(size, pageSize());

  return 0 == ::mprotect(addr, size, memProtToOsProt(prot));
}

uint64_t Os::hostTotalPhysicalMemory() {
  static uint64_t totalPhys = 0;

  if (totalPhys != 0) {
    return totalPhys;
  }

  totalPhys = sysconf(_SC_PAGESIZE) * sysconf(_SC_PHYS_PAGES);
  return totalPhys;
}

void* Os::alignedMalloc(size_t size, size_t alignment) {
  void* ptr = NULL;
  if (0 == ::posix_memalign(&ptr, alignment, size)) {
    return ptr;
  }
  return NULL;
}

void Os::alignedFree(void* mem) { ::free(mem); }

void Os::currentStackInfo(address* base, size_t* size) {
  // There could be some issue trying to get the pthread_attr of
  // the primordial thread if the pthread library is not present
  // at load time (a binary loads the OpenCL app/runtime dynamically.
  // We should look into this... -laurent

  pthread_t self = ::pthread_self();

  pthread_attr_t threadAttr;
  if (0 != ::pthread_getattr_np(self, &threadAttr)) {
    fatal("pthread_getattr_np() failed");
  }

  if (0 != ::pthread_attr_getstack(&threadAttr, (void**)base, size)) {
    fatal("pthread_attr_getstack() failed");
  }
  *base += *size;

  ::pthread_attr_destroy(&threadAttr);

  assert(Os::currentStackPtr() >= *base - *size && Os::currentStackPtr() < *base &&
         "just checking");
}

void Os::setCurrentThreadName(const char* name) { ::prctl(PR_SET_NAME, name); }


void* Thread::entry(Thread* thread) {
  sigset_t set;

  sigfillset(&set);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  sigemptyset(&set);
  sigaddset(&set, SIGFPE);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);

  return thread->main();
}

bool Os::isThreadAlive(const Thread& thread) {
  return ::pthread_kill((pthread_t)thread.handle(), 0) == 0;
}

const void* Os::createOsThread(amd::Thread* thread) {
  pthread_attr_t threadAttr;
  ::pthread_attr_init(&threadAttr);

  if (thread->stackSize_ != 0) {
    size_t guardsize = 0;
    if (0 != ::pthread_attr_getguardsize(&threadAttr, &guardsize)) {
      fatal("pthread_attr_getguardsize() failed");
    }
    ::pthread_attr_setstacksize(&threadAttr, thread->stackSize_ + guardsize);
  }

  // We never plan the use join, so free the resources now.
  ::pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);

  pthread_t handle = 0;
  if (0 != ::pthread_create(&handle, &threadAttr, (void* (*)(void*)) & Thread::entry, thread)) {
    thread->setState(Thread::FAILED);
  }

  ::pthread_attr_destroy(&threadAttr);
  return reinterpret_cast<const void*>(handle);
}


void Os::setThreadAffinity(const void* handle, const Os::ThreadAffinityMask& mask) {
  if (pthread_setaffinity_fptr != NULL) {
    pthread_setaffinity_fptr((pthread_t)handle, sizeof(cpu_set_t), &mask.mask_);
  }
}

void Os::yield() { ::sched_yield(); }

uint64_t Os::timeNanos() {
  struct timespec tp;
  ::clock_gettime(CLOCK_MONOTONIC, &tp);
  return (uint64_t)tp.tv_sec * (1000ULL * 1000ULL * 1000ULL) + (uint64_t)tp.tv_nsec;
}

uint64_t Os::timerResolutionNanos() {
  static uint64_t resolution = 0;
  if (resolution == 0) {
    struct timespec tp;
    ::clock_getres(CLOCK_MONOTONIC, &tp);
    resolution = (uint64_t)tp.tv_sec * (1000ULL * 1000ULL * 1000ULL) + (uint64_t)tp.tv_nsec;
  }
  return resolution;
}


const char* Os::libraryExtension() { return MACOS_SWITCH(".dylib", ".so"); }

const char* Os::libraryPrefix() { return "lib"; }

const char* Os::objectExtension() { return ".o"; }

char Os::fileSeparator() { return '/'; }

char Os::pathSeparator() { return ':'; }

bool Os::pathExists(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

bool Os::createPath(const std::string& path) {
  mode_t mode = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;
  size_t pos = 0;
  while (true) {
    pos = path.find(fileSeparator(), pos);
    const std::string currPath = path.substr(0, pos);
    if (!currPath.empty() && !pathExists(currPath)) {
      int ret = mkdir(currPath.c_str(), mode);
      if (ret == -1) return false;
    }
    if (pos == std::string::npos) break;
    ++pos;
  }
  return true;
}

bool Os::removePath(const std::string& path) {
  size_t pos = std::string::npos;
  bool removed = false;
  while (true) {
    const std::string currPath = path.substr(0, pos);
    if (!currPath.empty()) {
      int ret = rmdir(currPath.c_str());
      if (ret == -1) return removed;
      removed = true;
    }
    if (pos == 0) break;
    pos = path.rfind(fileSeparator(), pos == std::string::npos ? pos : pos - 1);
    if (pos == std::string::npos) break;
  }
  return true;
}

int Os::printf(const char* fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  int len = ::vprintf(fmt, ap);
  va_end(ap);

  return len;
}

// Os::systemCall()
// ================
// Execute a program and return the program exitcode or -1 if there were problems.
// The input argument 'command' is expected to be a space separated string of
// command-line arguments with arguments containing spaces between double-quotes.
//
// In order to avoid duplication of memory, we use vfork()+exec(). vfork() has
// potiential security risks; read the following for details:
//
//     https://www.securecoding.cert.org/confluence/display/seccode/POS33-C.+Do+not+use+vfork()
//
// In spite of these risks, the alternatives (system() or fork()) create resource
// issues when running conformance test_allocation which stretches the system
// memory to its limits. Thus we will accept this compromise under the condition
// that the runtime will soon remove any need to call out to external commands.
//
// Note that stdin/stdout/stderr of the command are sent to /dev/null.
//
int Os::systemCall(const std::string& command) {
#if 1
  size_t len = command.size();
  char* cmd = new char[len + 1];
  fastMemcpy(cmd, command.c_str(), len);
  cmd[len] = 0;

  // Split the command into arguments. This is a very
  // simple parser that only takes care of quotes and
  // doesn't support escaping with back-slash. In
  // the future, Os::systemCall() will either
  // disappear or it will be replaced with an
  // argc/argv interface. This parser also assumes
  // that if an argument is quoted, the whole
  // argument starts and ends with a double-quote.
  bool inQuote = false;
  int argLength = 0;
  int n = 0;
  char* cp = cmd;
  while (*cp) {
    switch (static_cast<int>(*cp)) {
      case ' ':
        if (inQuote) {
          ++argLength;
        } else {
          *cp = '\0';
          argLength = 0;
        }
        break;
      case '"':
        if (inQuote) {
          inQuote = false;
          *cp = '\0';
        } else {
          inQuote = true;
          *cp = '\0';
          argLength = 1;
          ++n;
        }
        break;
      default:
        if (++argLength == 1) {
          ++n;
        }
        break;
    }
    ++cp;
  }

  char** argv = new char*[n + 1];
  int argc = 0;
  cp = cmd;
  do {
    while ('\0' == *cp) {
      ++cp;
    }
    argv[argc++] = cp;
    while ('\0' != *cp) {
      ++cp;
    }
  } while (argc < n);
  argv[argc] = NULL;

  int ret = -1;
  pid_t pid = vfork();
  if (0 == pid) {
    // Child. Redirect stdin/stdout/stderr to /dev/null
    int fdIn = open("/dev/null", O_RDONLY);
    int fdOut = open("/dev/null", O_WRONLY);
    if (0 <= fdIn || 0 <= fdOut) {
      dup2(fdIn, 0);
      dup2(fdOut, 1);
      dup2(fdOut, 2);

      // Execute the program
      execvp(argv[0], argv);
    }
    _exit(-1);
  } else if (0 > pid) {
    // Can't vfork
  } else {
    // Parent - wait for program to complete and get exit code.
    int exitCode;
    if (0 <= waitpid(pid, &exitCode, 0)) {
      ret = exitCode;
    }
  }
  delete[] argv;
  delete[] cmd;

  return ret;
#else
  return ::system(command.c_str());
#endif
}

std::string Os::getEnvironment(const std::string& name) {
  char* dstBuf;

  dstBuf = ::getenv(name.c_str());
  if (dstBuf == NULL) {
    return std::string("");
  }
  return std::string(dstBuf);
}

std::string Os::getTempPath() {
  std::string tempFolder = amd::Os::getEnvironment("TEMP");
  if (tempFolder.empty()) {
    tempFolder = amd::Os::getEnvironment("TMP");
  }

  if (tempFolder.empty()) {
    tempFolder = "/tmp";
    ;
  }
  return tempFolder;
}

std::string Os::getTempFileName() {
  static std::atomic_size_t counter(0);

  std::string tempPath = getTempPath();
  std::stringstream tempFileName;

  tempFileName << tempPath << "/OCL" << ::getpid() << 'T' << counter++;
  return tempFileName.str();
}

int Os::unlink(const std::string& path) { return ::unlink(path.c_str()); }

#if defined(ATI_ARCH_X86)
void Os::cpuid(int regs[4], int info) {
#ifdef _LP64
  __asm__ __volatile__(
      "movq %%rbx, %%rsi;"
      "cpuid;"
      "xchgq %%rbx, %%rsi;"
      : "=a"(regs[0]), "=S"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
      : "a"(info));
#else
  __asm__ __volatile__(
      "movl %%ebx, %%esi;"
      "cpuid;"
      "xchgl %%ebx, %%esi;"
      : "=a"(regs[0]), "=S"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
      : "a"(info));
#endif
}

uint64_t Os::xgetbv(uint32_t ecx) {
  uint32_t eax, edx;

  __asm__ __volatile__(".byte 0x0f,0x01,0xd0"  // in case assembler doesn't recognize xgetbv
                       : "=a"(eax), "=d"(edx)
                       : "c"(ecx));

  return ((uint64_t)edx << 32) | (uint64_t)eax;
}
#endif  // ATI_ARCH_X86

void* Os::fastMemcpy(void* dest, const void* src, size_t n) { return memcpy(dest, src, n); }

uint64_t Os::offsetToEpochNanos() {
  static uint64_t offset = 0;

  if (offset != 0) {
    return offset;
  }

  struct timeval now;
  if (::gettimeofday(&now, NULL) != 0) {
    return 0;
  }

  offset = (now.tv_sec * UINT64_C(1000000) + now.tv_usec) * UINT64_C(1000) - timeNanos();

  return offset;
}

void Os::setCurrentStackPtr(address sp) {
  sp -= sizeof(void*);
  *(void**)sp = __builtin_return_address(0);

#if defined(ATI_ARCH_ARM)
  assert(!"Unimplemented");
#else
  __asm__ __volatile__(
#if !defined(OMIT_FRAME_POINTER)
      LP64_SWITCH("movl (%%ebp),%%ebp;", "movq (%%rbp),%%rbp;")
#endif  // !OMIT_FRAME_POINTER
          LP64_SWITCH("movl %0,%%esp; ret;", "movq %0,%%rsp; ret;")::"r"(sp));
#endif
}

size_t Os::getPhysicalMemSize() {
  struct ::sysinfo si;

  if (::sysinfo(&si) != 0) {
    return 0;
  }

  if (si.mem_unit == 0) {
    // Linux kernels prior to 2.3.23 return sizes in bytes.
    si.mem_unit = 1;
  }

  return (size_t)si.totalram * si.mem_unit;
}

void Os::getAppPathAndFileName(std::string& appName, std::string& appPathAndName) {
  std::unique_ptr<char[]> buff(new char[FILE_PATH_MAX_LENGTH]());

  if (readlink("/proc/self/exe", buff.get(), FILE_PATH_MAX_LENGTH) > 0) {
    // Get filename without path and extension.
    appName = std::string(basename(buff.get()));
    appPathAndName = std::string(buff.get());
  }
  else {
    appName = "";
    appPathAndName = "";
  }
  return;
}

}  // namespace amd

#endif  // !defined(_WIN32) && !defined(__CYGWIN__)
