// Packet Sniffer. The base esp32 promicious wifi only allows for filtering on 
// packet type and some sub types and only gives a single stream of packets. 
// We add an extra layer of filtering of promiscious packets with this 
// component and the ability to multiplex the promiscious stream of pkts. We
// do more indepth packet identification in this module, and also do in place
// parsing. This gives down stream modules the ability subscrtibe to certain
// types and attributes found in a frame and have packets send to them pre
// indexed.
//
// Assumptions) We assume that this component has unfeddered and uninterrupted
//              access to the WHOLE wifi chip. No other tasks should be running
//              that use the wifi chip. Please kill this this component before
//              attempting to connect a client to the built in AP if the wifi
//              mode if STA/AP.
//
//              For now we only support Data and Management Type packets as 1
//              control packets clutter the inbound pkt queue and 2 most of the
//              interesting 802.11 behavor comes from data and mgmt packets.
//
// Resources)
//    * https://www.oreilly.com/library/view/80211-wireless-networks/0596100523/ch04.html
//    * https://en.wikipedia.org/wiki/802.11_frame_types
//
// TODO)
//    * Reassoc Req
//    * Auth
//    * Action Frame
//    * All data shit
//    * EAPOL?  

#pragma once
#include  <stdint.h>

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

//*****************************************************************************
// MGMT Frame Header |
//--------------------
//
// 802.11 frames are broken into 4 main types defined by bits 2-3 of the first
// byte the packet. One of which that is of particular interest is management
// frames. Their header looks like
//
// |--------------------------------------------------------------------------|
// | FCS | Duration | DA | SA | AA | Seq |       Fixed / Tagged Params        |
// |  2  |    2     | 6  | 6  | 6  |  2  |           0-2312                   |
// |--------------------------------------------------------------------------|
//
// FCS      - Frame control sequence contains meta data on the packets
//          - Byte 0 has type and subtype info
//          - Byte 1 has flags   
// Duration - Has to do with access to the wireless media
// DA       - Destination MAC addr
// SA       - Source Mac Addr
// AA       - AP Mac addr
// Seq      - Sequence number
//
// Fixed Params - These are fields of fixed length that appear in every packet
// of the same subtube. Thus parsing these is depedant on the subtype of the
// packet
//
// Tagged Params - Start immediatly after the fixed params and have a two byte
// header of the form id, length
//*****************************************************************************
typedef struct 
{
    uint16_t version    : 2;
    uint16_t type       : 2;
    uint16_t sub_type   : 4;
    uint16_t ds_status  : 2;
    uint16_t more_frags : 1;
    uint16_t retry      : 1;
    uint16_t pwr_mgt    : 1;
    uint16_t more_data  : 1;
    uint16_t protect    : 1;
    uint16_t ordered    : 1;
    uint16_t duration;
    uint8_t dest_mac[6];
    uint8_t src_mac[6];
    uint8_t ap_mac[6];
    uint16_t sequence;
} mgmt_header_t;

typedef struct
{
    mgmt_header_t mgmt_header;
    uint8_t payload[];
} mgmt_generic_t;

//*****************************************************************************
// Fixed Parameter BitMaps and TypeDefs
//*****************************************************************************
typedef struct
{
    uint16_t ess_capabilities : 1;
    uint16_t ibss_status      : 1;
    uint16_t cfp              : 2;
    uint16_t privacy          : 1;
    uint16_t short_preable    : 1;
    uint16_t pbcc             : 1;
    uint16_t channel_agility  : 1;
    uint16_t reserved0        : 2;
    uint16_t short_slot_time  : 1;
    uint16_t reserved1        : 2;
    uint16_t dsss_odfm        : 1;
    uint16_t reserved2        : 2;
} fixed_param_capability_t;

typedef uint16_t   fixed_param_listen_interval_t;
typedef uint64_t   fixed_param_timestamp_t;
typedef uint16_t   fixed_param_beacon_interval_t;
typedef uint16_t   fixed_param_aid_t;
typedef uint16_t   fixed_param_status_code_t;
typedef uint16_t   fixed_param_reason_code_t;
typedef uint16_t   fixed_param_auth_algo_t;
typedef uint16_t   fixed_param_auth_trans_seq_t;
typedef struct {uint8_t mac[6];} fixed_param_current_mac_t;

//*****************************************************************************
// Tagged Param ID defs
//*****************************************************************************
#define TAGGED_PARAM_SSID 0
#define TAGGED_PARAM_SUPPORTED_RATES 1
#define TAGGED_PARAM_FH_PARAM 2
#define TAGGED_PARAM_DS_PARAM 3
#define TAGGED_PARAM_CF_PARAM 4
#define TAGGED_PARAM_IBSS 6
#define TAGGED_PARAM_COUNTRY                7
#define TAGGED_PARAM_HOPPING_PATTERN_PARAMS 8
#define TAGGED_PARAM_HOPPING_PATTERN_TABLE  9
#define TAGGED_PARAM_POWER_CONSTANT        32
#define TAGGED_PARAM_POWER_CAPABILITIES    33
#define TAGGED_PARAM_TPC_REQ               34
#define TAGGED_PARAM_TPC_REPORT            35
#define TAGGED_PARAM_SUPPORTED_CHANNNELS   36
#define TAGGED_PARAM_CHANNEL_SWITCH        37
#define TAGGED_PARAM_QUIET                 40
#define TAGGED_PARAM_IBSS_DFS              41
#define TAGGED_PARAM_ERP                   42
#define TAGGED_PARAM_HT_CAPABILITES        45
#define TAGGED_PARAM_RSN 48
#define TAGGED_PARAM_EXTENDED_SUPPORTED_RATES 50
#define TAGGED_PARAM_EXTENDED_CAPABILITES 127

//*****************************************************************************
// Assoc Request - This is part of the association process for an STA  
// connecting to an AP. It is a mgmt frame of subtype 0 and It has the 
// follwing fixed fields.
//
// |------------------------------|
// | Capability | Listen Interval |
// |      2     |       2         |
// |------------------------------|
//
// Some tagged parameteres that usually apart of a assoc request are)
//   - SSID            id=0
//   - Supported Rates id=1
//   - RSN             id=48
//*****************************************************************************
typedef struct
{
    mgmt_header_t                     mgmt_header;
    fixed_param_capability_t         capabilities;
    fixed_param_listen_interval_t listen_interval;
    uint8_t                       tagged_params[];
} assoc_req_t;

//*****************************************************************************
// Assoc Response - This is a mgmt frame that is sent as a response to a assoc
// request. It has the following fixed params and has a subtybe of 1.
//
// Reassoc Response - Same for an reassoc req.
//
// |-----------------------------------|
// | Cability Info | Status Code | AID |
// |       2       |        2    |  2  |
// |-----------------------------------|
//
// Some tagged params that are usually apart of an assoc response)
//    - Suported Rates id=1
//*****************************************************************************
typedef struct
{
    mgmt_header_t              mgmt_header;
    fixed_param_capability_t  capabilities;
    fixed_param_status_code_t  status_code;
    fixed_param_aid_t                  aid;
    uint8_t                tagged_params[];
} assoc_res_t;

typedef assoc_res_t reassoc_re_t;

//*****************************************************************************
// Probe Request - Sent by a STA to an AP requesting a probe response which
// contains info on the AP. This is used when scanning available APs. This is
// a management frame of subtype 4. It has no fixed parameters but must
// contained the following tagged parameteres:
//    - SSID
//    - Supported Rates
//    - Extended Supported Rates
//*****************************************************************************
#define PKT_PROBE_REQ_SUBTYPE 4

typedef struct
{
    mgmt_header_t mgmt_header;
    uint8_t tagged_params[];
} probe_req_t;

//*****************************************************************************
// Probe Response - Sent by an AP and contains info about the AP. Its is of 
// subtype 5 and contains the following fixed fields)
//
// Beacons - These are sent at regular intervals by the AP to announce its
// presensce. They have same fixed parameters and are of subtype 8.
//
// |---------------------------------------------|
// | Time Stamp | Beacon Interval | Capabilities |
// |     8      |        2        |      2       |
// |---------------------------------------------|
//
// It also contains the following tagged params
//    - SSID
//    - Supported Rates
//    - FH Parameter set
//    - DS Param set
//    - CF param set
//    - IBSS param set
//    - Country info
//    - FH Hopping Param
//    - FH Pattern Table
//    - Power Constant
//    - Channel Switch Announcment
//    - Quiet
//    - IBSS DFS
//    - TPC Report
//    - ERP Info
//    - Exteneded support rates
//    - RSN
//*****************************************************************************
typedef struct
{
    mgmt_header_t                     mgmt_header;
    fixed_param_timestamp_t             timestamp;
    fixed_param_beacon_interval_t beacon_interval;
    fixed_param_capability_t         capabilities;
    uint8_t                       tagged_params[];
} probe_res_t;

typedef probe_res_t beacon_t;

//*****************************************************************************
// ATIM frame - This is used in an IBSS network where traffic is sent p2p and
// not through an AP. This packet notifies the recipent that the sender has
// buffered data for it. It is just mgmt header with no body.
//*****************************************************************************
typedef mgmt_header_t atim_t;

//*****************************************************************************
// Deauth and Disassoc - these frames are send to end either an association or
// an authentication between and AP and an STA. It contains a single fixed 
// param given the reason code.
//*****************************************************************************
typedef struct disassoc
{
    mgmt_header_t mgmt_header;
    fixed_param_reason_code_t reason_code;
} disassoc_t;

typedef disassoc_t deauth_t;

//*****************************************************************************
// Data PKTS - ??????????
//*****************************************************************************


//*****************************************************************************
// We define a structure that allows one to specifiy which type and sub type
// of packets should invoke the call back attached to the filter. The bitmaps
// contain a 1 in the i-th if the packet is of type or subtype i where i is 
// the integer index of the type or subtype in the enums above.
//
// In the pkt_sniffer_cb the pkt is just a uint8_t* buffer and the meta data is
// a esp_wifi.h struct of  wifi_pkt_rx_ctrl_t*. We keep these void* to keep
// this interface as portable as possible.
//*****************************************************************************
typedef struct
{
    uint8_t type_bitmap;
    uint16_t mgmt_subtype_bitmap;
    uint16_t data_subtype_bitmap;
}  pkt_filter_t;

typedef void (*pkt_sniffer_cb_t)(void* pkt, 
                                 void* meta_data, 
                                 pkt_type_t type, 
                                 pkt_subtype_t subtype);

typedef struct 
{
    pkt_filter_t filter;
    pkt_sniffer_cb_t cb;
} pkt_sniffer_filtered_src_t;

//*****************************************************************************
// Returns) 1 if running 0 else
//*****************************************************************************
uint8_t pkt_sniffer_is_running(void);


//*****************************************************************************
// pkt_sniffer_clear_filter_list) Clears all the filters, resets it back to 0
//
// Returns) ESP_OK, otherwise might timeout trying to grab the lock
//*****************************************************************************
esp_err_t pkt_sniffer_clear_filter_list(void);


//*****************************************************************************
// pkt_sniffer_add_filter) Fill in the filter src struct in accordance with the
//                         documentation provided above. Will add the filter to
//                         list of filters and if the sniffer is running, 
//                         matching packet will be sent your way via the passed
//                         call back.
//
// f) See above.
//
// Returns) ESP_OK if added, otherwise could be full or failed to grab lock
//*****************************************************************************
esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_src_t* f);


//****************************************************************************
// pkt_sniffer_add_type_subtype) Add the indicated type and subtype to the
//                               filter contained in the passed filtered src
//
// f - pointer to location that can fit a pkt_sniffer_filtered_src_t
// type - enum above. Checked if when viewed as a uint8_t is below 16
// subtype - enum above. Checked if when viewed as a uint8_t is below 16
//
// Returns) ESP_OK if all good, else invalid arg
//****************************************************************************
esp_err_t pkt_sniffer_add_type_subtype(pkt_sniffer_filtered_src_t* f, 
                                       pkt_type_t type, 
                                       pkt_subtype_t subtype);


//*****************************************************************************
// pkt_sniffer_launch) Given a specific channel and start the pkt_sniffer
//
// channel) Must be between 1 and 11
//
// type_filter) refer to esp_wifi documentation for details
//
// Returns) ESP OK If launched otherwise could have invalid arg or its already
//          running. Also could be failures to set the wifi chip to the
//          appropirate config.
//*****************************************************************************
esp_err_t pkt_sniffer_launch(uint8_t channel);


//*****************************************************************************
// pkt_sniffer_kill) Kills the already running sniffer.
//
// Return) ESP OK if killed. 
//*****************************************************************************
esp_err_t pkt_sniffer_kill(void);