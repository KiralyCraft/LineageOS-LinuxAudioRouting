#!/usr/bin/env python3
"""Isolated PipeWire graph plus production native broker and simulated Android."""
import array, json, os, pathlib, select, socket, struct, subprocess, sys, tempfile, threading, time
source, build = map(pathlib.Path,sys.argv[1:])
name='linux-audio-pw-fixture-'+str(os.getpid())
processes=[]; opens=[]; errors=[]; samples=bytearray(); alternate_samples=bytearray(); native_samples=bytearray(); workers=[]; direct={}; sendlock=threading.Lock()
def exact(sock,n):
    data=bytearray()
    while len(data)<n:
        part=sock.recv(n-len(data))
        if not part: raise EOFError()
        data.extend(part)
    return bytes(data)
def send(sock,msg):
    data=json.dumps(msg,separators=(',',':')).encode()
    with sendlock:sock.sendall(struct.pack('<I',len(data))+data)
def recv(sock):return json.loads(exact(sock,struct.unpack('<I',exact(sock,4))[0]))
def connect(role):
    s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect('\0'+name)
    send(s,dict(op='hello',role=role,version=1));assert recv(s)['op']=='ready'
    s.settimeout(None);return s
def attach(request):
    s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect('\0'+name)
    send(s,dict(op='hello',role='data',version=1,side='android',stream=request['stream'],token=request['token']))
    assert recv(s)['op']=='ready'
    marker,anc,flags,_=s.recvmsg(1,socket.CMSG_SPACE(4));assert marker==b'F'
    fds=array.array('i');fds.frombytes(anc[0][2]);assert len(fds)==1
    d=socket.socket(fileno=fds[0]);s.close();d.settimeout(4);return d
header=struct.Struct('<IIQQQII')
def consume(d,request):
    frame=0
    try:
        while True:
            magic,kind,epoch,serial,stamp,frames,n=header.unpack(exact(d,40))
            width=request['channels']*4
            assert (magic,kind,epoch,serial,n)==(0x50445541,1,request['epoch'],frame,frames*width)
            data=exact(d,n)
            if request['endpoint'].startswith('fixture.native'):native_samples.extend(data)
            elif request['endpoint']=='fixture.alternate':alternate_samples.extend(data)
            else:samples.extend(data)
            frame+=frames
            d.sendall(header.pack(0x50445541,2,epoch,frame,time.monotonic_ns(),0,0))
    except (EOFError,OSError):pass
    except Exception as e:errors.append(repr(e))
def produce(d,request):
    frame=0;deadline=time.monotonic();periods=0;stalled=False
    headset=request.get('format')=='s16le';width=2 if headset else 4
    frames=request['rate']//50 if headset else request['rate']//100
    period=frames/request['rate']
    try:
        while True:
            payload=struct.pack('<'+str(frames)+'f',*([.25]*frames))
            if headset:payload=struct.pack('<'+str(frames)+'h',*([8192]*frames))
            if request['endpoint'].startswith('fixture.native'):payload=bytes(((i*31)^(i>>8))&255 for i in range(frame*width,(frame+frames)*width))
            d.sendall(header.pack(0x50445541,1,request['epoch'],frame,time.monotonic_ns(),frames,len(payload))+payload)
            frame+=frames;periods+=1;deadline+=period
            if request['endpoint']=='fixture.headset.input' and not stalled and frame>=request['rate']//10:
                deadline+=.08;stalled=True
            jitter=.005 if headset and periods%2 else 0
            time.sleep(max(0,deadline+jitter-time.monotonic()))
    except OSError:pass
    except Exception as e:errors.append(repr(e))

def helper_loop(helper):
    try:
        while True:
            request=recv(helper);op=request['op']
            print('Helper request',op,request.get('endpoint',''),request.get('stream',''),flush=True)
            if op=='open':
                opens.append(request['endpoint'])
                # A deliberate cold-open delay tests Linux's retention path.
                time.sleep(.04)
                d=attach(request);direct[request['stream']]=(d,request)
                send(helper,dict(op='reply',id=request['id'],ok=True,stream=request['stream'],epoch=request['epoch'],token=request['token']))
            elif op=='activate':
                d,r=direct[request['stream']]
                target=produce if r['endpoint'].endswith('input') else consume
                t=threading.Thread(target=target,args=(d,r),daemon=True);t.start();workers.append(t)
            elif op=='close':
                item=direct.pop(request['stream'],None)
                if item:
                    try:item[0].shutdown(socket.SHUT_RDWR)
                    except OSError:pass
                    item[0].close()
            else:raise AssertionError(op)
    except (EOFError,OSError):pass
    except Exception as e:errors.append(repr(e))
with tempfile.TemporaryDirectory(prefix='audio-pw-') as directory:
    root=pathlib.Path(directory);os.chmod(root,0o700)
    env=os.environ.copy();env.update(PULSE_SERVER='unix:'+directory+'/pulse/native',PULSE_RUNTIME_PATH=directory+'/pulse',PIPEWIRE_RUNTIME_DIR=directory,PIPEWIRE_REMOTE='linux-audio',PIPEWIRE_CONFIG_DIR=directory,WIREPLUMBER_CONFIG_DIR=directory)
    for base,extra in [('/usr/share/pipewire/pipewire.conf','pipewire-extra.conf'),('/usr/share/pipewire/pipewire-pulse.conf','pulse-extra.conf'),('/usr/share/wireplumber/wireplumber.conf','wireplumber-extra.conf')]:
        conf=root/pathlib.Path(base).name;conf.write_text(pathlib.Path(base).read_text())
        fragments=root/(conf.name+'.d');fragments.mkdir()
        (fragments/'99-linux-audio.conf').write_text((source/'linux/config'/extra).read_text())
    (root/'client.conf').write_text(pathlib.Path('/usr/share/pipewire/client.conf').read_text())
    logs={}
    def launch(key,args):
        logs[key]=(root/(key+'.log')).open('w+')
        p=subprocess.Popen(args,env=env,stdout=logs[key],stderr=subprocess.STDOUT);processes.append(p);return p
    def wait_for(test):
        for _ in range(160):
            if test():return
            time.sleep(.025)
        raise AssertionError('fixture timeout')
    try:
        launch('broker',[str(build/'tests/broker-arm'),'--test','isolated','--socket',name,'--linux-uid',str(os.getuid())])
        wait_for(lambda: pathlib.Path('/proc/net/unix').read_text().find(name)>=0)
        helper=connect('helper');threading.Thread(target=helper_loop,args=(helper,),daemon=True).start()
        devices=[dict(key='fixture.output',group='fixture',name='Fixture output',direction='output',rate=48000,channels=2,format='f32le',profile='media',available=True),dict(key='fixture.input',group='fixture',name='Fixture input',direction='input',rate=48000,channels=1,format='f32le',profile='media',available=True)]
        devices.append(dict(devices[0],key='fixture.alternate',name='Fixture other output',group='other'))
        devices.append(dict(devices[1],key='fixture.headset.input',name='Fixture headset microphone',group='headset',rate=16000,format='s16le',profile='headset'))
        native_devices=[dict(devices[1],key='fixture.native.input'),dict(devices[1],key='fixture.native.output',direction='output'),dict(devices[-1],key='fixture.native.headset.input')]
        send(helper,dict(op='inventory',devices=devices+native_devices,suspended=False,reason=''))
        time.sleep(.05)
        for operation,direction in [('playback','output'),('capture','input'),('capture-headset','headset.input')]:
            p=subprocess.run([str(build/'tests/transport-fixture'),name,operation,'fixture.native.'+direction],timeout=7,capture_output=True)
            assert p.returncode==0,p.stderr.decode()
        expected=bytes(((i*31)^(i>>8))&255 for i in range(20000));assert bytes(native_samples)==expected
        print('PASS native transport playback/capture exact 20000-byte patterns and tail',flush=True)
        send(helper,dict(op='inventory',devices=devices,suspended=False,reason=''))
        launch('pipewire',['pipewire','-c','pipewire.conf'])
        wait_for(lambda:(root/'linux-audio').exists())
        launch('policy',['wireplumber','--profile','linux-audio'])
        launch('pulse',['pipewire-pulse','-c','pipewire-pulse.conf'])
        bridge=launch('bridge',[str(build/'native/linux-audio-bridge'),'--socket',name])
        def nodes():
            p=subprocess.run(['pw-dump'],env=env,capture_output=True,timeout=3)
            if p.returncode!=0:return False
            data=json.loads(p.stdout)
            sinks=[n['id'] for n in data if n.get('info',{}).get('props',{}).get('node.name')=='linux-audio.fixture.output']
            return any(n.get('type')=='PipeWire:Interface:Port' and n.get('info',{}).get('props',{}).get('node.id') in sinks for n in data)
        wait_for(nodes)
        wait_for(lambda:(root/'pulse/native').exists())
        sinks=subprocess.check_output(['pactl','list','short','sinks'],env=env,timeout=3)
        assert b'linux-audio.fixture.output' in sinks and b'linux-audio.fixture.alternate' in sinks
        # Distinct patterns from concurrent applications must reach distinct devices.
        raw=struct.pack('<9600f',*([.125,-.375]*4800))
        alternate=struct.pack('<9600f',*([.5,-.625]*4800))
        players=[]
        for endpoint,payload in [('fixture.output',raw),('fixture.alternate',alternate)]:
            path=root/(endpoint+'.raw');path.write_bytes(payload)
            command=['pw-cat','--playback','--raw','--format','f32','--rate','48000','--channels','2','--target','linux-audio.'+endpoint,str(path)]
            if endpoint=='fixture.alternate':command=['pacat','--playback','--raw','--format=float32le','--rate=48000','--channels=2','--device=linux-audio.'+endpoint,str(path)]
            player=subprocess.Popen(command,env=env,stdout=logs['bridge'],stderr=logs['bridge'])
            processes.append(player);players.append(player)
        for player in players:
            player.wait(timeout=8);assert player.returncode==0
        wait_for(lambda:len(samples)>0)
        time.sleep(.6)
        values=struct.unpack('<'+str(len(samples)//4)+'f',samples)
        print('Playback sample counts', len(values), sum(v!=0 for v in values), flush=True)
        assert any(v==.125 for v in values) and any(v==-.375 for v in values),'No playback pattern'
        assert len([v for v in values if v!=0])==9600,'Initial/tail playback samples lost'
        other=struct.unpack('<'+str(len(alternate_samples)//4)+'f',alternate_samples)
        assert sum(v!=0 for v in other)==9600,'Other device samples lost'
        assert .5 in other and -.625 in other and .125 not in other and -.375 not in other,'Cross-device audio leaked'
        assert .5 not in values and -.625 not in values,'Cross-device audio leaked'
        print('PASS concurrent per-application device routing, startup and short final packet')
        # PulseAudio's move operation is the route used by pavucontrol's app selector.
        path=root/'move.raw';path.write_bytes(struct.pack('<192000f',*([.75,-.875]*96000)))
        mover=subprocess.Popen(['pacat','--playback','--raw','--format=float32le','--rate=48000','--channels=2','--device=linux-audio.fixture.output','--client-name=RoutingFixture','--stream-name=MoveFixture',str(path)],env=env,stdout=logs['bridge'],stderr=logs['bridge'])
        processes.append(mover)
        moving=[]
        def find_mover():
            info=json.loads(subprocess.check_output(['pactl','--format=json','list','sink-inputs'],env=env,timeout=3))
            moving[:]=[i['index'] for i in info if i.get('properties',{}).get('media.name')=='MoveFixture']
            return bool(moving)
        wait_for(find_mover)
        time.sleep(.15)
        subprocess.run(['pactl','move-sink-input',str(moving[0]),'linux-audio.fixture.alternate'],env=env,check=True,timeout=3)
        info=json.loads(subprocess.check_output(['pactl','--format=json','list','sink-inputs'],env=env,timeout=3))
        sinks=json.loads(subprocess.check_output(['pactl','--format=json','list','sinks'],env=env,timeout=3))
        target=next(i['index'] for i in sinks if i['name']=='linux-audio.fixture.alternate')
        assert next(i for i in info if i['index']==moving[0])['sink']==target
        mover.wait(timeout=8);assert mover.returncode==0
        time.sleep(.2)
        assert .75 in struct.unpack('<'+str(len(samples)//4)+'f',samples)
        assert .75 in struct.unpack('<'+str(len(alternate_samples)//4)+'f',alternate_samples)
        print('PASS moving one running PulseAudio application between devices')
        for target in ['fixture.input','fixture.headset.input']:
            recorder=subprocess.Popen(['pw-cat','--record','--raw','--format','f32','--rate','48000','--channels','1','--target','linux-audio.'+target,'-'],env=env,stdout=subprocess.PIPE,stderr=logs['bridge'])
            processes.append(recorder)
            required=19200
            if target=='fixture.headset.input':required=192000
            captured=bytearray();deadline=time.monotonic()+6
            while len(captured)<required and time.monotonic()<deadline:
                if select.select([recorder.stdout],[],[],.2)[0]:
                    part=os.read(recorder.stdout.fileno(),required-len(captured))
                    if not part:break
                    captured.extend(part)
            assert len(captured)==required
            assert .25 in struct.unpack('<'+str(required//4)+'f',captured),'Capture pattern absent'
            recorder.terminate();recorder.wait(timeout=5)
            if target=='fixture.headset.input':assert opens.count(target)==1,'Recorder reopened after temporary starvation'
            print('PASS graph capture',target)
        send(helper,dict(op='inventory',devices=[],suspended=False,reason=''))
        time.sleep(.1);assert bridge.poll() is None,'Unplug stopped the bridge'
        dump=subprocess.check_output(['pw-dump'],env=env,timeout=3);assert b'linux-audio.fixture.output' in dump,'Unplug removed the retained endpoint'
        send(helper,dict(op='suspend',reason='fixture call'))
        time.sleep(.1);assert bridge.poll() is None
        send(helper,dict(op='inventory',devices=devices,suspended=False,reason=''))
        time.sleep(.1);assert bridge.poll() is None
        assert not errors,errors
        logs['bridge'].flush();assert 'PCM stream failed' not in (root/'bridge.log').read_text()
        assert 'recorder retained' in (root/'bridge.log').read_text()
        print('PASS SCO capture starvation re-primes without dropping PCM or reopening recorder')
        print('PASS unplug retention and call/reconnect lifecycle')
    finally:
        for p in reversed(processes):
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=8)
            except subprocess.TimeoutExpired:p.kill();p.wait();errors.append('process did not stop')
        for d,r in list(direct.values()):d.close()
        if 'helper' in locals():helper.close()
        for key,log in logs.items():
            log.flush();log.seek(0);data=log.read();(build/'tests'/('integration-'+key+'.log')).write_text(data);log.close()
        assert not errors,errors
print('PASS isolated production PipeWire/native stack; no hardware audio touched')
