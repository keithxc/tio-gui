#!/usr/bin/env python3
"""Exercise a bundled app on an isolated PTY/config. Requires macOS Accessibility permission. Temporarily selects US/ABC for physical test keys, restoring the previous input source. Optional screenshots need screen-capture permission."""
import os,pty,subprocess,tempfile,time,select,json,sys
from pathlib import Path
repo=Path(__file__).resolve().parents[1]; (repo/'.cache').mkdir(exist_ok=True)
helper=repo/'.cache/macos-input'
sdk_env=dict(os.environ);sdk_env.pop('SDKROOT',None);sdk_env.pop('DEVELOPER_DIR',None)
sdk=subprocess.check_output(['/usr/bin/xcrun','--sdk','macosx','--show-sdk-path'],env=sdk_env,text=True).strip()
subprocess.run(['/usr/bin/xcrun','swiftc','-sdk',sdk,str(repo/'tests/macos_input.swift'),'-o',str(helper)],env=sdk_env,check=True)
app=Path(sys.argv[1]).resolve(); root=Path(tempfile.mkdtemp(prefix='tio-bundle-smoke-'));(root/'config/tio-gui').mkdir(parents=True)
master,slave=pty.openpty();path=os.ttyname(slave);log=root/'received.log'
(root/'config/tio-gui/serial.ini').write_text(f'[general]\nlanguage=en\nnative-tools-version=1\nrestore-tabs=false\n[defaults]\ndevice={path}\nbaud=115200\nreconnect=false\nlogging=true\nlog-file={log}\nlog-append=true\nlog-strip=false\n')
env={'HOME':str(Path.home()),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','TMPDIR':os.environ.get('TMPDIR','/tmp'),'XDG_CONFIG_HOME':str(root/'config'),'XDG_CACHE_HOME':str(root/'cache'),'LANG':'en_US.UTF-8'}
stdout=(root/'app.log').open('w')
# Deny development roots even when they exist on the test Mac. This catches
# implicit dlopen/resource fallbacks that a system-only PATH cannot detect.
command=['/usr/bin/sandbox-exec','-p','(version 1)(allow default)(deny file-read* (subpath "/nix/store") (subpath "/opt/homebrew") (subpath "/usr/local/Cellar"))',str(app/'Contents/MacOS/tio-gui')]
process=subprocess.Popen(command,env=env,stdout=stdout,stderr=subprocess.STDOUT)
(repo/'.cache/packaged-smoke-state.json').write_text(json.dumps({'pid':process.pid,'root':str(root),'device':path}))
print('PID',process.pid,'ROOT',root,flush=True)
try:
 time.sleep(2)
 def input_(*args):
  r=subprocess.run([str(helper),str(process.pid),*args],text=True,capture_output=True,timeout=5)
  print(r.stdout.strip(),flush=True)
  assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
 input_('click','0.94','542');time.sleep(.3)
 for _ in range(50):
  if log.exists():break
  time.sleep(.1)
 if os.environ.get('TIO_TEST_LAYOUT_DIR'):input_('snapshot',str(Path(os.environ['TIO_TEST_LAYOUT_DIR'])/'bundle-connected.png'))
 assert log.exists(), (root/'app.log').read_text()
 payload=bytes(range(256))*8+b'\r\nPACKAGE-RX-OK\r\n'
 time.sleep(.5);os.write(master,payload)
 for _ in range(40):
  time.sleep(.1)
  if log.exists() and log.stat().st_size>=len(payload):break
 assert log.read_bytes()==payload,(len(log.read_bytes()) if log.exists() else -1,len(payload),(root/'app.log').read_text())
 print('PASS bundled application receives and logs',len(payload),'exact bytes without development PATH',flush=True)
 input_('click','0.4','64');input_('text','bundle-smoke')
 if os.environ.get('TIO_TEST_LAYOUT_DIR'):input_('snapshot',str(Path(os.environ['TIO_TEST_LAYOUT_DIR'])/'bundle-send.png'))
 input_('key','36')
 received=b'';deadline=time.monotonic()+4
 while time.monotonic()<deadline and not received.endswith(b'\r'):
  if select.select([master],[],[],.1)[0]:received+=os.read(master,4096)
 assert received==b'bundle-smoke\r',received
 print('PASS bundled application sends exact text and CR through its installed UI',flush=True)
 input_('key','97');time.sleep(.2)
finally:
 process.terminate()
 try:process.wait(timeout=5)
 except subprocess.TimeoutExpired:process.kill();process.wait()
 os.close(master);os.close(slave);stdout.close()
