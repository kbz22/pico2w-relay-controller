#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// Bare-metal Pico W setup (known-good SDK-style options).
#define NO_SYS                      1
#define LWIP_DHCP                   1
#define LWIP_RAW                    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_DNS                    1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#endif
