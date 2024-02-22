#pragma once

#include "dot11.h"
#include "esp_err.h"

esp_err_t data_pkt_dumper_init(data_pkt_subtype_t s,  char* file_name);
esp_err_t data_pkt_dumper_fini(void);