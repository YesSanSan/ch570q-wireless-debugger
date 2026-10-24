#ifndef RADIO_MOCK_RF_H
#define RADIO_MOCK_RF_H
#include <stdint.h>
typedef uint8_t bStatus_t;
typedef uint32_t rfRole_States_t;
typedef void (*pfnRfRoleProcess)(rfRole_States_t, uint8_t);
#define RF_STATE_RX 1u
#define RF_STATE_TIMEOUT 4u
#define RF_STATE_RX_CRCERR 8u
#define RF_STATE_TX_FINISH 16u
#define PHY_MODE_PHY_2M 16u
#define LL_TX_PWR_0_DBM 0x12u
typedef struct { pfnRfRoleProcess rfProcessCB; uint32_t processMask; } rfRoleConfig_t;
typedef struct {
    uint32_t accessAddress;
    uint8_t accessAddressEx;
    uint32_t crcInit, frequency, properties, txDMA;
    uint8_t whiteChannel, txLen, waitTime;
    int8_t txPowerVal;
    uint32_t crcPoly;
} rfipTx_t;
typedef struct {
    uint32_t accessAddress;
    uint8_t accessAddressEx;
    uint32_t crcInit, frequency, properties, rxDMA;
    uint8_t whiteChannel, rxMaxLen;
    uint32_t timeOut, crcPoly;
} rfipRx_t;
bStatus_t RFRole_BasicInit(rfRoleConfig_t *config);
bStatus_t RFRole_Stop(void);
bStatus_t RFIP_StartTx(rfipTx_t *config);
bStatus_t RFIP_SetRx(rfipRx_t *config);
void BB_LibIRQHandler(void);
void LLE_LibIRQHandler(void);
#endif
