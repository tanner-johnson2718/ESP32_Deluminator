#pragma once
#include  <stdint.h>

//*****************************************************************************
// Data Frame Header |
// -------------------
//
//*****************************************************************************

#define DATA_QOS_BIT 4

// Normal MGMT Header
// QoS Control?
// CCMP paramteres?
// Optional 4th addr iff toDS == 1 && fromDS == 1

// PKT_DATA_DATA           = b0000
// PKT_NULL                = b0100
// PKT_QOS_DATA            = b1000 
// PKT_QOS_CF_ACK          = b1001
// PKT_QOS_CF_POLL         = b1010
// PKT_QOS_CF_ACK_CF_POLL  = b1011
// PKT_QOS_NULL            = b1100
// PKT_QOS_CF_POLL_NULL    = b1110
// PKT_CF_ACK_CF_POLL_NULL = b1111