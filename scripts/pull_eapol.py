from math import log10
from scapy.all import wrpcap
from scapy.layers.dot11 import Dot11
import socket
import sys

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("192.168.4.1", 420))

print("Connected to File Trasnfer Server:")
print(("192.168.4.1", 420))
print()

files = []
indexs = []
data = sock.recv(1)
n_files = data[0]
print("n = " + str(n_files))
sock.send(bytes([n_files]))
for i in range(0, n_files):
    data = sock.recv(33)
    if len(data) != 33:
        print("error")
        continue
    index = data[0]
    path = data[1:]

    if b'.pkt' in path:
        files.append(path)
        indexs.append(index)


print("Packet Dumps Found from server: ")
for i in range(0, len(files)):
    print(str(i) + " : " + files[i].decode('utf-8'))
print()
inp = input("Index of File to pull: ")
index = int(inp)

bstr = files[index]
_bstr = bstr
# _bstr = bstr.decode('ascii').strip('\n').encode('ascii')
data = b''
    
print("Requesing: " + str(_bstr))
sock.send(bytes([indexs[index]]))
print("Server Sending " + str(sock.recv(33)[1:]))

l = 256
while 1:
    _data = sock.recv(256)
    if(len(_data) == 0):
        break
    data += _data
    l = len(_data)
    print("Recieved " + str(len(_data)) + " bytes")


print("Writing to test.pcap")
f = open("test.pcap", "wb")
f.write(data)
f.close()