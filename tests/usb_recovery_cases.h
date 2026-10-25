/* Included by test_usb.c: exercises real USB handler in IRQ and poll builds. */
static uint32_t recovery_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void recovery_setup(void)
{
    setup(0x41, WD_USB_RECOVERY_REQUEST, WD_USB_RECOVERY_MAGIC, 1, 0);
}
static void recovery_status(uint8_t snapshot[24])
{
    setup(0xc1, WD_USB_RECOVERY_REQUEST, 0, 1, WD_USB_STATUS_SIZE);
    assert(control_state==CTRL_IN && R8_UEP0_T_LEN==24);
    memcpy(snapshot,ep0_buffer,24);in0();out0(0);
    assert(!memcmp(snapshot,"WUSB",4) && snapshot[4]==1 && !snapshot[11]);
}
static void test_ep0_recovery(void)
{
    uint8_t packet[64]={0x05},length,snapshot[24];uint32_t tag;
    /* Both transports, before execution, during RF, and with unread response. */
    for(unsigned ep=1;ep<=2;++ep) for(unsigned state=REQ_READY;state<=REQ_RESPONSE;++state) {
        usb_dap_init();R8_USB_INT_FG=0;configure();
        setup(0x21,0x22,3,2,0);in0();
        memset(ep3_buffer,0x6a,9);event(UIS_TOKEN_OUT,3,9,RB_UIS_TOG_OK);
        assert(usb_cdc_write(packet,4)==4);
        memset(ep0_buffer+64,0x7b,16); /* Notification DMA is separate from EP0. */
        uint8_t *dma=ep==1?ep1_buffer:ep2_buffer;
        memcpy(dma,packet,2);event(UIS_TOKEN_OUT,ep,2,RB_UIS_TOG_OK);
        tag=generation;
        if(state>=REQ_PROCESSING) assert(usb_dap_take_request_tagged(packet,&length,&tag));
        if(state==REQ_RESPONSE) assert(usb_dap_send_response_tagged(packet,2,tag));
        assert(request_state==state);
        for(unsigned i=0;i<10;++i) {usb_dap_poll();R8_USB_INT_FG=0;}
        assert(request_state==state); /* No automatic expiry of live responses. */
        assert((R8_UEP1_CTRL&MASK_UEP_R_RES)==UEP_R_RES_NAK);
        assert((R8_UEP2_CTRL&MASK_UEP_R_RES)==UEP_R_RES_NAK);
        recovery_status(snapshot);
        assert(snapshot[5]==state && snapshot[6]==ep && snapshot[7]==1);
        assert(recovery_u32(snapshot+12)==tag && !recovery_u32(snapshot+16));
        assert(request_state==state && generation==tag && cdc_rx_length==9 && cdc_tx_busy);
        for(unsigned i=64;i<80;++i) assert(ep0_buffer[i]==0x7b);
        /* Reset intent must not discard anything before its status completes. */
        recovery_setup();assert(control_state==CTRL_STATUS_IN && generation==tag && request_state==state);
        setup(0x80,USB_GET_CONFIGURATION,0,0,1);in0();out0(0);
        assert(generation==tag && request_state==state && !usb_recoveries);
        /* Preserve arbitrary/nonzero DAP toggles and HALT bits, as well as CDC. */
        R8_UEP1_CTRL|=RB_UEP_T_TOG;R8_UEP2_CTRL|=RB_UEP_R_TOG;
        halt_in=1u<<1;halt_out=1u<<2;ep1_update();
        uint8_t ctrl1=R8_UEP1_CTRL&(RB_UEP_T_TOG|RB_UEP_R_TOG);
        uint8_t ctrl2=R8_UEP2_CTRL&(RB_UEP_T_TOG|RB_UEP_R_TOG);
        recovery_setup();in0();
        assert(generation==tag+1 && usb_recoveries==1 && request_state==REQ_IDLE);
        assert(transport==USB_DAP_TRANSPORT_NONE && !R8_UEP1_T_LEN && !R8_UEP2_T_LEN);
        assert((R8_UEP1_CTRL&(RB_UEP_T_TOG|RB_UEP_R_TOG))==ctrl1);
        assert((R8_UEP2_CTRL&(RB_UEP_T_TOG|RB_UEP_R_TOG))==ctrl2);
        assert(halt_in==(1u<<1) && halt_out==(1u<<2));
        assert(cdc_rx_length==9 && cdc_tx_busy && usb_cdc_control_line_state()==3);
        assert(line_coding[0]==0 && line_coding[1]==0xc2 && line_coding[6]==8);
        for(unsigned i=0;i<9;++i) assert(ep3_buffer[i]==0x6a);
        for(unsigned i=64;i<80;++i) assert(ep0_buffer[i]==0x7b);
        in0();assert(usb_recoveries==1); /* Duplicate status interrupt cannot recommit. */
        usb_dap_ack_generation(tag);assert(cleanup_generation!=generation);
        usb_dap_ack_generation(generation);assert(cleanup_generation==generation);
        recovery_status(snapshot);assert(recovery_u32(snapshot+16)==1);
        assert(recovery_u32(snapshot+20)==generation);
        /* A new operation must not accept the old generation's result/discard. */
        halt_in=halt_out=0;ep1_update();dma[0]=0x00;dma[1]=4;
        event(UIS_TOKEN_OUT,ep,2,RB_UIS_TOG_OK);
        uint32_t fresh;assert(usb_dap_take_request_tagged(packet,&length,&fresh));
        assert(!usb_dap_send_response_tagged(packet,2,tag));
        assert(!usb_dap_discard_request_tagged(tag) && request_state==REQ_PROCESSING);
        assert(usb_dap_send_response_tagged(packet,2,fresh));
        event(UIS_TOKEN_IN,ep,0,0);assert(request_state==REQ_IDLE);
    }
    /* Strict request validation, including direction/recipient/index/length. */
    static const uint16_t bad[][5]={
        {0x41,0,1,0,0},{0x41,WD_USB_RECOVERY_MAGIC,0,0,0},
        {0x40,WD_USB_RECOVERY_MAGIC,1,0,0},{0x41,WD_USB_RECOVERY_MAGIC,1,1,0},
        {0xc1,1,1,24,0},{0xc1,0,0,24,0},{0xc1,0,1,23,0},{0xc1,0,1,25,0}
    };
    usb_dap_init();R8_USB_INT_FG=0;configure();tag=generation;
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        setup(bad[i][0],WD_USB_RECOVERY_REQUEST,bad[i][1],bad[i][2],bad[i][3]);
        assert(control_state==CTRL_STALL && generation==tag && !usb_recoveries);
    }
    /* A retained previous status completion with a new SETUP commits first. */
    recovery_setup();
    setup_status(0xc1,WD_USB_RECOVERY_REQUEST,0,1,24,RB_UIS_SETUP_ACT|UIS_TOKEN_IN);
    assert(generation==tag+1 && usb_recoveries==1 && control_state==CTRL_IN);
    assert(recovery_u32(ep0_buffer+16)==1);in0();out0(0);
    /* An accepted DAP IN before recovery SETUP changes its toggle exactly once. */
    memcpy(ep2_buffer,packet,2);event(UIS_TOKEN_OUT,2,2,RB_UIS_TOG_OK);
    assert(usb_dap_take_request_tagged(packet,&length,&tag));
    assert(usb_dap_send_response_tagged(packet,2,tag));
    uint8_t toggle=R8_UEP2_CTRL&RB_UEP_T_TOG;
    setup_status(0x41,WD_USB_RECOVERY_REQUEST,WD_USB_RECOVERY_MAGIC,1,0,
                 RB_UIS_SETUP_ACT|UIS_TOKEN_IN|2);
    assert((R8_UEP2_CTRL&RB_UEP_T_TOG)!=toggle && generation==tag);
    in0();assert(generation==tag+1 && usb_recoveries==2);
    /* Accepted OUT before reset SETUP is discarded only at status ACK. */
    memcpy(ep1_buffer,packet,2);toggle=R8_UEP1_CTRL&RB_UEP_R_TOG;
    setup_rx_len=2;
    setup_status(0x41,WD_USB_RECOVERY_REQUEST,WD_USB_RECOVERY_MAGIC,1,0,
                 RB_UIS_SETUP_ACT|UIS_TOKEN_OUT|RB_UIS_TOG_OK|1);
    setup_rx_len=8;
    assert(request_state==REQ_READY && (R8_UEP1_CTRL&RB_UEP_R_TOG)!=toggle);
    in0();assert(request_state==REQ_IDLE && usb_recoveries==3);
    /* Bus reset cancels uncommitted vendor action, not counted as recovery. */
    recovery_setup();R8_USB_INT_FG=RB_UIF_BUS_RST;usb_dap_poll();R8_USB_INT_FG=0;
    assert(!configuration && usb_recoveries==3 && control_state==CTRL_IDLE);
    recovery_setup();assert(control_state==CTRL_STALL && usb_recoveries==3);
    /* Recovery counter wraps independently of ordinary generation changes. */
    configure();usb_recoveries=UINT32_MAX;recovery_setup();in0();assert(!usb_recoveries);
    /* Two resets before foreground service cannot share an acknowledgement. */
    recovery_setup();in0();uint32_t first_generation=generation;
    recovery_setup();in0();assert(generation==first_generation+1);
    usb_dap_ack_generation(first_generation);assert(cleanup_generation!=generation);
    usb_dap_ack_generation(generation);assert(cleanup_generation==generation);
    /* ack_generation services a pending status ACK: its caller's tag is stale. */
    tag=generation;recovery_setup();
    R8_USB_INT_FG=RB_UIF_TRANSFER;R8_USB_INT_ST=UIS_TOKEN_IN;
    usb_dap_ack_generation(tag);R8_USB_INT_FG=0;
    assert(generation==tag+1 && cleanup_generation==tag);
    usb_dap_ack_generation(generation);assert(cleanup_generation==generation);
}
