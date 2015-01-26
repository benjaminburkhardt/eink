//#define USE_US_TIMER
#include "espmissingincludes.h"
#include "ets_sys.h"
#include "stdout.h"
#include "c_types.h"
#include "user_interface.h"
#include "osapi.h"
#include "espconn.h"
#include "io.h"
#include "httpdclient.h"


//For first run: set ESSID and STA mode, no sleep so Vcomm can be adjusted

//#define FIRST

extern uint8_t at_wifiMode;
void user_init(void);

static ETSTimer tstTimer;
static ETSTimer wdtTimer;
static ETSTimer klontTimer;


#define INK_STARTUP 0
#define INK_VSTART 1
#define INK_HSEND 2
#define INK_VSTOP 3
#define INK_PAUSED 4
#define INK_RESUME 5
static int einkState=0;
static int einkYpos;
static int einkPat=0;

#define BMFIFOLEN (1024*32)
static char bmBuff[BMFIFOLEN];
static char *bmRpos, *bmWpos;
static char tcpConnOpen;


void sleepmode();

static int ICACHE_FLASH_ATTR fifoLen() {
	int l=bmWpos-bmRpos;
	if (l<0) l+=BMFIFOLEN;
	return l;
}

static void ICACHE_FLASH_ATTR tstTimerCb(void *arg) {
	int x;
	int rep=0;
	if (einkState==INK_STARTUP) {
		einkPat=0;
		ioEinkEna(1);
		os_timer_arm(&tstTimer, 100, 0);
		einkState=INK_VSTART;
		REG_SET_BIT(0x3ff00014, BIT(0));
		os_update_cpu_frequency(160);
	} else if (einkState==INK_PAUSED) {
		if (fifoLen()>(BMFIFOLEN-17300) || !tcpConnOpen) {
			//Wake up e-ink display!
			ioEinkEna(1);
			os_timer_arm(&tstTimer, 100, 0);
			einkState=INK_RESUME;
		} else {
			//Keep sleeping
			os_timer_arm(&tstTimer, 100, 0);
		}
	} else if (einkState==INK_RESUME) {
		//Eink has just been powered on to resume writing. Move back to current line.
		ioEinkVscanStart();
		REG_SET_BIT(0x3ff00014, BIT(0));
		os_update_cpu_frequency(160);
		if (einkYpos!=0) {
			ioEinkWrite(0);
			ioEinkClk(800/4);
			ioEinkVscanWrite(15);
			for (x=1; x<einkYpos; x++) {
				ioEinkHscanStart();
				ioEinkHscanStop();
				ioEinkVscanSkip();
			}
		}
		einkState=INK_HSEND;
		os_timer_arm(&tstTimer, 0, 0);
	} else if (einkState==INK_VSTART) {
		ioEinkVscanStart();
		einkYpos=0;
		einkState=INK_HSEND;
		os_timer_arm(&tstTimer, 20, 0);
	} else if (einkState==INK_HSEND) {
		if (einkPat<4) {
			os_timer_arm(&tstTimer, 0, 0);
			ioEinkHscanStart();
	
			if (einkPat==0 || einkPat==1) {
				ioEinkWrite(0x55);
				ioEinkClk(800/4);
			}
			if (einkPat==2 || einkPat==3) {
				ioEinkWrite(0xaa);
				ioEinkClk(800/4);
			}
			ioEinkHscanStop();
			ioEinkVscanWrite(15);
			einkYpos++;
			if (einkYpos==600) {
				einkState=INK_VSTOP;
			}
		} else {
			//Calculate length of data in fifo
			if (fifoLen()<(800/4) && tcpConnOpen) {
				//Fifo ran dry. Kill eink power and sleep to wait for more data.
				REG_CLR_BIT(0x3ff00014, BIT(0));
				os_update_cpu_frequency(80);
				ioEinkEna(0);
				einkState=INK_PAUSED;
				os_timer_arm(&tstTimer, 20, 0);
			} else {
				//We can draw stuf. Reschedule ASAP please.
				os_timer_arm(&tstTimer, 0, 0);
				ioEinkHscanStart();

				for (x=0; x<800; x+=4) {
					ioEinkWrite(*bmRpos++);
					if (bmRpos>&bmBuff[BMFIFOLEN-1]) bmRpos=bmBuff;
					if (((*bmRpos)&0xc0)==0xc0) {
						rep=(*bmRpos++)&0x3f;
						x+=rep*4;
						ioEinkClk(rep);
						if (bmRpos>&bmBuff[BMFIFOLEN-1]) bmRpos=bmBuff;
					}
				}
				ioEinkHscanStop();
				ioEinkVscanWrite(115);
				einkYpos++;
				if (einkYpos==600) {
					einkState=INK_VSTOP;
				}
			}
		}

	} else if (einkState==INK_VSTOP) {
		//Skip any remaining lines
		ioEinkWrite(0);
		ioEinkClk(800/4);
		for (x=einkYpos; x<620; x++) {
			ioEinkHscanStart();
			ioEinkHscanStop();
			ioEinkVscanWrite(15);
		}

		ioEinkVscanStop();
		if (einkPat==4) {
			einkState=INK_STARTUP;
			os_printf("Done displaying image. Sleeping.\n");
			ioEinkEna(0);
			sleepmode();
		} else {
			einkPat++;
			einkState=INK_VSTART;
			os_timer_arm(&tstTimer, 10, 0);
		}
	}
}



void cb(char* data, int len) {
	int x;
//	os_printf("Data: %d bytes\n", len);
	if (len==0) {
		tcpConnOpen=0;
	}
	for (x=0; x<len; x++) {
		*bmWpos++=data[x];
		if (bmWpos>&bmBuff[BMFIFOLEN-1]) {
//			os_printf("fifo: w wraparound\n");
			bmWpos=bmBuff;
		}
		if (bmWpos==bmRpos) {
			os_printf("fifo: OVERFLOW!\n");
		}
	}
}


static void ICACHE_FLASH_ATTR wdtTimerCb(void *arg) {
	os_printf("Wdt. This takes too long. Go to sleep.\n");
	ioEinkEna(0);
	sleepmode();
}

static struct station_config stconf;

static void ICACHE_FLASH_ATTR klontTimerCb(void *arg) {
	os_printf("klonttimer\n");
	if (wifi_get_opmode()!=1) {
		wifi_set_opmode(1);
		system_restart();
	}
	os_strncpy((char*)stconf.ssid, "Sprite", 32);
	os_strncpy((char*)stconf.password, "", 64);
	wifi_station_set_config(&stconf);
	wifi_station_connect();
}

void sleepmode() {
#ifdef FIRST
	ioEinkEna(1);
#else
//	wifi_set_sleep_type(MODEM_SLEEP_T);
	system_deep_sleep(60*1000*1000);
#endif
}




void user_init(void)
{
//	system_timer_reinit();
	stdoutInit();
	ioInit();

#ifdef FIRST
	//Temp store for new ap info.
	static struct station_config stconf;
	os_strncpy((char*)stconf.ssid, "Sprite", 32);
	os_strncpy((char*)stconf.password, "", 64);
	wifi_station_disconnect();
	wifi_station_set_config(&stconf);
	wifi_station_connect();
	wifi_set_opmode(1);
#endif

	bmRpos=bmBuff;
	bmWpos=bmBuff;
	tcpConnOpen=1;
	httpclientFetch("meuk.spritesserver.nl", "/espbm.php", 1024, cb);

	os_timer_disarm(&tstTimer);
	os_timer_setfn(&tstTimer, tstTimerCb, NULL);
	os_timer_arm(&tstTimer, 1000, 0);

	os_timer_disarm(&klontTimer);
	os_timer_setfn(&klontTimer, klontTimerCb, NULL);
//	os_timer_arm(&klontTimer, 2000, 1);

	os_timer_disarm(&wdtTimer);
	os_timer_setfn(&wdtTimer, wdtTimerCb, NULL);
	os_timer_arm(&wdtTimer, 20000, 0); //shut down after 15 seconds
}
