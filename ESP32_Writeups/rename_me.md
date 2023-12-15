# ESP 32 Systems Overview

* start with `system/console/basic/example` to give us an interactive starting point
* Build, flash, and use USB UART as described [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md#esp-32-set-up)

# Flash Memory Layout

* `0x0000 - 0x0FFF` - All `0xFF`s
* `0x1000 - 0x7FFF` - Second Stage Boot loader
* `0x8000 - 0x8FFF` - Partition Table
* `0x9000 - 0xEFFF` - NVS Data
* `0xF000 - 0xFFFF` - Phy Init Data 
* `0x10000 - ?`     - application image and other partitions

# 