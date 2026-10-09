#!/usr/bin/env python3
"""Daemon survives launcher process-group termination; stale sockets are not readiness."""
import os,pathlib,signal,subprocess,sys,tempfile,time
source=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='audio-start-fixture-') as directory:
    root=pathlib.Path(directory);prefix=root/'prefix';runtime=root/'runtime';mock=root/'mock'
    (prefix/'bin').mkdir(parents=True);(prefix/'config').mkdir();runtime.mkdir(mode=0o700);mock.mkdir()
    stock=root/'stock';stock.mkdir()
    for name in ['pipewire.conf','pipewire-pulse.conf','wireplumber.conf']:(stock/name).write_text('# fixture\n')
    for name in ['pipewire-extra.conf','pulse-extra.conf','wireplumber-extra.conf']:(prefix/'config'/name).write_text('# fixture\n')
    script=prefix/'bin/linux-audio'
    script.write_text((source/'linux/bin/linux-audio').read_text().replace('AUDIO_RUNTIME=/tmp/linux-audio-$(id -u)','AUDIO_RUNTIME='+str(runtime)).replace('/usr/share/pipewire/',str(stock)+'/').replace('/usr/share/wireplumber/',str(stock)+'/'))
    script.chmod(0o755)
    fake='''#!/usr/bin/env python3
import os,pathlib,signal,socket,sys,time
root=pathlib.Path(os.environ['MOCK_RUNTIME']);name=pathlib.Path(sys.argv[0]).name
if name in ['pw-cli','wpctl','pactl']:
    marker='pipewire' if name!='pactl' else 'pipewire-pulse'
    sys.exit(0 if (root/(marker+'.ready')).exists() else 1)
marker=root/(name+'.ready')
def stop(*args):
    marker.unlink(missing_ok=True);sys.exit(0)
signal.signal(signal.SIGTERM,stop)
if name=='wireplumber' and not (root/'pipewire.ready').exists():sys.exit(1)
if name in ['pipewire','pipewire-pulse']:
    path=root/'linux-audio' if name=='pipewire' else root/'pulse/native';path.parent.mkdir(exist_ok=True)
    if not path.exists():sock=socket.socket(socket.AF_UNIX);sock.bind(str(path));sock.close()
    time.sleep(.3)
marker.touch()
while True:time.sleep(1)
'''
    for name in ['pipewire','pipewire-pulse','wireplumber','pw-cli','wpctl','pactl']:
        p=mock/name;p.write_text(fake);p.chmod(0o755)
    bridge=prefix/'bin/linux-audio-bridge';bridge.write_text('#!/bin/sh\nexit 1\n');bridge.chmod(0o755)
    env=os.environ.copy();env.update(PATH=str(mock)+':'+env['PATH'],MOCK_RUNTIME=str(runtime))
    for iteration in range(2):
        launcher=subprocess.Popen(['bash','-c','"$1" start; sleep 20','fixture',str(script)],env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            for _ in range(120):
                if (runtime/'pipewire-pulse.ready').exists():break
                time.sleep(.02)
            assert (runtime/'pipewire.ready').exists() and (runtime/'wireplumber.ready').exists()
            time.sleep(.05)
            os.killpg(launcher.pid,signal.SIGKILL);launcher.wait(timeout=3)
            pid=int((runtime/'supervisor.pid').read_text().split()[0]);os.kill(pid,0)
            assert os.getpgid(pid)!=launcher.pid
            assert subprocess.run([str(script),'start'],env=env,stdout=subprocess.DEVNULL).returncode==0
        finally:
            if launcher.poll() is None:os.killpg(launcher.pid,signal.SIGKILL);launcher.wait()
            subprocess.run([str(script),'stop'],env=env,check=True)
        assert not (runtime/'pipewire.ready').exists() and not (runtime/'pipewire-pulse.ready').exists()
    print('PASS detached supervisor, idempotent start, owned stop, stale-socket reconnect readiness')
