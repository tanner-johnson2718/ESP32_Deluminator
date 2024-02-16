#pragma once

#include <stdint.h>

#define PCAP_MAGIC 0xa1b2c3d4
#define DOT11_LINK_TYPE 105

typedef struct 
{
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;    /* gmt to local correction */
    uint32_t sigfigs;    /* accuracy of timestamps */
    uint32_t snaplen;    /* max length saved portion of each pkt */
    uint32_t linktype;   /* data link type (LINKTYPE_*) */
} pcap_file_header_t;

typedef struct  
{
    uint64_t ts;      /* time stamp */
    uint32_t caplen;     /* length of portion present */
    uint32_t len;        /* length this packet (off wire) */
} pcap_pkthdr_t;