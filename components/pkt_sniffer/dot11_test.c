#include "dot11.h"
#include <stdio.h>

int main()
{
    mgmt_header_t hdr = {0};

    hdr.type = 1;
    hdr.sub_type = 0xf;

    uint8_t* buff = &hdr;

    printf("%x\n", buff[0]);
    printf("Size = %d\n", sizeof(mgmt_header_t));
}