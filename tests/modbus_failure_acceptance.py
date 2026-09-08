#!/usr/bin/env python3
import os,socket,subprocess,sys,threading,time
for failure in ('transaction','exception','timeout'):
    server=socket.socket();server.bind(('127.0.0.1',0));server.listen();server.settimeout(5)
    def peer():
        conn,_=server.accept()
        with conn:
            request=b''
            while len(request)<12:request+=conn.recv(12-len(request))
            if failure=='transaction':conn.sendall(bytes.fromhex('00 08 00 00 00 07 01 03 04 12 34 AB CD'))
            elif failure=='exception':conn.sendall(bytes.fromhex('00 07 00 00 00 03 01 83 02'))
            else:time.sleep(3)
    thread=threading.Thread(target=peer);thread.start()
    try:
        subprocess.run([sys.argv[1],'-p','/modbus/roundtrip'],env={**os.environ,'TIO_TEST_MODBUS_ENDPOINT':'127.0.0.1','TIO_TEST_MODBUS_PORT':str(server.getsockname()[1]),'TIO_TEST_EXPECT_ERROR':'1'},check=True,timeout=6)
        print('PASS Modbus rejects',failure)
    finally:thread.join();server.close()
