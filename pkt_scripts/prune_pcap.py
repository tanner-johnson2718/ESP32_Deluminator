# To crack a wpa2 handshake we need the association req + response + 4 eapol
# packets. Take a cap file, find those 4 packets and export slimmed down pcap
# with just those

import sys
from scapy.all import EAPOL, wrpcap, rdpcap

input_file = sys.argv[1]
out_file = sys.argv[2]

pkts = rdpcap(input_file)
out_pkts = []

for p in pkts: 
    if EAPOL in p:
        out_pkts.append(p)
    if p.fields['type'] == 0 and p.fields['subtype'] == 0:
        out_pkts.append(p)
    if p.fields['type'] == 0 and p.fields['subtype'] == 1:    
        out_pkts.append(p)

for p in out_pkts:
    wrpcap(out_file, p, append=True)