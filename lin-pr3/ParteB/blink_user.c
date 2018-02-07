#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>


#define SIMON_INITIAL_SIZE 8
#define SIMON_INITIAL_DIFFICULTY 3

#define BLINKSTICK_DEV_PATH "/dev/usb/blinkstick0"
#define BLINKSTICK_CMD_CH_LENGTH 11 /* 1 digit index + 1 char colon + 8 digit color + \0 */
#define BLINKSTICK_NR_LEDS 8
#define LED_AWAIT_TIME_SECS 1

typedef struct {
	char* leds;
	size_t size;
	unsigned int difficulty;
} simon_t;

static void simon_init(simon_t* simon) {
	simon->leds = malloc(SIMON_INITIAL_SIZE * sizeof(char));
	simon->size = SIMON_INITIAL_SIZE;
	simon->difficulty = SIMON_INITIAL_DIFFICULTY;
}

static void simon_resize(simon_t* simon) {
	free(simon->leds);
	simon->size *= 2;
	simon->leds = malloc(simon->size * sizeof(char));
}

static void simon_destroy(simon_t* simon) {
	free(simon->leds);
}

/* Decide which leds will turn on */
static void simon_generate(simon_t* simon) {
	int i;
	
	if(simon->difficulty > simon->size)
		simon_resize(simon);

	srand(time(NULL));
	for(i = 0; i < simon->difficulty; i++) {
		simon->leds[i] = rand() % BLINKSTICK_NR_LEDS;
	}
}

static const unsigned int DEF_COLORS[BLINKSTICK_NR_LEDS] = {
	0x330000,	/* Red     */
	0x331A00,	/* Orange  */
	0x333300,	/* Yellow  */
	0x003300,	/* Green   */
	0x003333,	/* Indigo  */
	0x000033,	/* Blue    */
	0x1A0033,	/* Violet  */
	0x33001A	/* Magenta */
};

/* Write control commands to the blinkstick device */
static int write_to_blinkstick(char* str, size_t len) {
	int devfd;
	if((devfd = open(BLINKSTICK_DEV_PATH, O_WRONLY | O_APPEND)) < 0) {
		errno = ENODEV;
		perror("Blinkstick");
		return -1;
	}
	write(devfd, str, len);
	close(devfd);
	return 0;
}

/* Set a color value for a single led */
static int set_single_led(char led, unsigned int color) {
	char cmd[BLINKSTICK_CMD_CH_LENGTH];
	sprintf(cmd, "%u:0x%06X", led, color);

	return write_to_blinkstick(cmd, sizeof(cmd));
}

/* Set a series of color values for all the leds */
static int set_all_leds(const unsigned int* colors) {
	char cmd[BLINKSTICK_CMD_CH_LENGTH * BLINKSTICK_NR_LEDS];
	char* it = cmd;
	int i;
	for(i = 0; i < BLINKSTICK_NR_LEDS - 1; i++) {
		it += sprintf(it, "%u:0x%06X,", i, colors[i]);
	}
	sprintf(it, "%u:0x%06X", i, colors[i]);

	return write_to_blinkstick(cmd, sizeof(cmd));
}

/* Indicate the next level starts to the user */
static int simon_next_level(simon_t* simon) {
	unsigned int loading[BLINKSTICK_NR_LEDS] = {0};
	static const unsigned int color = 0x003300;
	int i;

	simon->difficulty++;
	for(i = 0; i < BLINKSTICK_NR_LEDS; i++) {
		loading[i] = color;
		if(set_all_leds(loading) != 0)
			return 1;
		usleep(1000000/BLINKSTICK_NR_LEDS);
	}
	return set_single_led(0,0);
}

/* Show the sequence to the user */
static int simon_show(const simon_t* simon) {
	char led;
	int i;

	for(i = 0; i < simon->difficulty; i++) {
		led = simon->leds[i];
		if(set_single_led(led, DEF_COLORS[led]))
			return -1;
		sleep(LED_AWAIT_TIME_SECS);
	}
	/* Turn off all leds */
	set_single_led(0,0);
	return 0;
}

/* Run simon says game */
static int simon_run(simon_t* simon) {
	unsigned int guessed; // Count of guessed lights
	unsigned int input;   // User input
	unsigned int level = 1;
	
	do {
		simon_generate(simon);
		printf("[ Level %u ]\n", level);
		if(simon_show(simon) != 0)
			return 1;

		printf("What did Simon say? (0 to exit)\n");
		
		guessed = 0;
		while((guessed < simon->difficulty) && scanf("%u",&input) && (input-1 == simon->leds[guessed])) {
			printf("OK!\n");
			guessed++;
		}
		if(guessed == simon->difficulty) {
			if(simon_next_level(simon) != 0) return 1;
			level++;
		} else {
			break;
		}
	} while(1);
	
	if(input == 0)
		printf("You disappoint simon :(\n");
	else
		printf("Wrong, you die hard!\n");

	return 0;
}


int main(void) {
	int ret = 0;
	simon_t simon;
	simon_init(&simon);
	ret = simon_run(&simon);
	simon_destroy(&simon);
	return ret;
}
