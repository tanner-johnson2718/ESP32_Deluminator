# ESP 32 Systems Overview

* Start with `system/console/basic/example` to give us an interactive starting point
* Build, flash, and use USB UART as described [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md#esp-32-set-up)
* This document will describe some esp32 systems concepts in general but in particular will describe at a high level the esp32 project located at [esp32_build](../esp32_build/)

# Flash Memory

## Layout

| Addr Range | Desc | Comment |
| --- | --- | --- |
| `0x0000 - 0x0FFF` | All `0xFF`s | ? |
| `0x1000 - 0x7FFF` | Second Stage Boot loader | image at `build/bootloader/bootloader.bin` |
| `0x8000 - 0x8FFF` | Partition Table | image at `build/partition_table/partition-table.bin` |
| `0x9000 - 0xEFFF` | NVS Data | See below |
| `0xF000 - 0xFFFF` | Phy Init Data | - 
| `0x10000 - 0x110000` | Application image | Can extend size as needed |
| `0x110000 - 0x400000` | SPIFFS parition | Takes up rest of flash memory |

* [Partition Table API Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html)
* `esptool.py read_flash` subcommand
* [JTAG Flash reading](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Reverse_Engineer_Example.md)

## NVS

* Basically just allows one to store key-value pairs in a dedicated namespace
* Usage is as follows:

```C
#include "nvs_flash.h"
#include "nvs.h"

// MAKE SURE TO CHECK ERRORS, this is for brevity
nvs_flash_init();
nvs_open("ns", NVS_READWRITE, &my_handle);
nvs_set_i32(my_handle, "key", val);
nvs_get_i32(my_handle, "key", &val);
```

* [NVS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html)

## File systems on Flash Memory

* Tried using FAT FS but it got corrupted rather quickly
* Using SPIFFS which does not allow dir

```C
#include "esp_spiffs.h"

// Easy mounting helper to mount fs
esp_vfs_spiffs_register(&config);
esp_spiffs_check(config.partition_label);
esp_spiffs_info(conf.partition_label, &total, &used);
esp_vfs_spiffs_unregister(conf.partition_label);

// Now the vfs gives POSIX complient like read, write, open, close and delete
FILE* f = fopen("/spiffs/hello.txt", "w");
fgets(line, sizeof(line), f); fread(...);
fwrite(...);
remove(path);
fclose(f);
```

* [SPIFFS API ref](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/spiffs.html)

# Early Start Up

* Bootstage 1
    * Reset vector code located in ROM and is unchangable
    * Based on type of reset certain registers dictate boot flow
    * In the usual case the 2nd stage boot loader is called from addr `0x1000`
    * Reads the Boot Loader header and loads 3 memory regions:

| Seg | Addr | Len | Target | Comment | 
| --- | --- | --- | --- |
| .dram0.bss, .dram0.data, .dram0.rodata | 0x3FFF_0000 | 0x27FC | Internal SRAM 1 | .dram0.bss is 0x30 in len and this seg is actually loaded at `0x3FFF_0030` |


* Boot Stage 2
    * 

* Who or what is creating these tasks
* [Start Up Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/startup.html)

# Tasks / FreeRTOS

* [FreeRTOS API Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html)
* [Vanilla RTOS Ref](https://www.freertos.org/RTOS.html)


# Thangs TODO

* What is going on with like this copying of the entire esp-idf folder
* RAM - account for every byte
* Interrupts
* Get NVS usage
* Command for formatting storage