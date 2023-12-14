# ESP 32 Deluminator

The goals of this project are 3 fold:

* 1) Explore the wifi 802.11 protocol and internal workings of this medium
* 2) Explore the HW and programming model of the ESP32
* 3) Create an esp32 device that can carry out various wifi based attacks on our home network

The physical hardware set up will be the same of that as in the previous project and details can be found [here](https://github.com/tanner-johnson2718/PI_JTAG_DBGR/blob/master/writeups/Init_PI_JTAG_Test.md).

The workflow for this project will be the following. First take some well known or at least documented wifi attack. Execute that attack (on property you have the rights to do so on) using known tools, scripts, really using anything to just execute the attack in whatever is the fastest way possible. Afterwords, use the steps of the attack as targeted research questions to learn about Wifi protocols, about how to negate this attack, how to automate or improve this attack etc. More importantly if this attack was not first executed on a esp32, implement it as bare metal as possible on the esp32 and in turn use that task as means to learn about the esp32 system architecture.