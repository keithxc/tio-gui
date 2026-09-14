#!/usr/bin/env python3
"""Native transport and bundled lrzsz, all protocols/directions, exact binary PTY peer."""
import os, pty, select, socket, subprocess, sys, tempfile, threading, time, tty
from pathlib import Path
helper = str(Path(sys.argv[1]).resolve())
for protocol, option in enumerate(['--xmodem', '--ymodem', '--zmodem']):
    for receive in (False, True):
        with tempfile.TemporaryDirectory(prefix='tio-native-transfer-') as root:
            root = Path(root); source = root/'firmware.bin'; target = root/'received'; target.mkdir()
            payload = bytes(range(256))*32; source.write_bytes(payload)
            master, slave = pty.openpty(); tty.setraw(slave)
            link, child = socket.socketpair(); stop = threading.Event(); errors = []
            def relay():
                try:
                    while not stop.is_set():
                        for fd in select.select([master,link],[],[],.05)[0]:
                            block = os.read(master,65536) if fd == master else link.recv(65536)
                            if not block: return
                            if fd == master: link.sendall(block)
                            else:
                                while block: block = block[os.write(master,block):]
                except (BrokenPipeError, ConnectionResetError): pass
                except Exception as e: errors.append(e)
            thread = threading.Thread(target=relay); thread.start(); app=peer=None
            try:
                app=subprocess.Popen([helper,os.ttyname(slave),str(target if receive else source),str(protocol),str(int(receive))],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                assert app.stdout.readline() == b"READY\n"
                argv=['sz' if receive else 'rz',option,'--binary','--restricted','--protect']
                if receive: argv += ['--',source.name]
                elif protocol == 0: argv += ['--with-crc','--','firmware.bin']
                peer=subprocess.Popen(argv,stdin=child,stdout=child,stderr=subprocess.PIPE,cwd=root if receive else target);child.close()
                output=app.communicate(timeout=18)[0]; print(protocol,receive,app.returncode,output,flush=True); peerout=peer.communicate(timeout=3)[1]
                assert app.returncode == peer.returncode == 0,(protocol,receive,output,peerout)
                assert not errors,errors
                files=list(target.iterdir());assert len(files)==1,files
                assert files[0].read_bytes()==payload,(protocol,receive,files)
                print('PASS',option,'receive' if receive else 'send',len(payload),'exact bytes',flush=True)
            finally:
                for proc in (app,peer):
                    if proc and proc.poll() is None: proc.kill();proc.wait()
                stop.set();thread.join(2);link.close();child.close();os.close(master);os.close(slave)
