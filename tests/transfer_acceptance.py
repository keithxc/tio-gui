#!/usr/bin/env python3
"""All three protocols, both directions: application engine -> tio -> PTY peer."""
import os, pty, select, socket, subprocess, sys, tempfile, threading, time, tty
from pathlib import Path
helper = str(Path(sys.argv[1]).resolve())
for protocol, option in enumerate(['--xmodem', '--ymodem', '--zmodem']):
    for receive in (False, True):
        with tempfile.TemporaryDirectory(prefix='tio-transfer-') as root:
            root = Path(root)
            source = root / 'firmware.bin'
            payload = bytes(range(256)) * 32
            source.write_bytes(payload)
            target = root / 'received'; target.mkdir()
            serial, slave = pty.openpty(); tty.setraw(slave)
            control, terminal = pty.openpty()
            sock = str(root / 'serial.sock')
            tio = subprocess.Popen(['tio', '--no-reconnect', '--socket', 'unix:'+sock, os.ttyname(slave)],
                stdin=terminal, stdout=terminal, stderr=terminal, env={**os.environ,'XDG_CONFIG_HOME':str(root)})
            peer = app = None
            peer_link, peer_child = socket.socketpair()
            relay_stop = threading.Event()
            relay_errors = []
            # lrzsz flushes terminal queues on exit. On a PTY, tcdrain() does not
            # wait for the master reader, so that flush can discard the final
            # ACK/OO. Give the reference peer a stream and relay to the real
            # tio PTY; do not let its terminal cleanup change the test link.
            def relay():
                try:
                    while not relay_stop.is_set():
                        ready, _, _ = select.select([serial, peer_link], [], [], .05)
                        for source_fd in ready:
                            block = os.read(serial, 65536) if source_fd == serial else peer_link.recv(65536)
                            if not block:
                                return
                            if source_fd == serial:
                                peer_link.sendall(block)
                            else:
                                while block:
                                    block = block[os.write(serial, block):]
                except (BrokenPipeError, ConnectionResetError):
                    pass  # The reference peer has completed and closed its stream.
                except Exception as error:
                    relay_errors.append(error)
            relay_thread = threading.Thread(target=relay)
            relay_thread.start()
            try:
                output=b''; until=time.monotonic()+5
                while b'Connected' not in output:
                    assert time.monotonic()<until, output
                    if select.select([control],[],[],.1)[0]: output+=os.read(control,8192)
                app = subprocess.Popen([helper,sock,str(target if receive else source),str(protocol),str(int(receive))], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                time.sleep(.2)
                command = ['sz' if receive else 'rz', option, '--binary', '--restricted', '--protect']
                if receive: command += ['--', str(source.name)]
                elif protocol == 0: command += ['--with-crc', '--', 'firmware.bin']
                peer = subprocess.Popen(command, stdin=peer_child,stdout=peer_child,stderr=subprocess.PIPE,cwd=root if receive else target)
                peer_child.close()
                until=time.monotonic()+18
                while app.poll() is None or peer.poll() is None:
                    assert time.monotonic()<until, 'transfer deadline'
                    if select.select([control],[],[],.05)[0]: os.read(control,65536)
                appout=app.communicate()[0]; peerout=peer.communicate()[1]
                assert app.returncode == 0 and peer.returncode == 0,(protocol,receive,appout,peerout)
                assert not relay_errors, relay_errors
                files=list(target.iterdir()); assert len(files)==1, files
                assert files[0].read_bytes()==payload,(protocol,receive,files)
                print('PASS',option,'receive' if receive else 'send',len(payload),'exact bytes')
            finally:
                for proc in (app,peer,tio):
                    if proc and proc.poll() is None: proc.kill();proc.wait()
                relay_stop.set(); relay_thread.join(timeout=2)
                assert not relay_thread.is_alive()
                peer_link.close(); peer_child.close()
                for fd in (serial,slave,control,terminal):os.close(fd)

# Nonempty destinations are refused before opening a transport.
with tempfile.TemporaryDirectory(prefix='tio-transfer-protect-') as root:
    sentinel=Path(root)/'keep.bin'; sentinel.write_bytes(b'unchanged')
    result=subprocess.run([helper, '/unused', root, '2', '1'],capture_output=True)
    assert result.returncode == 1 and b'empty receive directory' in result.stderr
    assert sentinel.read_bytes() == b'unchanged'
    print('PASS receive refuses nonempty directory')
# A peer that never starts cannot leave an orphaned transfer process.
import socket
with tempfile.TemporaryDirectory(prefix='tio-transfer-timeout-') as root:
    path=str(Path(root)/'socket'); server=socket.socket(socket.AF_UNIX);server.bind(path);server.listen()
    source=Path(root)/'send.bin';source.write_bytes(b'test')
    app=subprocess.Popen([helper,path,str(source),'2','0','1'],stdout=subprocess.PIPE)
    conn,_=server.accept()
    output=app.communicate(timeout=5)[0]
    assert app.returncode == 1 and b'timed out' in output,output
    conn.close();server.close()
    print('PASS stalled peer timeout and process cleanup')
