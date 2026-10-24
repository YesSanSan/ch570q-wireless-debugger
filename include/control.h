#ifndef WD_CONTROL_H
#define WD_CONTROL_H
/* CMSIS-DAP vendor commands. Local management never accesses the target. */
#define WD_CMD_LOCAL 0x80u
#define WD_CMD_REMOTE 0x81u
#define WD_CMD_UART 0x82u
#define WD_CTL_INFO 0u
#define WD_CTL_STATS 1u
#define WD_CTL_DISCOVER 2u
#define WD_CTL_DEVICES 3u
#define WD_CTL_GET_CONFIG 4u
#define WD_CTL_SET_CONFIG 5u
#define WD_CTL_SAVE_CONFIG 6u
#define WD_CTL_PAIR 7u
#define WD_CTL_ROLE 8u
#define WD_CTL_IDLE 10u
#define WD_CTL_APPLY 11u
#define WD_CTL_UART_STATS 12u
#define WD_CTL_RECONNECT 13u
#define WD_CTL_RECOVERY_STATS 14u
#define WD_STATUS_OK 0u
#define WD_STATUS_INVALID 1u
#define WD_STATUS_BUSY 2u
#define WD_STATUS_UNSUPPORTED 3u
#define WD_STATUS_IO 4u
#define WD_STATUS_DENIED 5u
#endif
