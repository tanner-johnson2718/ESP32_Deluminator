#pragma once
#include <stdint.h>

//*****************************************************************************
// PKT Type and Subtype enums. We use these to classify packets.
//*****************************************************************************

typedef enum 
{
    PKT_MGMT = 0,
    PKT_CTRL = 1,
    PKT_DATA = 2,
    PKT_EXT  = 3,
    PKT_ANY  = 4
} pkt_type_t;

typedef enum
{
    PKT_ASSOC_REQ = 0,
    PKT_ASSOC_RES = 1,
    PKT_REASSOC_REQ = 2,
    PKT_REASSOC_RES = 3,
    PKT_PROBE_REQ = 4,
    PKT_PROBE_RES = 5,
    PKT_TIMING_ADVERT = 6,
    PKT_MGMT_RES0 = 7,
    PKT_BEACON = 8,
    PKT_ATIM = 9,
    PKT_DISASSOC = 10,
    PKT_AUTH = 11,
    PKT_DEAUTH = 12,
    PKT_ACTION = 13,
    PKT_NACK = 14,
    PKT_MGMT_RES1 = 15,
    PKT_MGMT_ANY = 16
} mgmt_pkt_subtype_t;

typedef enum
{
    PKT_DATA_DATA = 0,
    PKT_DATA_RES0 = 1,
    PKT_DATA_RES1 = 2,
    PKT_DATA_RES2 = 3,
    PKT_NULL = 4,
    PKT_DATA_RES3 = 5,
    PKT_DATA_RES4 = 6,
    PKT_DATA_RES5 = 7,
    PKT_QOS_DATA = 8,
    PKT_QOS_CF_ACK = 9,
    PKT_QOS_CF_POLL = 10,
    PKT_QOS_CF_ACK_CF_POLL = 11,
    PKT_QOS_NULL = 12,
    PKT_DATA_RES6 = 13,
    PKT_QOS_CF_POLL_NULL = 14,
    PKT_CF_ACK_CF_POLL_NULL = 15,
    PKT_DATA_ANY = 16
} data_pkt_subtype_t;

typedef union 
{
    mgmt_pkt_subtype_t mgmt_subtype;
    data_pkt_subtype_t data_subtype;
} pkt_subtype_t;

typedef struct 
{
    uint8_t version    : 2;
    uint8_t type       : 2;
    uint8_t sub_type   : 4;
    uint8_t : 0;

    uint8_t ds_status  : 2;
    uint8_t more_frags : 1;
    uint8_t retry      : 1;
    uint8_t pwr_mgt    : 1;
    uint8_t more_data  : 1;
    uint8_t protect    : 1;
    uint8_t ordered    : 1;
    uint8_t : 0;

    uint16_t duration;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    
    uint16_t fragment_num : 4;
    uint16_t sequence_num : 12;
} dot11_header_t;
