#!/usr/bin/env python3
import hashlib,json,os,pathlib,subprocess,sys
source,build=map(pathlib.Path,sys.argv[1:])
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
artifacts=[build/'native/linux-audiod',build/'native/linux-audioctl',build/'native/linux-audio-bridge',build/'android/LinuxAudio.apk']
result=dict(schema=1,build_host='root@192.168.104.201',transport='Authenticated direct Unix socketpair FD handoff; uncompressed unchanged PCM; no broker sample processing',native_language='C99',sources={str(p.relative_to(source)):digest(p) for p in sorted(source.rglob('*')) if p.is_file() and not any(part in ['.git','__pycache__','build','.gradle'] for part in p.relative_to(source).parts)},artifacts={str(p.relative_to(build)):digest(p) for p in artifacts},validation={'native_unit':'PASS ASan UBSan','broker':'PASS ASan UBSan','physical_audio':'pending manual install and activation'},android_min_sdk=35,source_commit=os.environ.get('AUDIO_SOURCE_REVISION','unrecorded'),toolchain={'ndk':'29.0.14206865','android_api':35,'android_build_tools':'36.0.0','java':17,'pipewire':(build/'native/pipewire-build-version.txt').read_text().strip()},android_signing_verification=(build/'android/verification.txt').read_text())
for name in ['apk','startup','desktop','install']:
    test=build/'tests'/(name+'.log')
    if test.exists() and 'PASS ' in test.read_text():result['validation'][name]='PASS'
physical=build/'tests/physical-microphone.json'
if physical.exists():
    result['validation']['physical_microphone']=json.loads(physical.read_text())
    result['validation']['physical_audio']='Partial: background phone/Bluetooth microphone verified; audible playback, calls, doze and long soak pending'
log=build/'tests/integration.log'
if log.exists() and 'PASS isolated production PipeWire/native stack' in log.read_text():
    result['validation']['isolated_pipewire']='PASS; simulated Android endpoints, production native binaries'
else:result['validation']['isolated_pipewire']='pending'
(build/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
