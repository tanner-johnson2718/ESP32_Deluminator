from math import log10
from scapy.all import wrpcap
from scapy.layers.dot11 import Dot11
import socket
import sys
import select
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("192.168.4.1", 420))

print("Connected to File Trasnfer Server:")
print(("192.168.4.1", 420))
print()

files = []

while 1:
    ready = select.select([sock], [], [], 3)
    if ready[0]:
        data = sock.recv(33)

        if b'.pkt' in data:
            files.append(data)
    else:
        break


print("Packet Dumps Found from server: ")
print(files)
print()

for bstr in files:
    _bstr = bstr.decode('ascii').strip('\n').encode('ascii')
    data = b''
    
    print("Requesing: " + str(_bstr))
    sock.send(_bstr)

    l = 256
    while 1:
        _data = sock.recv(256)
        data += _data
        l = len(_data)
        print("Recieved " + str(len(_data)) + " bytes")
        if l < 256:
            break
    
    l0 = int(data[0]) + (int(data[1]) << 8)
    l1 = int(data[2]) + (int(data[3]) << 8)
    l2 = int(data[4]) + (int(data[5]) << 8)
    l3 = int(data[6]) + (int(data[7]) << 8)
    l4 = int(data[8]) + (int(data[9]) << 8)
    l5 = int(data[10]) + (int(data[11]) << 8)

    s0 = 12
    s1 = s0 + l0
    s2 = s1 + l1
    s3 = s2 + l2
    s4 = s3 + l3
    s5 = s4 + l4

    p0 = data[s0:(s0+l0)]
    p1 = data[s1:(s1+l1)]
    p2 = data[s2:(s2+l2)]
    p3 = data[s3:(s3+l3)]
    p4 = data[s4:(s4+l4)]
    p5 = data[s5:(s5+l5)]

    print("Writing to test.pcap")
    wrpcap('test.pcap', Dot11(p0), append=True)
    wrpcap('test.pcap', Dot11(p1), append=True)
    wrpcap('test.pcap', Dot11(p2), append=True)
    wrpcap('test.pcap', Dot11(p3), append=True)
    wrpcap('test.pcap', Dot11(p4), append=True)
    wrpcap('test.pcap', Dot11(p5), append=True)

    while 1:
        ready = select.select([sock], [], [], 3)
        if ready[0]:
            data = sock.recv(33)
        else:
            break