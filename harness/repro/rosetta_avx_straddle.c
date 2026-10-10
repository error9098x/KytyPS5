// Reproduces a Rosetta 2 abort that affects any x86-64 emulator process that tracks CPU writes
// with page protection (as KytyPS5 does for GPU coherence).
//
//   clang -arch x86_64 -mavx2 -O1 rosetta_avx_straddle.c -o rosetta_avx_straddle
//   arch -x86_64 ./rosetta_avx_straddle 0   # SSE 16-byte store across two protected pages: ok
//   arch -x86_64 ./rosetta_avx_straddle 1   # AVX 32-byte store across two protected pages: ABORT
//   arch -x86_64 ./rosetta_avx_straddle 2   # AVX 32-byte store aligned to a page: ok
//
// Mode 1 prints "rosetta error: unexpectedly need to EmulateForward on a synchronous exception".
// Rosetta executes a 256-bit store as two 128-bit halves. When the first half completes and the
// second half then faults on the next (protected) page, Rosetta cannot recover precise x86 state
// and kills the process before the SIGSEGV/SIGBUS handler runs. The handler below only
// unprotects the faulting page, exactly like a page-granular write tracker.
#include <immintrin.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int faults;

static void handler(int sig, siginfo_t* si, void* ctx) {
	(void)sig;
	(void)ctx;
	faults++;
	uintptr_t page = (uintptr_t)si->si_addr & ~(uintptr_t)4095;
	mprotect((void*)page, 4096, PROT_READ | PROT_WRITE);
}

int main(int argc, char** argv) {
	int   mode = argc > 1 ? atoi(argv[1]) : 1;
	char* base = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, 0);
	sigaction(SIGBUS, &sa, 0);

	mprotect(base, 16384, PROT_READ);
	volatile char* p = base;

	if (mode == 0) {
		_mm_storeu_si128((__m128i*)(p + 4088), _mm_set1_epi32(1));
	} else if (mode == 1) {
		_mm256_storeu_si256((__m256i*)(p + 4080), _mm256_set1_epi32(1));
	} else {
		_mm256_storeu_si256((__m256i*)(p + 4096), _mm256_set1_epi32(1));
	}
	printf("mode %d ok, faults handled: %d\n", mode, faults);
	return 0;
}
