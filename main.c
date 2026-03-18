/*
 * Based on Xilinx lwIP RAW echo-server template main.c
 * Integrated with:
 *  - PmodOLED display of current mode (MANUAL / TRACKING)
 *  - PmodJSTK2 joystick controlling a servo via AXI GPIO PWM
 *  - TCP RX lines used as tracking commands: "left", "right", "none"
 */

#include <stdio.h>
#include <string.h>

#include "xparameters.h"
#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"

#if defined (__arm__) || defined(__aarch64__)
#include "xil_printf.h"
#endif

#include "lwip/tcp.h"
#include "lwip/init.h"
#include "xil_cache.h"
#include "sleep.h"

// IPv4 / DHCP from template
#if LWIP_IPV6==1
#include "lwip/ip.h"
#else
#if LWIP_DHCP==1
#include "lwip/dhcp.h"
#endif
#endif

// ---------------- Added: OLED / Joystick / Servo ----------------
#include "PmodOLED.h"
#include "PmodJSTK2.h"
#include "xgpio.h"

// ---------------- Servo + Joystick ----------------
#define SERVO_GPIO_DEVICE_ID   XPAR_AXI_GPIO_0_DEVICE_ID
#define SERVO_GPIO_CHANNEL     1

#define JSTK2_SPI_BASEADDR     XPAR_PMODJSTK2_0_AXI_LITE_SPI_BASEADDR
#define JSTK2_GPIO_BASEADDR    XPAR_PMODJSTK2_0_AXI_LITE_GPIO_BASEADDR

#define SERVO_PERIOD_US        20000
#define SERVO_MIN_PULSE_US     1000
#define SERVO_MAX_PULSE_US     2000

// ---------------- OLED ----------------
static PmodOLED  myDevice;
static PmodJSTK2 joystick;
static XGpio     ServoGpio;

// Match your working demo constants
static const u8 orientation = 0x0;
static const u8 invert      = 0x0;

// ---------------- TCP command shared with echo.c ----------------
#define RX_LINE_MAX 32
volatile char g_tcp_cmd[RX_LINE_MAX] = "none";   // last complete line from TCP
volatile int  g_tcp_cmd_updated = 1;             // flag for debugging/optional

// ---------------- Control mode ----------------
typedef enum { MODE_MANUAL = 0, MODE_TRACKING = 1 } ControlMode;
static ControlMode g_mode = MODE_MANUAL;

// Tracking parameters
static float g_servo_angle = 90.0f;
static const float TRACK_STEP_DEG = 0.5f;   // degrees per 20ms loop (~100 deg/s)

// ------------------------------------------------------------
// Template externs/timers/hooks (implemented in echo.c)
// ------------------------------------------------------------
void tcp_fasttmr(void);
void tcp_slowtmr(void);
void lwip_init();

void print_app_header(void);
int  start_application(void);
int  transfer_data(void);

#if LWIP_IPV6==0
#if LWIP_DHCP==1
extern volatile int dhcp_timoutcntr;
err_t dhcp_start(struct netif *netif);
#endif
#endif

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

static struct netif server_netif;
struct netif *echo_netif;

// ============================================================
// Helpers
// ============================================================
static void OLED_ShowMode(ControlMode m)
{
    OLED_ClearBuffer(&myDevice);
    OLED_SetCursor(&myDevice, 0, 0);
    OLED_PutString(&myDevice, "Mode:");

    OLED_SetCursor(&myDevice, 0, 2);
    if (m == MODE_TRACKING) OLED_PutString(&myDevice, "TRACKING");
    else                   OLED_PutString(&myDevice, "MANUAL");

    Xil_DCacheFlush();
    OLED_Update(&myDevice);
}

// (Optional debug helper if you ever want it again)
void OLED_Show(const char *rx)
{
    OLED_ClearBuffer(&myDevice);
    OLED_SetCursor(&myDevice, 0, 0);
    OLED_PutString(&myDevice, "DBG:");
    OLED_SetCursor(&myDevice, 0, 2);
    OLED_PutString(&myDevice, (char*)rx);
    Xil_DCacheFlush();
    OLED_Update(&myDevice);
}

static void Servo_SendPulse(int pulse_us)
{
    XGpio_DiscreteWrite(&ServoGpio, SERVO_GPIO_CHANNEL, 1);
    usleep(pulse_us);
    XGpio_DiscreteWrite(&ServoGpio, SERVO_GPIO_CHANNEL, 0);
    usleep(SERVO_PERIOD_US - pulse_us);
}

static int Servo_AngleToPulseUs(float angle_deg)
{
    if (angle_deg < 0.0f) angle_deg = 0.0f;
    if (angle_deg > 180.0f) angle_deg = 180.0f;

    return SERVO_MIN_PULSE_US +
           (int)((angle_deg / 180.0f) * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US));
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

// ============================================================
// IP print helpers from template
// ============================================================
#if LWIP_IPV6==1
void print_ip6(char *msg, ip_addr_t *ip)
{
    print(msg);
    xil_printf(" %x:%x:%x:%x:%x:%x:%x:%x\n\r",
            IP6_ADDR_BLOCK1(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK2(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK3(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK4(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK5(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK6(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK7(&ip->u_addr.ip6),
            IP6_ADDR_BLOCK8(&ip->u_addr.ip6));
}
#else
void print_ip(char *msg, ip_addr_t *ip)
{
    print(msg);
    xil_printf("%d.%d.%d.%d\n\r", ip4_addr1(ip), ip4_addr2(ip),
            ip4_addr3(ip), ip4_addr4(ip));
}

void print_ip_settings(ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw)
{
    print_ip("Board IP: ", ip);
    print_ip("Netmask : ", mask);
    print_ip("Gateway : ", gw);
}
#endif

// ============================================================
// main
// ============================================================
int main()
{
#if LWIP_IPV6==0
    ip_addr_t ipaddr, netmask, gw;
#endif

    // Unique MAC per board (edit last byte if you use multiple boards)
    unsigned char mac_ethernet_address[] =
    { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

    echo_netif = &server_netif;

    init_platform();

    // ---------- OLED init (known-good style) ----------
    OLED_Begin(&myDevice,
        XPAR_PMODOLED_0_AXI_LITE_GPIO_BASEADDR,
        XPAR_PMODOLED_0_AXI_LITE_SPI_BASEADDR,
        orientation, invert);
    OLED_SetCharUpdate(&myDevice, 0);

    // start in MANUAL mode
    OLED_ShowMode(g_mode);

    // ---------- Servo GPIO init ----------
    if (XGpio_Initialize(&ServoGpio, SERVO_GPIO_DEVICE_ID) != XST_SUCCESS) {
        OLED_Show("Servo GPIO fail");
        return -1;
    }
    XGpio_SetDataDirection(&ServoGpio, SERVO_GPIO_CHANNEL, 0x0);

    // ---------- Joystick init ----------
    JSTK2_begin(&joystick, JSTK2_SPI_BASEADDR, JSTK2_GPIO_BASEADDR);

#if LWIP_IPV6==0
#if LWIP_DHCP==1
    ipaddr.addr = 0; gw.addr = 0; netmask.addr = 0;
#else
    // Example network (change to match your PC-side interface IP/subnet)
    IP4_ADDR(&ipaddr,  192, 168, 10, 50);   // board
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 10, 1);    // host (if you’re using one)
#endif
#endif

    print_app_header();
    lwip_init();

#if (LWIP_IPV6 == 0)
    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw,
                   mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding N/W interface\n\r");
        OLED_Show("xemac_add fail");
        return -1;
    }
#else
    if (!xemac_add(echo_netif, NULL, NULL, NULL,
                   mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding N/W interface\n\r");
        OLED_Show("xemac_add fail");
        return -1;
    }
    echo_netif->ip6_autoconfig_enabled = 1;
    netif_create_ip6_linklocal_address(echo_netif, 1);
    netif_ip6_addr_set_state(echo_netif, 0, IP6_ADDR_VALID);
    print_ip6("\n\rBoard IPv6 address ", &echo_netif->ip6_addr[0].u_addr.ip6);
#endif

    netif_set_default(echo_netif);

    platform_enable_interrupts();
    netif_set_up(echo_netif);

#if (LWIP_IPV6 == 0)
#if (LWIP_DHCP==1)
    dhcp_start(echo_netif);
    dhcp_timoutcntr = 24;

    while(((echo_netif->ip_addr.addr) == 0) && (dhcp_timoutcntr > 0))
        xemacif_input(echo_netif);

    if (dhcp_timoutcntr <= 0) {
        if ((echo_netif->ip_addr.addr) == 0) {
            xil_printf("DHCP Timeout\r\n");
            xil_printf("Configuring default IP of 192.168.10.50\r\n");
            IP4_ADDR(&(echo_netif->ip_addr),  192, 168, 10, 50);
            IP4_ADDR(&(echo_netif->netmask), 255, 255, 255, 0);
            IP4_ADDR(&(echo_netif->gw),      0, 0, 0, 0);
        }
    }

    ipaddr.addr = echo_netif->ip_addr.addr;
    gw.addr     = echo_netif->gw.addr;
    netmask.addr= echo_netif->netmask.addr;
#endif

    print_ip_settings(&ipaddr, &netmask, &gw);
#endif

    if (start_application() != 0) {
        OLED_Show("app start fail");
        return -1;
    }

    // Trigger edge detect (NOTE: field name may differ; see comment below)
    u8 prev_trigger = 0;

    while (1) {
        if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }
        if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }

        // pump lwIP
        xemacif_input(echo_netif);
        transfer_data();

        // read joystick
        JSTK2_DataPacket pkt = JSTK2_getDataPacket(&joystick);

        // ---- trigger button (YOU MAY NEED TO ADJUST THIS LINE) ----
        // Some Digilent JSTK2 drivers use pkt.Buttons, others pkt.Btns.
        u8 trigger = 0;
        trigger = pkt.Trigger;  // <-- if compile error: change Buttons -> Btns
        // rising edge toggles modes
        if (trigger && !prev_trigger) {
            g_mode = (g_mode == MODE_MANUAL) ? MODE_TRACKING : MODE_MANUAL;
            OLED_ShowMode(g_mode);
        }
        prev_trigger = trigger;

        // ---- servo control depending on mode ----
        if (g_mode == MODE_MANUAL) {
            // joystick X -> angle
            g_servo_angle = ((float)pkt.XData / 1023.0f) * 180.0f;
        } else {
            // tracking: TCP command drives continuous stepping
            // echo.c normalizes to lowercase trimmed commands: left/right/none
            if (starts_with((const char*)g_tcp_cmd, "right")) {
                g_servo_angle -= TRACK_STEP_DEG;
            } else if (starts_with((const char*)g_tcp_cmd, "left")) {
                g_servo_angle += TRACK_STEP_DEG;
            } else {
                // "none" -> hold
            	g_servo_angle = g_servo_angle;
            }
        }

        // clamp and output one PWM period
        if (g_servo_angle < 0.0f)   g_servo_angle = 0.0f;
        if (g_servo_angle > 180.0f) g_servo_angle = 180.0f;

        int pulse_us = Servo_AngleToPulseUs(g_servo_angle);
        Servo_SendPulse(pulse_us);
    }

    cleanup_platform();
    return 0;
}

