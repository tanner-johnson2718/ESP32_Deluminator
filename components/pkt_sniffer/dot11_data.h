#pragma once
#include  <stdint.h>

//*****************************************************************************
// Data Frame Header |
// -------------------
// Normal MGMT Header  but if toDS == fromDS == 1, then a 4 addr appears in the
// header. We explicitly reject these packets as they indicate an IBSS set up.
// The address interpetation is different for data packets, refer to 
// 802.11-2020 chapter 9 for more details.
//
// QoS Control -> if bit 4 of data subtype set
//
// HTC field -> present if the last bit of the Fram control field is 1. We
// explicitly dont allow this in the packet sniffer.
//
// CCM paramteres -> We assume that traffic that we capture is using AES as
// the cipher suite. Protected frames will have this header before the data.
//
//*****************************************************************************

typedef struct
{
    uint8_t tid:4;
    uint8_t eosp:1;
    uint8_t ack_policy:2;
    uint8_t amdsu_present:1;
    uint8_t : 0;

    uint8_t interpeted_byte;
} qos_ctrl_t;

typedef uint64_t ccmp_t;

// Technically 802.2 not dot11 should go in its own folder but oh wells.
// U - unnumbered
// I - information trasnfer
// S - supervisory
typedef struct
{
    uint8_t dsap;
    uint8_t ssap;
    uint8_t ctl;
    uint8_t data[];
} Ullc_header_t;

typedef struct
{
    uint8_t dsap;
    uint8_t ssap;
    uint16_t ctl;
    uint8_t data[];
} Sllc_header_t;

typedef Sllc_header_t Illc_header_t;

// If last 2 bits of first byte of ctl field are 11 then this is a snap header
typedef struct
{
    uint8_t dsap;
    uint8_t ssap;
    uint8_t ctl;
    uint8_t oui[3];
    uint16_t proto_id;
    uint8_t data[];
} snap_Ullc_header_t;

typedef struct
{
    uint8_t dsap;
    uint8_t ssap;
    uint16_t ctl;
    uint8_t oui[3];
    uint16_t proto_id;
    uint8_t data[];
} snap_Sllc_header_t;

typedef snap_Sllc_header_t snap_Illc_header_t;

#define LLC_SNAP 0xaa

//*****************************************************************************
// PKT_DATA_DATA           = b0000 (subtype)
//*****************************************************************************
typedef struct
{
    dot11_header_t header;
    ccmp_t ccmp;
    uint8_t data[];
} data_protected_t;

//*****************************************************************************
// PKT_NULL subtype = b0100 = 4- its sole purpose is to carry the power 
// management to let an AP know an STA's power management state. 
//*****************************************************************************
typedef dot11_header_t data_null_t;

//*****************************************************************************
// PKT_QOS_DATA            = b1000 (subtype)
//*****************************************************************************
typedef struct
{
    dot11_header_t header;
    qos_ctrl_t qos_ctrl;
    ccmp_t ccmp;
    uint8_t data[];
} qos_data_protected_t;

typedef struct
{
    dot11_header_t header;
    qos_ctrl_t qos_ctrl;
    uint8_t data[];
} qos_data_unprotected_t;

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