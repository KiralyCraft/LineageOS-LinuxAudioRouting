#!/usr/bin/env python3
"""Production broker fixtures. No Android devices, sound or display state touched."""
import array, json, os, pathlib, select, socket, struct, subprocess, sys, tempfile, time, threading
binary = sys.argv[1]
name = 'linux-audio-test-' + str(os.getpid())
def send(sock, msg):
    data = json.dumps(msg,separators=(',',':')).encode()
    sock.sendall(struct.pack('<I',len(data))+data)
def exact(sock,n):
    data=bytearray()
    while len(data)<n:
        part=sock.recv(n-len(data))
        if not part: raise EOFError('connection closed')
        data.extend(part)
    return bytes(data)
def receive(sock):
    n,=struct.unpack('<I',exact(sock,4));assert 0<n<=65536
    return json.loads(exact(sock,n))
def client(role='linux'):
    s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect('\0'+name)
    send(s,dict(op='hello',version=1,role=role));assert receive(s)['op']=='ready'
    if role=='linux':receive(s)
    return s
def reply(sock,request):
    while True:
        msg=receive(sock)
        if msg.get('op')=='reply' and msg.get('id')==request:return msg

def attach(stream,side):
    s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect('\0'+name)
    send(s,dict(op='hello',version=1,role='data',side=side,stream=stream['stream'],token=stream['token']))
    assert receive(s)['op']=='ready'
    data,anc,flags,_=s.recvmsg(1,socket.CMSG_SPACE(4))
    assert data==b'F' and not flags&socket.MSG_CTRUNC
    fds=array.array('i')
    for level,kind,value in anc:
        if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:fds.frombytes(value)
    assert len(fds)==1
    out=socket.socket(fileno=fds[0]);out.settimeout(3);s.close();return out

with tempfile.TemporaryDirectory(prefix='audio-broker-fixture-') as directory:
    log=open(pathlib.Path(directory)/'broker.log','w+')
    p=subprocess.Popen([binary,'--test','isolated','--socket',name,'--linux-uid',str(os.getuid())],stdout=log,stderr=log)
    peers=[]
    try:
        for _ in range(100):
            try:linux=client();break
            except (ConnectionRefusedError,FileNotFoundError):time.sleep(.02)
        else:raise RuntimeError('broker startup failed')
        peers.append(linux)
        send(linux,dict(op='status',id=1));assert not reply(linux,1)['helper']
        helper=client('helper');peers.append(helper)
        device=dict(key='fixture.output',group='fixture',name='Fixture Output',direction='output',android_id=10,profile='media',format='f32le',rate=48000,channels=2,available=True)
        send(helper,dict(op='inventory',devices=[device],suspended=False,reason=''))
        inventory=receive(linux);assert inventory['op']=='inventory'
        generation=inventory['generation']
        send(linux,dict(op='open',id=2,endpoint=device['key'],generation=generation-1));assert not reply(linux,2)['ok']
        send(linux,dict(op='open',id=3,endpoint=device['key'],generation=generation))
        request=receive(helper);assert request['op']=='open'
        android=attach(request,'android');peers.append(android)
        send(helper,dict(op='reply',id=request['id'],ok=True,stream=request['stream'],epoch=request['epoch'],token=request['token'],rate=48000,channels=2,format='f32le'))
        opened=reply(linux,3);assert opened['ok']
        producer=attach(opened,'linux');peers.append(producer)
        activation=receive(helper);assert activation['op']=='activate'
        before=len(os.listdir('/proc/'+str(p.pid)+'/fd'))
        payload=os.urandom(65536)
        writer=threading.Thread(target=producer.sendall,args=(payload,));writer.start();assert exact(android,len(payload))==payload;writer.join(timeout=3);assert not writer.is_alive()
        returned=os.urandom(65536)
        writer=threading.Thread(target=android.sendall,args=(returned,));writer.start();assert exact(producer,len(returned))==returned;writer.join(timeout=3);assert not writer.is_alive()
        # Arbitrary bytes are transported directly, not interpreted by the broker.
        assert len(os.listdir('/proc/'+str(p.pid)+'/fd'))==before
        print('PASS direct bidirectional raw bytes, capabilities and no broker data descriptors')
        duplicate=socket.socket(socket.AF_UNIX);duplicate.settimeout(2);duplicate.connect('\0'+name)
        send(duplicate,dict(op='hello',version=1,role='data',side='linux',stream=opened['stream'],token=opened['token']))
        assert duplicate.recv(1)==b'';duplicate.close()
        bad=socket.socket(socket.AF_UNIX);bad.settimeout(2);bad.connect('\0'+name)
        send(bad,dict(op='hello',version=2,role='linux'));assert bad.recv(1)==b'';bad.close()
        print('PASS duplicate attachment and version rejection')
        send(helper,dict(op='suspend',reason='fixture Android call'))
        event=receive(linux);assert event['op']=='inventory' and event['suspended']
        close=receive(helper);assert close['op']=='close'
        # Android acknowledges shutdown by closing its direct endpoint.
        android.close();peers.remove(android)
        assert producer.recv(1)==b''
        send(linux,dict(op='open',id=4,endpoint=device['key'],generation=generation));assert not reply(linux,4)['ok']
        print('PASS call suspension blocks new streams and retires old channel')
        send(helper,dict(op='inventory',devices=[device],suspended=False,reason=''))
        while True:
            event=receive(linux)
            if event.get('op')=='inventory' and not event.get('suspended'):break
        helper.close();peers.remove(helper)
        while True:
            event=receive(linux)
            if event.get('op')=='inventory' and not event.get('helper'):break
        assert event['devices']==[]
        print('PASS helper death invalidates inventory')
        # A fragmented, stalled client cannot block the dispatcher.
        stalled=socket.socket(socket.AF_UNIX);stalled.connect('\0'+name);stalled.sendall(b'\x20')
        start=time.monotonic();send(linux,dict(op='status',id=5));assert reply(linux,5)['ok'];assert time.monotonic()-start<.5
        stalled.close()
        print('PASS stalled control peer leaves status responsive')
    finally:
        for peer in peers:peer.close()
        p.terminate()
        try:p.wait(timeout=8)
        except subprocess.TimeoutExpired:p.kill();p.wait();raise
        log.flush();log.seek(0);text=log.read();log.close()
        assert p.returncode==0,text
        assert 'AddressSanitizer' not in text and 'runtime error:' not in text,text
print('PASS production broker lifecycle with sanitizers')
