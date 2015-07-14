CXX=g++
CXX_OPTS= -Wall -g -O2 -DDEBUG1
CXXFLAGS=$(CXX_OPTS)

CC=cc
CFLAGS=
CC_OPTS=-lstdc++ -lm -lbcm2835
LDFLAGS=$(CC_OPTS)
#LDFLAGS=-lpthread -pthread -lstdc++ -lsupc++ 
#LD_OPTS=-lpthread -lrt -lstdc++

INSTALL=install

OBJ_AVRSPI=avrspi.o avrlog.o routines.o spidev.o avrconfig.o mpu.o flightlog.o
OBJ_AVRSPI_CMD=avrspi_cmd.o routines.o

%.o: %.c                                                                         
	$(CXX) -c $(CXXFLAGS) $(CXX_OPTS) $< -o $@ 

all: _websockify avrspi avrspi_cmd

#modules:
#	$(MAKE) -C spi-bcm2708-dma/

_websockify:
	$(MAKE) -C websockify/

avrspi: $(OBJ_AVRSPI)
	$(CC) $(CFLAGS) $(OBJ_AVRSPI) -o avrspi $(LDFLAGS) $(CC_OPTS) 

avrspi_cmd: $(OBJ_AVRSPI_CMD)
	$(CC) $(CFLAGS) $(OBJ_AVRSPI_CMD) -o avrspi_cmd $(LDFLAGS) $(CC_OPTS) 

install:
	$(INSTALL) -m 0755 -d $(DESTDIR)/etc/init.d
	$(INSTALL) -m 0755 -d $(DESTDIR)/etc/avrminicopter
	$(INSTALL) -m 0755 -d $(DESTDIR)/usr/local/bin
	$(INSTALL) -m 777 utils/flog.config $(DESTDIR)/etc/avrminicopter/
	$(INSTALL) -m 777 utils/rpicopter.config $(DESTDIR)/etc/avrminicopter/
	$(INSTALL) -m 755 utils/S91avrspi $(DESTDIR)/etc/init.d/
	$(INSTALL) -m 755 avrspi $(DESTDIR)/usr/local/bin/
	$(INSTALL) -m 755 avrspi_cmd $(DESTDIR)/usr/local/bin/
	$(INSTALL) -m 755 websockify/websockify $(DESTDIR)/usr/local/bin/

clean:
	cd websockify && $(MAKE) clean
#	cd spi-bcm2708-dma && $(MAKE) clean
	rm -rf avrspi_cmd
	rm -rf avrspi 
	rm -rf *.o *~ *.mod
