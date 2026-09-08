#!/usr/bin/env python3
import os,socket,subprocess,sys,tempfile,time
with tempfile.TemporaryDirectory(prefix='tio-mqtt-') as root:
    probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
    path=root+'/mosquitto.conf'
    with open(path,'w') as f:f.write(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
    broker=subprocess.Popen(['mosquitto','-c',path],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        until=time.monotonic()+5
        while True:
            assert broker.poll() is None,broker.stderr.read()
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):break
            except OSError:assert time.monotonic()<until;time.sleep(.02)
        subprocess.run([sys.argv[1],str(port)],check=True,timeout=12)
        print('PASS real Mosquitto subscription, binary QoS1 publish/ack, retain, unsubscribe and cancellation')
    finally:broker.terminate();broker.wait(timeout=5)
