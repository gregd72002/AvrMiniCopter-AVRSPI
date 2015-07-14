#ifndef _AVRLOG_H
#define _AVRLOG_H

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>

#define AVRLOGBUF_SIZE 128
extern char avrlogbuf[AVRLOGBUF_SIZE];
extern uint8_t avrlogbuflen;

int avrlog_init();
void avrlog_write(const char *s, int len);
void avrlog_close();

#endif
