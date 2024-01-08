# ESP 32 Deluminator

Welcome to the ESP 32 Deluminator project. The name is a reference to Dumbledor's light snatching deluminator from the harry potter series. We have 3 very specific goals in mind as it relates to this project.

* (1) Create an esp32 device that can carry out various wifi based attacks on our home network. Specifically one that specializes in collecting encrypted WPA2 PSKs.
* (2) Explore the wifi 802.11 protocol and internal workings of this medium
* (3) Explore the HW and programming model of the ESP32

![alt text](./Docs/delum.jpg)

# Disclaimer

We do not support or condone the use of any attacks on non consenting parties. Please only do this on your own hardware or on systems that you have been given rights to attack.

# HW / SW Requiements

* Set up the esp-idf build env as we describe [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md#esp-32-set-up)
    * This page also describes how to set up JTAG debugging with a Raspberry Pi 4 model B. This is recommended if one does any ESP 32 development as it gives one intruction level stepping with a 45 dollar Raspberri PI.
* [ESP32 Wroom Dev Kit](https://www.amazon.com/gp/product/B08246MCL5/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&psc=1)
* [LCD 2004 w/ i2c backpack](https://www.amazon.com/dp/B0C1G9GBRZ?psc=1&ref=ppx_yo2ov_dt_b_product_details)
* [Battery Pack](https://www.walmart.com/ip/onn-Portable-Battery-4k-mAh-Black/934734622?wmlspartner=wlpa&selectedSellerId=0&wl13=2070&adid=22222222277934734622_117755028669_12420145346&wmlspartner=wmtlabs&wl0=&wl1=g&wl2=c&wl3=501107745824&wl4=aud-2230653093054:pla-306310554666&wl5=9033835&wl6=&wl7=&wl8=&wl9=pla&wl10=8175035&wl11=local&wl12=934734622&wl13=2070&veh=sem_LIA&gclsrc=aw.ds&&adid=22222222237934734622_117755028669_12420145346&wl0=&wl1=g&wl2=c&wl3=501107745824&wl4=aud-2230653093054:pla-306310554666&wl5=9033835&wl6=&wl7=&wl8=&wl9=pla&wl10=8175035&wl11=local&wl12=934734622&veh=sem&gad_source=1&gclid=CjwKCAiA-bmsBhAGEiwAoaQNmpeMOc645RI29sXwDRy94ucsxWZd484QlGaFLX9-s_fhE79IKZzTjxoCHxQQAvD_BwE) - We have a custom case for this pack but obviously any will do
* Rotary encoder - We used a random one we found but should be interoperable with any

# Software and the ESP32 System

```    
|-------------------|                   |--------------------|
|         UI        |                   | EAPOL | MAC Logger | 
|-------------------|    |-----------|  |--------------------|  |------------|
| Rotary Enc. | LCD |    | File Serv |  |      PKT Sniff     |  | WSL ByPass |
|-------------------|    |-----------|  |--------------------|  |------------|

|---------------------|  |----------------------------------------------------|
|        Main         |  |                         Wifi                       |
|---------------------|  |----------------------------------------------------|
| NVS | SPIFFS | REPL |  |            STA            |            AP          |
|---------------------|  |----------------------------------------------------|
```

## Coding Standards

* (1) All component API functions **shall** return `esp_err_t`
    * Exceptions made for trivial situations like `is_running(void)` function
* (2) All returns from component API functions **shall** are handled via `ESP_ERROR_CHECK` or `ESP_ERROR_CHECK_WITHOUT_ABORT`
    * Again minus trivial exceptions, see note above
* (3) All components *should* have both implementation and theory doc in the header
* (4) All component API functions *should* have a summary describing their execution logic
* (5) All component API functions **shall** have a description of possible return args
* (6) All component API functions **shall** a description of every input
    * While checking this its good to verify that the API function has appriopiatly strict check on input args.
* (7) All components *should* export a REPL test interface
* (8) All components **shall** not use printf outside of REPL test commands
* (9) All component API functions **shall** start with the name of the component
    * Abbreviations are allowed i.e. `ui_init` for the user interface components
* (10) All components *should* use Kconfig params to set defines within their module

| Component       | (1) | (2) | (3) | (4) | (5) | (6) | (7) | (8) | (9) | (10)|
| --------------- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| encoder         |  X  |  X  |  X  |  X  |  X  |  X  | N/A |  X  |  X  |  X  |
| HD44780         |  X  |  X  |  X  |  X  |  X  |  X  | N/A |  X  |  X  |  X  |
| pkt_sniffer     |  X  |  X  |     |  X  |  X  |  X  |  X  |  X  |  X  |  X  |
| tcp_file_server |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |   
| user_interface  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |  X  |
| wsl_bypasser    |  X  |  X  |     |  X  |  X  |  X  | N/A |  X  |  X  |  X  | 

# Wiring and 3D Printed Case

| ESP Pin | LCD / ROT Pin | Func |
| --- | --- | --- |
| 26 | SCL | SCL |
| 25 | SDA | SDA |
| Vin | LCD+ | Vin |
| GND | LCD- | GND |
| 33 | Rot Switch | Button Pin |
| GND | Rot Switch | Button Pin GND |
| 32 | Rot A Term | - |
| 27 | Rot B Term | - |
| GND | Rot Middle term | - |