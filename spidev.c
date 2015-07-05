/*
 * SPI testing utility (using spidev driver)
 *
 * Copyright (c) 2007  MontaVista Software, Inc.
 * Copyright (c) 2007  Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * Cross-compile with cross-gcc -I/path/to/cross-kernel/include
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <fcntl.h>
//#include <sys/ioctl.h>
#include <linux/types.h>
//#include <linux/spi/spidev.h>
#include <bcm2835.h>
#include "spidev.h"
#include "crc8.h"
#include "routines.h"
#include <string.h>

static bool t_start = false;
static uint8_t buf[4];
static int p = 0;

static int ret;

struct avr_msg spi_buf[SPI_BUF_SIZE]; //reader buf
int spi_buf_c = 0;

int spi_crc_err = 0;

int _spi_addByte(uint8_t b) {

    buf[p++] = b;

    if (p==4) {
        int16_t v = 0;
        v = buf[2] << 8 | buf [1]; 
        //uint8_t c = CRC8(buf,3);
        if (CRC8(buf,3) == buf[3]) {
	    if (spi_buf_c == SPI_BUF_SIZE) {
		printf("SPI input buffer full. Resetting.");
		spi_buf_c = 0;
	    }
	    spi_buf[spi_buf_c].t = buf[0];
	    spi_buf[spi_buf_c].v = v;
	    spi_buf_c++;
	    p = 0;
	    if (verbose>1) printf("Received from AVR msg %i %i\n",buf[0],v);
        } else {
		spi_crc_err++;
		if (verbose) printf("Received CRC failed msg %i %i %i %i\n",buf[0],buf[1],buf[2],buf[3]);
		buf[0]=buf[1];
		buf[1]=buf[2];
		buf[2]=buf[3];
		p--;
	}
	return 1;
    }
    return 0;
}

int spi_writeBytes(uint8_t *data, unsigned int len) {
/*
    uint8_t v[16];

    data[len] = CRC8((unsigned char*)(data),len);
    len++;

    bcm2835_spi_transfernb((char*)data,(char*)v,len);

    for (unsigned int i=0;i<len;i++) {
	if (!t_start && v[i]!=0) t_start=true; //looking for the first non-zero byte which indicates start of a packet
	
	if (t_start) //we have found begining of a transfer
		if (_spi_addByte(v[i])) t_start = false;
	//we have consumed the packet hence we will be waiting for a new start byte 
    }
*/

    uint8_t v;
    
    data[len] = CRC8((unsigned char*)(data),len);
    len++;

    for (unsigned int i=0;i<len;i++) {
	v = bcm2835_spi_transfer(data[i]);

	if (!t_start && v!=0) t_start=true; //looking for the first non-zero byte which indicates start of a packet
	
	if (t_start) //we have found begining of a transfer
		if (_spi_addByte(v)) t_start = false;
	//we have consumed the packet hence we will be waiting for a new start byte 
	bcm2835_delay(1);
    }
    return ret;
}

void spi_sendMsg(struct avr_msg *m) {
    spi_sendIntPacket(m->t,m->v);
    bcm2835_delay(2); 
}

void spi_sendIntPacket_delay(uint8_t t, int16_t v) {
	spi_sendIntPacket(t,v);
	bcm2835_delay(5); //should be bigger than the AVR controller loop 
}

void spi_sendIntPacket(uint8_t t, int16_t v) {
    static unsigned char b[4];
    int16_t *vptr = &v;
    b[0] = t;
    memcpy(b+1,vptr,2);

    if (verbose>1) printf("Sending to AVR msg %u %i\n",t,v);
    spi_writeBytes(b,3);
}

void spi_reset() {
	t_start = false;
	p = 0;
	spi_buf_c = 0;
}

int spi_close() {
    bcm2835_spi_end();
    return 0;
}

int spi_init() {
    bcm2835_spi_begin();
    bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);      // The default
    bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);                   // The default
    //bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_65536); // The default
    bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_256); // The default
    bcm2835_spi_chipSelect(BCM2835_SPI_CS0);                      // The default
    bcm2835_spi_setChipSelectPolarity(BCM2835_SPI_CS0, LOW);      // the default

    return 0;
}

