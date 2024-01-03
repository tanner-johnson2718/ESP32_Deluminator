# LCD Component

* Taken from [here](https://github.com/maxsydney/ESP32-HD44780)
* Only modification is I made the I2C speed configurable and exported pin assignments to the Kconfig, removing them from the init function
* **NOTE** Not reentrant nor thread safe. All access in a concurrent environment needs to be guarded by CVs and Mutexes
* Also removed referecences to `portTICK_RATE_MS` that has been depreciated and replaced with `portTICK_PERIOD_MS`