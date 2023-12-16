# ESP 32 Systems Overview

* Start with `system/console/basic/example` to give us an interactive starting point
* Build, flash, and use USB UART as described [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md#esp-32-set-up)
* This document will describe some esp32 systems concepts in general but in particular will describe at a high level the esp32 project located at [esp32_build](../esp32_build/)

# Flash Memory

## Layout

| Addr Range | Desc | Comment |
| --- | --- | --- |
| `0x0000 - 0x0FFF` | All `0xFF`s | ? |
| `0x1000 - 0x8FFF` | Second Stage Boot loader | image at `build/bootloader/bootloader.bin` |
| `0x9000 - 0xAFFF` | Partition Table | image at `build/partition_table/partition-table.bin` |
| `0xA000 - 0xAFFF` | Phy Init Data | - |
| `0xB000 - 0x1_FFFF` | NVS | See Below | 
| `0x2_0000 - 0x11_FFFF` | Application image | Can extend size as needed |
| `0x12_0000 - 0x400000` | SPIFFS parition | Takes up rest of flash memory |

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
    * Reads the Boot Loader header and loads 3 memory regions associated with the 2nd stage boot loader:
    * The passes control over to the loaded 2nd stage bootloader with entry at `0x4008_0688`

| Seg | Addr | Len | Target | Comment | 
| --- | --- | --- | --- | --- |
| .dram0.bss, .dram0.data, .dram0.rodata | `0x3FFF_0000` | `0x27FC` | Internal SRAM 1 | .dram0.bss is `0x30` in len and this seg is actually loaded at `0x3FFF_0030` |
| .iram_loader.text | `0x4007_8000` | `0x421B` | Internal SRAM 0 | - |
| .iram.text .iram.text | `0x4008_0400` | `0x10A1`| Internal SRAM 0 | - |

* Boot Stage 2
    * Maps First `0x1_0000` of flash to `0x3F40_0000`, mapping the partition table to `0x3F40_9000`
        * This makes sense why reading the virt addrs on the jtag only allowed us read the first `0x1_0000` bytes
    * Loads in the app image in accorance with the partition table and maps the following segments shown below
    * Remaps first `0x1_0000` bytes at addr `0x2_000` (the app) of flash to `0x3F40_0000`.
        * This maps over the part table mapping
        * Only maps `0x1_000` bytes to map the ro_data from the app image into the address space
    * Maps `0x4_0000` bytes starting at flash offset `0x3_0000` (this is `0x1_0000` bytes offset into the app.bin image) to vaddr `0x400D_0000`.
        * This mapping happens twice and both times we get a log message like this: `0x400d0020: _stext at ??:?`
        * Would need to look closer but my hypothesis is this is from the APP CPU, which in reset until app code starts, is getting pointed to this addr and this is where the messsage comes from
    * Finally it verifys the app image and uses the load addr contained in it to start the PRO CPU at address `0x4008_16DC` which is the function `call_start_cpu0`

| Seg | Addr | Len | Target | Comment |
| --- | --- | --- | --- | --- |
| .flash.appdesc .flash.rodata | `0x3f400020` | `0x0e710` | External Flash | - |
| .rtc.dummy .rtc.force_fast | `0x3ff80000` | `0x00068` | RTC Fast Memory | - |
| .dram0.data .noinit .dram0.bss | `0x3ffb0000` | `0x02c08` | Internal SRAM 2 | - |
| .iram0.vectors .iram0.text .iram0.text_end  | `0x40080000` | `0x0ecac` | Internal SRAM 0 | Appears to cache important systems functions that probably are called from interrupt contexts. Can cat and grep the symbol table looking for entiers in section ` 9 ` which is the iram.text section |
| .rtc.text | `0x400c0000` | `0x00063` | RTC Fast Memory | - |
| .flash.text | `0x400d0020` | `0x31757` | External Flash | - |
| .rtc_slow_reserved | `0x50001fe8` | `0x00018` | RTC Slow | - |

* Application start up
    * Application code starts `call_start_cpu0` which is located at `esp-idf/components/esp_system/port/cpu_start.c`


[Start Up Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/startup.html)

# Tasks / FreeRTOS

* [FreeRTOS API Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html)
* [Vanilla RTOS Ref](https://www.freertos.org/RTOS.html)


# Thangs TODO

* What is going on with like this copying of the entire esp-idf folder
* RAM - account for every byte
* Interrupts
* Get NVS usage
* Command for formatting storage