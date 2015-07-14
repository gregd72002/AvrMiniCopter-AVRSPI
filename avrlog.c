#include "avrlog.h"

char avrlogbuf[AVRLOGBUF_SIZE];
uint8_t avrlogbuflen;


static int f = 0;

int avrlog_init() {
	f = open("/rpicopter/avr.log",O_CREAT | O_WRONLY | O_APPEND | O_DSYNC | O_SYNC);
	if (f == -1) return -1;
	avrlog_write("Initializing...",15);
	return 0;
}

void avrlog_write(const char *s, int len) {
	if (f == -1) return;
	write(f,s,len);
	write(f,"\n",1);
	fsync(f);
}

void avrlog_close() {
	if (f == -1) return;
	avrlog_write("Closing\n\n",9);
	close(f);
}


