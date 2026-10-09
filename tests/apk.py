#!/usr/bin/env python3
"""APK bootstrap: pipe-backed Binder output, matching APK skip and broker independence."""
import hashlib,os,pathlib,shutil,subprocess,sys,tempfile,time
source=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='audio-apk-fixture-') as directory:
    root=pathlib.Path(directory);mock=root/'mock';mock.mkdir();installed=root/'installed.apk';calls=root/'pm.calls'
    pm=mock/'pm';pm.write_text('''#!/usr/bin/env python3
import os,pathlib,shutil,stat,sys
p=pathlib.Path(os.environ['MOCK_INSTALLED'])
with open(os.environ['MOCK_CALLS'],'a') as f:f.write(sys.argv[1]+'\\n')
if not stat.S_ISFIFO(os.fstat(1).st_mode):print('FAILED_TRANSACTION');sys.exit(2)
if sys.argv[1]=='path':
    if not p.exists():sys.exit(1)
    print('package:'+str(p));sys.exit(0)
if os.environ.get('MOCK_FAIL')=='1':print('fixture installation failed');sys.exit(2)
data=sys.stdin.buffer.read();assert len(data)==int(sys.argv[sys.argv.index('-S')+1])
p.write_bytes(data);print('Success')
''');pm.chmod(0o755)
    env=os.environ.copy();env.update(PATH=str(mock)+':'+env['PATH'],MOCK_INSTALLED=str(installed),MOCK_CALLS=str(calls))
    apk=root/'payload.apk';apk.write_bytes(os.urandom(1024));state=root/'state.sha';log=root/'install.log'
    command=['sh','-c','. "$1"; audio_install_apk "$2" "$3" "$4"','fixture',str(source/'module/apk.sh'),str(apk),str(state),str(log)]
    subprocess.run(command,env=env,check=True)
    assert installed.read_bytes()==apk.read_bytes() and state.read_text().strip()==hashlib.sha256(apk.read_bytes()).hexdigest()
    before=calls.read_text().count('install');subprocess.run(command,env=env,check=True);assert calls.read_text().count('install')==before
    print('PASS pipe-backed install output, streamed exact APK size, verified package, matching APK skip')
    installed.unlink();state.unlink();env['MOCK_FAIL']='1'
    assert subprocess.run(command,env=env).returncode==1 and not state.exists()
    assert 'broker will still start' in log.read_text()
    # The complete service must keep its broker running despite the failed installer.
    module=root/'module';(module/'bin').mkdir(parents=True);(module/'app').mkdir()
    shutil.copy2(apk,module/'app/LinuxAudio.apk');shutil.copy2(source/'module/apk.sh',module/'apk.sh')
    runtime=root/'runtime'
    service=module/'service.sh';service.write_text((source/'module/service.sh').read_text().replace('AUDIO_RUNTIME=/dev/linux-audio-routing','AUDIO_RUNTIME='+str(runtime)))
    prop=mock/'getprop';prop.write_text('#!/bin/sh\nprintf "1\\n"\n');prop.chmod(0o755)
    broker=module/'bin/linux-audiod';broker.write_text('#!/bin/sh\nprintf "READY fixture broker\\n"\nwhile true; do sleep 1; done\n');broker.chmod(0o755)
    p=subprocess.Popen(['sh',str(service)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if (runtime/'apk-install.log').exists():break
            time.sleep(.02)
        assert p.poll() is None and 'READY fixture broker' in (runtime/'broker.log').read_text()
        assert 'broker will still start' in (runtime/'apk-install.log').read_text()
    finally:p.terminate();p.wait(timeout=4)
    print('PASS root service starts broker before APK setup and survives failed installation')
