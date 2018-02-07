#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define NR_LEDS 8

/* FORMAT: <numled-a>:<color>,<numled-b>:<color>,<numled-c>:<color>,... */

int main(int argc, char** argv) {
	char *input;
	int len;
	
	char *it, *token;
	
	unsigned int colors[NR_LEDS] = {};
	unsigned int idx, color;
	int chparsed;
	
	if(argc != 2)
		return 1;
	
	/* 
	if(input[len-1] != '\0' && input[len-1] != '\n')
		len++
	// Put \0 at the end
	*/
	input = strdup(argv[1]); // copy_from_user
	len = strlen(input);
	
	it = input;
	while((token = strsep(&it, ",")) != NULL) {
		if(sscanf(token, "%u:0x%6x%n", &idx, &color, &chparsed) != 2 || token[chparsed] != '\0') {
			// error - invalid argument format
			break;
		}
		
		if(idx > NR_LEDS) {
			// error - invalid index
			break;
		}
		
		if(colors[idx] != 0) {
			// error - invalid argument: repeated index
			break;
		}
		
		colors[idx] = color;
		printf("%s => %u - %X\n", token, idx, color);
	}
	
	printf("==================\n|     RESULT     |\n==================\n");
	for(int i = 0; i < NR_LEDS; i++) {
		printf("color[%u] = %X\n", i, colors[i]);
	}

	free(input);
	return 0;
}




