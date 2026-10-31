#include <stdio.h>

#include "scraper.h"

int main(int argc, char** argv) {
	
	printf("arg count: %d\n", argc);
	if (argc > 1)
		printf("%s\n", argv[1]);

	Scrape(argv[1]);

	return 0;
}