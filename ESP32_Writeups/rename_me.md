# ESP 32 Systems Overview

* start with `system/console/basic/example` to give us an interactive starting point
* Build, flash, and use USB UART as described [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md#esp-32-set-up)

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
| `0x110000 - ?` | Further paritions | Can put other images or filesystems here |

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

## File systems on Flash Memory

## Ref

* [Partition Table API Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html)
* `esptool.py read_flash` subcommand
* [JTAG Flash reading](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Reverse_Engineer_Example.md)
* [NVS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html)
* 
# RAM

# Tasks