#!/usr/bin/env python3
import socket, subprocess, sys, threading
for udp in (False, True):
    server=socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)
    server.bind(('127.0.0.1',0));server.settimeout(5)
    if not udp:server.listen()
    errors=[]
    def echo():
        try:
            if udp:
                data,peer=server.recvfrom(65536)
                server.sendto(b'',peer);server.sendto(data,peer)
            else:
                peer,_=server.accept()
                with peer:
                    data=b''
                    while len(data)<256:data+=peer.recv(256-len(data))
                    peer.sendall(data[:13]);peer.sendall(data[13:])
        except Exception as error:errors.append(error)
    thread=threading.Thread(target=echo);thread.start()
    try:
        subprocess.run([sys.argv[1],str(server.getsockname()[1]),str(int(udp))],check=True,timeout=8)
        thread.join();assert not errors, errors
        print('PASS', 'UDP datagram boundaries/empty packet' if udp else 'TCP segmented stream', 'binary roundtrip and cancellation')
    finally:server.close();thread.join()
