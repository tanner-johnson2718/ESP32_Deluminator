#pragma once

void init_wifi(void);
int do_repl_scan_ap(int argc, char** argv);
int do_repl_scan_mac_start(int argc, char** argv);
int do_repl_scan_mac_stop(int argc, char** argv);
int do_deauth(int argc, char** argv);