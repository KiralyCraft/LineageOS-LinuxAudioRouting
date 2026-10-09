"""Mock Android hardware, using real shared descriptors and release/acquire atomics."""
import array, ctypes, ctypes.util, mmap, os, select, socket, struct, time

class SharedFixture:
    def __init__(self, sock, request):
        self.sock=sock; self.request=request; self.fds=[]; self.memory=None
        try:
            for _ in range(3):
                marker,anc,flags,_=sock.recvmsg(1,socket.CMSG_SPACE(32))
                received=array.array('i')
                for level,kind,data in anc:
                    if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:received.frombytes(data)
                self.fds.extend(received)
                assert marker==b'F' and len(received)==1 and not flags
            size=os.fstat(self.fds[0]).st_size
            self.memory=mmap.mmap(self.fds[0],size)
            self.address=ctypes.addressof(ctypes.c_char.from_buffer(self.memory))
            self.atomic=ctypes.CDLL(ctypes.util.find_library('atomic'))
            self.load8=getattr(self.atomic,'__atomic_load_8');self.load8.argtypes=[ctypes.c_void_p,ctypes.c_int];self.load8.restype=ctypes.c_uint64
            self.store8=getattr(self.atomic,'__atomic_store_8');self.store8.argtypes=[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_int]
            self.load4=getattr(self.atomic,'__atomic_load_4');self.load4.argtypes=[ctypes.c_void_p,ctypes.c_int];self.load4.restype=ctypes.c_uint32
            self.store4=getattr(self.atomic,'__atomic_store_4');self.store4.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int]
            magic,version,epoch,rate,channels,sample,capture,capacity=struct.unpack_from('<IIQIIIIQ',self.memory)
            assert (magic,version,epoch,rate,channels)==(0x53445541,1,request['epoch'],request['rate'],request['channels'])
            assert capacity==size-4096 and sample in (2,4)
            self.width=sample*channels;self.capacity=capacity;self.capture=bool(capture);self.serial=0
        except:
            self.close();raise
    def get(self,offset):return self.load8(self.address+offset,2)
    def put(self,offset,value):self.store8(self.address+offset,value,3)
    def notify(self,fd):
        try:os.eventfd_write(fd,1)
        except BlockingIOError:pass
    def clear(self,fd):
        try:os.eventfd_read(fd)
        except BlockingIOError:pass
    def clock(self,played):
        self.serial+=1;self.store4(self.address+64,self.serial,3)
        self.put(72,played);self.put(80,played);self.put(88,time.monotonic_ns())
        self.put(96,played);self.put(104,time.monotonic_ns()+100_000_000)
        self.serial+=1;self.store4(self.address+64,self.serial,3)
    def live(self):
        return not self.load4(self.address+56,2) and not select.select([self.sock],[],[],0)[0]
    def run(self,collect):
        request=self.request;rate=request['rate'];period=.02 if self.width==2 else .01
        frames=int(rate*period);frame=0;played=0;next_tick=time.monotonic()+period;stalled=False
        # Only the fixture uses synthetic hardware deadlines. Production I/O
        # progress comes from AAudio, never from this scheduler.
        while self.live():
            reader=self.get(40);writer=self.get(48);available=writer-reader
            assert 0<=available<=self.capacity
            now=time.monotonic()
            if self.capture:
                if now>=next_tick and self.capacity-available>=frames*self.width:
                    if request['endpoint'].startswith('fixture.native'):
                        data=bytes(((i*31)^(i>>8))&255 for i in range(frame*self.width,(frame+frames)*self.width))
                    elif self.width==2:data=struct.pack('<'+str(frames)+'h',*([8192]*frames))
                    else:data=struct.pack('<'+str(frames)+'f',*([.25]*frames))
                    offset=writer%self.capacity;first=min(len(data),self.capacity-offset)
                    self.memory[4096+offset:4096+offset+first]=data[:first];self.memory[4096:4096+len(data)-first]=data[first:]
                    self.put(48,writer+len(data));frame+=frames;self.clock(frame);self.notify(self.fds[1]);next_tick+=period
                    if request['endpoint']=='fixture.headset.input' and not stalled and frame>=rate//10:next_tick+=.08;stalled=True
            else:
                if now>=next_tick:
                    complete=reader//self.width
                    if request['endpoint']=='fixture.native.undrained':complete-=complete%1920
                    played=min(complete,played+frames);self.clock(played);self.notify(self.fds[2]);next_tick+=period
                count=min(available,max(0,rate//25-(reader//self.width-played))*self.width)
                if count:
                    offset=reader%self.capacity;first=min(count,self.capacity-offset)
                    data=self.memory[4096+offset:4096+offset+first]+self.memory[4096:4096+count-first]
                    collect(data);self.put(40,reader+count);self.notify(self.fds[2])
            event=self.fds[2] if self.capture else self.fds[1]
            ready,_,_=select.select([self.sock,event],[],[],max(.001,min(.01,next_tick-time.monotonic())))
            if self.sock in ready:break
            if event in ready:self.clear(event)
    def close(self):
        if self.memory is not None:self.memory.close();self.memory=None
        for fd in self.fds:os.close(fd)
        self.fds=[]
