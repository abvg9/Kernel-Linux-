#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#ifdef __i386__
#define __NR_LEDCTL 353
#else
#define __NR_LEDCTL 316
#endif

#define ALL_LEDS_ON 0x7

long ledctl(unsigned int mask) {
	return (long) syscall(__NR_LEDCTL, mask);
}


int main(int argc, char *argv[]) {
	unsigned int leds;
	
	if(argc != 2) {
		errno=E2BIG;
		perror("");
		return 4;
	}

	if(1 != sscanf(argv[1], "0x%X", &leds) || leds > ALL_LEDS_ON) {
		errno=EINVAL;
		perror("");
		return 2;
	}

	if(ledctl(leds) != 0) {
		perror("");	
		return 1;
	}
	
	return 0;
}
