//*****************************************************************************
// Packet Parsing Helpers
//*****************************************************************************

// assume type is known to be data
static inline int8_t get_eapol_index(uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    uint16_t len = rx_ctrl->sig_len;

    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        // eapol
        int16_t s = ((dot11_header_t*) p)->sequence_num;
        uint8_t ds = ((dot11_header_t*) p)->ds_status;

        if     (s == 0 && ds == 2) { return 0; }
        else if(s == 0 && ds == 1) { return 1; }
        else if(s == 1 && ds == 2) { return 2; }
        else if(s == 1 && ds == 1) { return 3; }
    }

    return -1;
}

//*****************************************************************************
// Handling Eapol PKTS
//*****************************************************************************

static void eapol_dump_to_disk(uint8_t ap_index)
{
    char path[MAX_SSID_LEN];

    snprintf(path, MAX_SSID_LEN, "%s/%.19s.pkt", MOUNT_PATH, ap_list[ap_index].ssid);
    
    ESP_LOGI(TAG, "Opening %s to writeout eapol pkts", path);
    FILE* f = fopen(path, "w");
    if(!f)
    {
        ESP_LOGE(TAG, "Failed to open %s - %s", path, strerror(errno));
        fclose(f);
        return;
    }

    size_t num_written = fwrite(ap_list[ap_index].eapol_pkt_lens, 1, 2*EAPOL_NUM_PKTS, f);
    if(num_written != 2 * EAPOL_NUM_PKTS)
    {
        ESP_LOGE(TAG, "Failed to write EAPOL Header (%d / %d)", num_written, 2*EAPOL_NUM_PKTS);
        fclose(f);
        return;
    }

    uint8_t i;
    for(i = 0; i < EAPOL_NUM_PKTS; ++i)
    {
        num_written = fwrite(ap_list[ap_index].eapol_buffer + i*EAPOL_MAX_PKT_LEN, 1, ap_list[ap_index].eapol_pkt_lens[i], f);
        if(num_written != ap_list[ap_index].eapol_pkt_lens[i])
        {
            ESP_LOGE(TAG, "Failed to write EAPOL %d Pkt (%d / %d)", i, num_written, ap_list[ap_index].eapol_pkt_lens[i]);
            fclose(f);
            return;
        }
    }
    
    fclose(f);
    ESP_LOGI(TAG, "Write out of EAPOL pkts successful!");
}

void parse_eapol_pkt(uint8_t eapol_index, uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    if(rx_ctrl->sig_len >= EAPOL_MAX_PKT_LEN)
    {
        ESP_LOGE(TAG, "Recved EAPOL pkt with len greater than %d", EAPOL_MAX_PKT_LEN);
        return;
    }

    if(_take_lock()){ return; }

    int8_t ap_index;
    find_ap(p + 16, &ap_index);
    if(ap_index < 0)
    {
        ESP_LOGE(TAG, "Recieved EAPOL pkt for unregistered AP??");
        _release_lock();
        return;
    }

    if(ap_list[ap_index].eapol_pkt_lens[eapol_index] != 0)
    {
        ESP_LOGE(TAG, "Possibly recved duplicate eapol pkt: %d", eapol_index);
    }

    memcpy(ap_list[ap_index].eapol_buffer + (eapol_index*EAPOL_MAX_PKT_LEN), p, rx_ctrl->sig_len);
    ap_list[ap_index].eapol_pkt_lens[eapol_index] = rx_ctrl->sig_len;
    ESP_LOGI(TAG, "%s -> Eapol Captured (%d/6)", ap_list[ap_index].ssid, eapol_index);

    uint8_t i;
    for(i = 0; i < EAPOL_NUM_PKTS; ++i)
    {
        if(ap_list[ap_index].eapol_pkt_lens[i] == 0)
        {
            _release_lock();
            return;
        }
    }

    eapol_dump_to_disk(ap_index);

    _release_lock();
    return;
}