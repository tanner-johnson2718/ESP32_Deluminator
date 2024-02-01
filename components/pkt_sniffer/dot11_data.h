#pragma once
#include  <stdint.h>

//*****************************************************************************
// Data Frame Header |
// -------------------
//*****************************************************************************

typedef struct
{
    uint8_t tid:4;
    uint8_t ack_policy:2;
    uint8_t eosp:1;
    uint8_t res:1
    uint8_t : 0;

    uint8_t interpeted_byte;
} qos_ctrl_t;

// Normal MGMT Header
// QoS Control?
// CCMP paramteres?
// Optional 4th addr iff toDS == 1 && fromDS == 1

// PKT_DATA_DATA           = b0000 (subtype)

//*****************************************************************************
// PKT_NULL subtype = b0100 - its sole purpose is to carry the power management
// to let an AP know an STA's power management state
//*****************************************************************************
typedef dot11_header_t data_null_t;

// PKT_QOS_DATA            = b1000 (subtype)
// PKT_QOS_CF_ACK          = b1001 (subtype)
// PKT_QOS_CF_POLL         = b1010 (subtype)
// PKT_QOS_CF_ACK_CF_POLL  = b1011 (subtype)

//*****************************************************************************
// PKT_QOS_NULL, subtype = b1100
//*****************************************************************************
typedef struct 
{
    dot11_header_t header;
    qos_ctrl_t qos_ctrl;
} data_qos_null_t;

// PKT_QOS_CF_POLL_NULL    = b1110 (subtype)
// PKT_CF_ACK_CF_POLL_NULL = b1111 (subtype)