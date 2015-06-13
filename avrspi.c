#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "routines.h"
#include "spidev.h"
#include <bcm2835.h>
#include <getopt.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <strings.h>
#include <string.h>

#include <stdio.h>

#include "avrconfig.h"
#include "flightlog.h"
#include "mpu.h"

#define RESET_PIN RPI_GPIO_P1_22
#define RESET_TIMEOUT 10000

#define MAX_UNIX_CLIENTS 6
#define MAX_UDP_CLIENTS 6



//avrspi will keep SPI transfer active by sending MSG_RATE messages every MSG_PERIOD
#define MSG_RATE 8
#define MSG_PERIOD 50 //ms

//alternative: every 25 - 5 msg

int verbose;

int msg_counter = 0;

struct local_msg local_buf[64]; 
int local_buf_c=0;

#define AVR_OBUF_SIZE 16
struct avr_msg avr_obuf[AVR_OBUF_SIZE];
int avr_obuf_c = 0;

#define BUF_SIZE 64

int portno = 1030;

struct timespec time_now;
/* UDP */
#define CLIENT_TIMEOUT 10
struct sockaddr_in address;
struct sockaddr_in uaddress[MAX_UDP_CLIENTS];
int uclients[MAX_UDP_CLIENTS];
unsigned char ubuf[BUF_SIZE]; //input udp buffer 
struct sockaddr_in tmpaddress;
socklen_t addrlen = sizeof(tmpaddress);
int usock;
struct timespec udp_time_prev;
/* UDP END */

/* UNIX */
int unix_sock;
int sock[MAX_UNIX_CLIENTS]; 
int8_t sock_type[MAX_UNIX_CLIENTS];
struct sockaddr_un local;
int locallen;
unsigned char buf[MAX_UNIX_CLIENTS][BUF_SIZE]; //input tcp buffer for each client
unsigned short buf_c[MAX_UNIX_CLIENTS];
char socket_path[256] = "/dev/avrspi";
/* UNIX END */

int rpistatus = 0;
/* CONFIG */
int avrstatus = -1;
int avr_spi_check = 0;
int autoconfig = 1;
struct timespec reset_time_prev;
int reset_timeout = 0;

#define CFG_PATH "/etc/avrminicopter/"
struct s_config config;
/* END CONFIG */

int spi = 1;
int echo = 0;
int background = 0;
int stop = 0;
int log_mode;


struct timespec spi_time_prev,spi_last_msg;
struct timespec *dt;

void reset_avr();
void spi_add(uint8_t t, int16_t v);

void catch_signal(int sig)
{
	if (verbose) printf("Signal: %i\n",sig);
	stop = 1;
}

int udp_check_client(struct sockaddr_in *c) {
	int i;
	for (i=0;i<MAX_UDP_CLIENTS;i++)
		if (uclients[i]) //client active
			if ((c->sin_port == uaddress[i].sin_port) &&
					(c->sin_addr.s_addr == uaddress[i].sin_addr.s_addr)) {
				uclients[i] = CLIENT_TIMEOUT;
				return i;
			}

	return -1;
}

int udp_add_client(struct sockaddr_in *c) {
	if (verbose) printf("New client... ");
	int i;

	for (i=0;i<MAX_UDP_CLIENTS && uclients[i]>0;i++); //find a free spot

	if (i==MAX_UDP_CLIENTS) {
		if (verbose) printf("All spots taken!\n");
		return -1;
	} else {
		uclients[i] = CLIENT_TIMEOUT;
		memcpy(&uaddress[i],c,addrlen);
	}

	if (verbose) printf("OK: %i\n",i);
	return i;
}

void ping_back(struct local_msg *m, struct sockaddr_in *c) {
	static unsigned char buf[LOCAL_MSG_SIZE];
	int ret;

	if (verbose) printf("AVRSPI: Responding to ping %i\n",m->v);

	pack_lm(buf,m);

	ret = sendto(usock,buf,LOCAL_MSG_SIZE,0,(struct sockaddr *)c,addrlen);
	if (ret<0) {
		printf("AVRSPI: Error sending ping response\n");
	}
}

void process_msg_l(struct local_msg *m) {
	struct local_msg lm;
	switch (m->v) {
		case 0: break;
		case 1: autoconfig = 1; reset_avr(); break; 
		case 2: autoconfig = 0; reset_avr(); break; 
		case 3: //get number of SPI errors 
			lm.c = 0; lm.t = 253; lm.v=spi_crc_err;
			local_buf[local_buf_c++] = lm;
			//printf("%u %i\n",lm.t,lm.v);
			break;
		case 4: flog_save(); break;
		case 5: 
			lm.c = 0; lm.t = 254; lm.v=rpistatus;
			local_buf[local_buf_c++] = lm;
			break;
		default: printf("Unknown local message: %u\n",m->v);
	}
}

void process_msg(struct local_msg *m) {
	static struct avr_msg am;
	if (echo) 
		local_buf[local_buf_c++] = *m;

	if (m->t == 0) process_msg_l(m);
	else {
		if (verbose) printf("Forwarding to AVR t: %u v: %i\n",m->t,m->v);
		if (spi) {
			local2avr(m,&am);
			spi_add(am.t,am.v);
			flog_process_msg(&am);
		}
		clock_gettime(CLOCK_REALTIME, &spi_last_msg);	
		msg_counter++;
	}
}

void process_udp_msg(struct local_msg *m, struct sockaddr_in *addr) {
	int i;
	switch (m->c) {
		case 0: process_msg(m); break;
		case 1: case 2: 
			i = udp_check_client(&tmpaddress);
			if (i<0) { //new client
				i=udp_add_client(&tmpaddress);
			}
			if (m->c==2) ping_back(m,addr);
			else process_msg(m);
			break;
		case 3:
			local_buf[local_buf_c++] = *m;
			break;
		default: printf("Unknown UDP control: %u\n",m->c);
	}
}

void process_socket_queue(int client) {
	int msg_c = buf_c[client] / LOCAL_MSG_SIZE;
	int msg_r = buf_c[client] % LOCAL_MSG_SIZE;
	struct local_msg m;

	for (int i=0;i<msg_c;i++) {
		unpack_lm(buf[client]+i*LOCAL_MSG_SIZE,&m);
		switch (m.c) {
			case 0: process_msg(&m); break;
			case 3: 
				local_buf[local_buf_c++] = m;
			break;
			default: printf("Unknown SOCKET control: %u\n",m.c);
		}
	}

	if (msg_c) {
		for (int i=0;i<msg_r;i++)
			buf[client][i] = buf[client][msg_c*LOCAL_MSG_SIZE+i];
		buf_c[client] = msg_r;
	}
}

void reset_clients() {
	unsigned char bufout[LOCAL_MSG_SIZE];
	bufout[0] = 2; //reset msg
	for (int k=0;k<MAX_UNIX_CLIENTS;k++) {
		if (sock[k]!=0) 
			send(sock[k], bufout, LOCAL_MSG_SIZE, MSG_NOSIGNAL );
	}
}

void reset_avr() {
	reset_clients();
	if (verbose) printf("Reset AVR\n");

	avrstatus = -1;
	avr_spi_check = 0;
	avr_obuf_c = 0;
	bcm2835_gpio_write(RESET_PIN, LOW);
	bcm2835_delay(500);
	bcm2835_gpio_write(RESET_PIN, HIGH);
	bcm2835_delay(1000);
	spi_reset();
	
	if (autoconfig) {
		int ret = config_open(&config,CFG_PATH);
		if (ret<0) {
			printf("Disabling AUTOCONFIG\n");
			//printf("Failed to initiate config! [%s]\n", strerror(ret));	
			autoconfig = 0;
			return;
		}
		clock_gettime(CLOCK_REALTIME, &reset_time_prev);
		reset_timeout = RESET_TIMEOUT;
	}
}

void sendConfig() {
	if (verbose) printf("AVRSPI: Sending config\n");
        spi_sendIntPacket_delay(3,0); //initial mode
        spi_sendIntPacket_delay(2 ,log_mode); //log mode

        int gyro_orientation = inv_orientation_matrix_to_scalar(config.gyro_orient);
        spi_sendIntPacket_delay(4,gyro_orientation);

        spi_sendIntPacket_delay(9,config.mpu_addr);

        spi_sendIntPacket_delay(17,config.throttle_min);
        spi_sendIntPacket_delay(18,config.throttle_inflight);
        spi_sendIntPacket_delay(19,config.throttle_midflight);

	uint8_t motor_order = 
		(config.motor_pin[0]) |
		(config.motor_pin[1] << 2) |
		(config.motor_pin[2] << 4) |
		(config.motor_pin[3] << 6); 
	spi_sendIntPacket_delay(5,motor_order);

        spi_sendIntPacket_delay(69,config.baro_f);
        //PIDS
        for (int i=0;i<3;i++)
                for (int j=0;j<5;j++) {
                        spi_sendIntPacket_delay(100+i*10+j,config.r_pid[i][j]);
                        spi_sendIntPacket_delay(200+i*10+j,config.s_pid[i][j]);
                }

        for (int i=0;i<5;i++) {
                spi_sendIntPacket_delay(70+i,config.accel_pid[i]);
                spi_sendIntPacket_delay(80+i,config.alt_pid[i]);
                spi_sendIntPacket_delay(90+i,config.vz_pid[i]);
        }

        spi_sendIntPacket_delay(130,config.a_pid[0]);

	if (verbose) printf("AVRSPI: Config sent.\n");
}

void avr_spi_err_check(int v) {
	static int val = 0;

	if (avrstatus>=5) return;

	if (avr_spi_check==0) {
		val = v;
	} else if (avr_spi_check==1) {
		if (val!=v) //number of crc err increased
			rpistatus = 2;
		else 
			spi_sendIntPacket_delay(255,2);
	}

	avr_spi_check++;
}

void do_avr_init() {
	if (avrstatus>=5) return;
	static int prev_status = -1;
	static long dt_ms;
	struct timespec *dt;

	dt = TimeSpecDiff(&time_now,&reset_time_prev);
	dt_ms = dt->tv_sec*1000 + dt->tv_nsec/1000000;
	if (dt_ms>reset_timeout) { //we exceeded timeout waiting for a status change
		reset_avr();
		prev_status = -1;
		return;
	}

	if (prev_status == avrstatus) return;
	prev_status = avrstatus;
	reset_time_prev = time_now;

	if (verbose) printf("AVR status: %i\n",avrstatus);

	switch(avrstatus) {
		case -1: break;
		case 0: reset_avr(); prev_status = -1; break; //AVR should boot into status 1 so 0 means something wrong
		case 1: spi_sendIntPacket_delay(255,1); sendConfig(); spi_sendIntPacket_delay(255,1); prev_status = avrstatus = -1; reset_timeout=RESET_TIMEOUT; break;
		case 2: break; //AVR should arm motors and set status to 3
		case 3: break; //AVR is initializing MPU 
		case 4: reset_timeout=20000; break; //AVR is calibration gyro
		case 5: if (verbose) printf("Initialization OK.\n"); break;
		case 255: printf("AVRCONFIG: Gyro calibration failed!\n"); reset_avr(); break; //calibration failed
		default: printf("AVRCONFIG: Unknown AVR status %i\n",avrstatus); break;
	}
}

void process_avr_msg(struct avr_msg *m) { //will be called for every received msg from AVR
	flog_process_avrmsg(m);

	switch (m->t) {
		case 255: avrstatus = m->v; break; //status
		case 254: 
			if (avrstatus<5) avr_spi_err_check(m->v);
			break; //crc errors
	}
}

void print_usage() {
	printf("-d run in background\n");
	printf("-e run echo mode (useful for debugging)\n");
	printf("-f do not do SPI (useful for debugging)\n");
	printf("-v [level] verbose level\n");
	printf("-p [port] port to listen on (defaults to %i)\n",portno);
	printf("-u [SOCKET] socket to listen on (defaults to %s)\n",socket_path);
}

void spi_add(uint8_t t, int16_t v) {
	static bool overflow = false;
	if (avr_obuf_c>=AVR_OBUF_SIZE) {
		printf("AVR obuf overflow!\n");
		overflow = true; 
		rpistatus = 1;
	}

	if (!overflow) {
		avr_obuf[avr_obuf_c].t = t;
		avr_obuf[avr_obuf_c].v = v;
		avr_obuf_c++;
	}
}

int main(int argc, char **argv)
{
	/* SELECT */
	int max_fd;
	fd_set readfds;
	struct timeval timeout;
	/* END SELECT */
	int i,j,ret;
	unsigned char bufout[BUF_SIZE];
	struct avr_msg dummy_msg = {t: 255, v: 254};
	struct avr_msg status_msg = {t: 255, v: 0};
	long dt_ms = 0;


	if (!bcm2835_init())
        	return -1;

	bcm2835_gpio_fsel(RESET_PIN, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_set_pud(RESET_PIN, BCM2835_GPIO_PUD_OFF);
	bcm2835_gpio_write(RESET_PIN, HIGH);


	clock_gettime(CLOCK_REALTIME, &time_now);

	clock_gettime(CLOCK_REALTIME, &spi_time_prev);

	//we use this to measure UDP connection time each second
	clock_gettime(CLOCK_REALTIME, &udp_time_prev);

	int option;
	verbose = 1;
	background = 0;
	echo = 0;
	while ((option = getopt(argc, argv,"dep:fv:u:")) != -1) {
		switch (option)  {
			case 'd': background = 1; verbose=0; break;
			case 'p': portno = atoi(optarg);  break;
			case 'v': verbose = atoi(optarg);  break;
			case 'f': spi=0; break;
			case 'e': echo=1; break;
			case 'u': strcpy(socket_path,optarg); break;
			default:
				  print_usage();
				  return -1;
		}
	}

	signal(SIGTERM, catch_signal);
	signal(SIGINT, catch_signal);

	unix_sock = 0;
	for (i=0; i<MAX_UNIX_CLIENTS; i++) { 
		sock[i] = 0;
		sock_type[i] = -1;
		if (i>0) {
			buf_c[i] = 0;
		}
	}

	unix_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (unix_sock < 0) {
		perror("opening unix socket");
		exit(1);
	}

	usock = socket(AF_INET, SOCK_DGRAM, 0);
	if (usock < 0) {
		perror("opening datagram socket");
		exit(1);
	}

	/* Create name sock, usock */
	bzero((char *) &address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(portno);

	if (bind(usock, (struct sockaddr *) &address, sizeof(struct sockaddr_in))) {
		perror("binding datagram socket");
		exit(1);
	}

	printf("Socket created on port %i\n", portno);


	/* UNIX */
	bzero((char *) &local, sizeof(local));
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, socket_path);
	unlink(local.sun_path);
	locallen = strlen(local.sun_path) + sizeof(local.sun_family);
	if (bind(unix_sock, (struct sockaddr *) &local, locallen)) {
		perror("binding unix socket");
		exit(1);
	}


	flog_init(CFG_PATH);
	log_mode = flog_getmode();

	if (listen(unix_sock,3) < 0) {
		perror("listen");
		stop=1;
	}

	if (background) {
		if (daemon(0,1) < 0) { 
			perror("daemon");
			return -1;
		}
		if (verbose) printf("Running in the background\n");
	}


	if (spi) {
		ret = spi_init();
		if (ret < 0) {
			printf("Error initiating SPI! %i\n",ret);
			stop = 1;
		}
	}

	reset_avr();

	clock_gettime(CLOCK_REALTIME, &spi_last_msg);

	if (verbose) printf("Starting main loop\n");

	while (!stop) {
		FD_ZERO(&readfds);
		max_fd = unix_sock;
		FD_SET(unix_sock, &readfds);
		for (i=0;i<MAX_UNIX_CLIENTS;i++) {
			if (sock[i]<=0) continue;
			FD_SET(sock[i], &readfds);
			max_fd = sock[i]>max_fd?sock[i]:max_fd;
		}
		FD_SET(usock, &readfds);
		max_fd = usock>max_fd?usock:max_fd;

		timeout.tv_sec = 0;
		timeout.tv_usec = MSG_PERIOD*1000L; //ms 
		int sel = select( max_fd + 1 , &readfds , NULL , NULL , &timeout);
		if ((sel<0) && (errno!=EINTR)) {
			perror("select");
			stop=1;
		}

		clock_gettime(CLOCK_REALTIME, &time_now);

		//check for orphan UDP connections
		dt = TimeSpecDiff(&time_now,&udp_time_prev);
		dt_ms = dt->tv_sec*1000 + dt->tv_nsec/1000000;
		if (dt_ms>1000) {
			udp_time_prev = time_now;
			for (i=0;i<MAX_UDP_CLIENTS;i++)
				if (uclients[i]>0) {
					uclients[i]--;
					if (uclients[i]<=0) {
						if (verbose) printf("Client %i timeout.\n",i);
					}
				}
		}
		//check UDP
		if (!stop && FD_ISSET(usock, &readfds)) {
                        ret = 0;
                        int t = 0;
                        do {
                                t = recvfrom(usock, ubuf+ret, BUF_SIZE-ret, MSG_DONTWAIT, (struct sockaddr *)&tmpaddress, &addrlen);
                                if (t>0) ret+=t;
                        } while (t>0);
			if (ret<=0) {
				printf("UDP recvfrom error? %i\n",ret);
			} else {
				int msg_c = ret / LOCAL_MSG_SIZE;
				if (verbose) printf("UDP received %i msgs\n", msg_c);
				for (i=0;i<msg_c;i++) { 
					struct local_msg m;
					unpack_lm(ubuf+i*LOCAL_MSG_SIZE,&m);
					process_udp_msg(&m,&tmpaddress); 
				}
			}
		}

		//If something happened on the master socket , then its an incoming connection
		if (!stop && FD_ISSET(unix_sock, &readfds)) {
			int t = accept(unix_sock, NULL, NULL);
			if (t<0) {
				perror("accept");
				continue;
			}
			for (i=0;i<MAX_UNIX_CLIENTS;i++)
				if (sock[i] == 0) {
					if (verbose) printf("Incoming client: %i\n",i);
					sock[i] = t;
					sock_type[i] = -1;
					buf_c[i] = 0;
					break;
				}
			if (i==MAX_UNIX_CLIENTS) {
				printf("AVRSPI: No space in connection pool! Disconnecting client.\n");
				close(t);
			}
		} 
		for (i=0;(i<MAX_UNIX_CLIENTS) && (!stop) && sock[i]>0;i++) {
			if (FD_ISSET(sock[i], &readfds)) {
				if (sock_type[i]==-1) {
					ret = read(sock[i],&sock_type[i],1);
					if (ret<=0) {
						perror("Reading error");
						close(sock[i]);
						sock[i] = 0;
					}
					continue;
				}
				ret = read(sock[i] , buf[i]+buf_c[i], BUF_SIZE - buf_c[i]); 
				if (ret < 0) {
					perror("Reading error");
					close(sock[i]);
					sock[i] = 0;
				}
				else if (ret == 0) {	//client disconnected
					if (verbose) printf("Client %i disconnected.\n",i);
					close(sock[i]);
					sock[i] = 0;
					buf_c[i] = 0;
				} else { //got data
					buf_c[i] += ret;
					if (verbose) printf("Received: %i bytes. Buf size: %i\n",ret,buf_c[i]);
					process_socket_queue(i);
				}
			}
		}
		//process of SPI received messages
		for (i=0;i<spi_buf_c;i++) {
			process_avr_msg(&spi_buf[i]);
			avr2local(&spi_buf[i],&local_buf[local_buf_c++]);
		}
		spi_buf_c = 0;

		//send out any available messages to clients
		if (local_buf_c*LOCAL_MSG_SIZE>BUF_SIZE) {
			printf("output buffer overflow (bufout)!");
			local_buf_c = 0;
		}

		for (j=0;j<local_buf_c;j++) {
			if (verbose>=2) printf("To clients: c: %u, t: %u, v: %i\n",local_buf[j].c,local_buf[j].t,local_buf[j].v);
			//local_buf[j].c = 0;
			pack_lm(bufout+j*LOCAL_MSG_SIZE,&local_buf[j]);
		}

		if (local_buf_c) {
			if (verbose) printf("To clients msgs: %i bytes: %i\n",local_buf_c,local_buf_c*LOCAL_MSG_SIZE);
			for (int k=0;k<MAX_UNIX_CLIENTS;k++) {
				if (sock[k]!=0 && sock_type[k]==0) { 
					ret = send(sock[k], bufout, local_buf_c*LOCAL_MSG_SIZE, MSG_NOSIGNAL );
					if (ret == -1) {
						if (verbose) printf("Lost connection to client %i.\n",k);
						close(sock[k]);
						sock[k] = 0;
					}
				}
			}
			for (int k=0;k<MAX_UDP_CLIENTS;k++) {
				if (uclients[k]>0) { 
					ret = sendto(usock,bufout,LOCAL_MSG_SIZE,0,(struct sockaddr *)&uaddress[k],addrlen);
					if (ret<0) {
						printf("Error sending datagram packet\n");
						uclients[k] = 0;
					}
				}
			}
		}

		local_buf_c = 0;

		if (autoconfig)
			do_avr_init(); 

		flog_loop();

		//send any packets to avr

		j = avr_obuf_c;
		for (i=0;i<j;i++) { 
			if (i<MSG_RATE) {
				spi_sendIntPacket(avr_obuf[i].t,avr_obuf[i].v);
				avr_obuf_c--;
			}
			else avr_obuf[i-MSG_RATE] = avr_obuf[i];
		}

		for (j=i;j<MSG_RATE;j++) {
			if (j==i && autoconfig && avrstatus>0 && avrstatus<5) spi_sendMsg(&status_msg); 
			else spi_sendMsg(&dummy_msg);
		}
/*
		for (j=0;j<2;j++) {
			spi_sendMsg(&dummy_msg);
		}
*/			
	}

	if (spi) spi_close();

	bufout[0] = 1; //disconnect msg
	for (int k=0;k<MAX_UNIX_CLIENTS;k++) {
		if (sock[k]!=0) 
			send(sock[k], bufout, LOCAL_MSG_SIZE, MSG_NOSIGNAL );
	}

	close(unix_sock);
	mssleep(1000);

	if (verbose) {
		printf("closing\n");
		fflush(NULL);
	}

	for (i=0;i<MAX_UNIX_CLIENTS;i++)
		if (sock[i]!=0) close(sock[i]);


	close(usock);

	bcm2835_close();

}

