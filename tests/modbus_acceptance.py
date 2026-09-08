#!/usr/bin/env python3
import os, socket, subprocess, sys, tempfile, threading, pty, tty, select, time
from pathlib import Path

def exact(fd,count):
    data=b'';until=time.monotonic()+5
    while len(data)<count:
        assert time.monotonic()<until
        if select.select([fd],[],[],.1)[0]:
            chunk=os.read(fd,count-len(data));assert chunk;data+=chunk
    return data

def crc(data):
    value=0xffff
    for byte in data:
        value^=byte
        for _ in range(8):value=(value>>1)^(0xa001 if value&1 else 0)
    return value.to_bytes(2,'little')

for tcp in (True,False):
    with tempfile.TemporaryDirectory(prefix='tio-modbus-') as root:
        tio=None;fds=[];server=None;errors=[]
        env={**os.environ,'XDG_CONFIG_HOME':root}
        if tcp:
            server=socket.socket();server.bind(('127.0.0.1',0));server.listen();server.settimeout(5)
            env.update(TIO_TEST_MODBUS_ENDPOINT='127.0.0.1',TIO_TEST_MODBUS_PORT=str(server.getsockname()[1]))
        else:
            serial,slave=pty.openpty();control,terminal=pty.openpty();fds=[serial,slave,control,terminal];tty.setraw(slave)
            path=root+'/tio.sock';env['TIO_TEST_MODBUS_ENDPOINT']=path;env.pop('TIO_TEST_MODBUS_PORT',None)
            tio=subprocess.Popen(['tio','--no-reconnect','--socket','unix:'+path,os.ttyname(slave)],stdin=terminal,stdout=terminal,stderr=terminal,env=env)
            output=b'';until=time.monotonic()+5
            while b'Connected' not in output:
                assert time.monotonic()<until
                if select.select([control],[],[],.1)[0]:output+=os.read(control,8192)
        def peer():
            try:
                if tcp:
                    conn,_=server.accept()
                    with conn:
                        request=exact(conn.fileno(),12)
                        assert request[6:]==bytes.fromhex('01 03 00 00 00 02'),request
                        response=request[:2]+bytes.fromhex('00 00 00 07 01 03 04 12 34 AB CD')
                        conn.sendall(response[:4]);conn.sendall(response[4:])
                else:
                    request=exact(serial,8);assert request==bytes.fromhex('01 03 00 00 00 02 C4 0B'),request
                    response=bytes.fromhex('01 03 04 12 34 AB CD');response+=crc(response)
                    os.write(serial,response[:3]);time.sleep(.02);os.write(serial,response[3:])
            except Exception as error:errors.append(error)
        thread=threading.Thread(target=peer);thread.start()
        try:
            subprocess.run([sys.argv[1]],env=env,check=True,timeout=8);thread.join();assert not errors,errors
            print('PASS Modbus','TCP' if tcp else 'RTU through real tio','fragmented response and register decoding')
        finally:
            if tio:tio.kill();tio.wait()
            if server:server.close()
            for fd in fds:os.close(fd)
            thread.join()
