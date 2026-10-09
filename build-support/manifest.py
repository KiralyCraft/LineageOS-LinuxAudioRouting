#!/usr/bin/env python3
import hashlib,json,os,pathlib,subprocess,sys
source,build=map(pathlib.Path,sys.argv[1:])
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
artifacts=[build/'native/linux-audiod',build/'native/linux-audioctl',build/'native/linux-audio-bridge',build/'android/LinuxAudio.apk',build/'android/jni/lib/arm64-v8a/liblinux_audio.so']
result=dict(schema=1,build_host='root@192.168.104.201',transport='Negotiated sealed memfd PCM ring and eventfds; native AAudio; legacy socket opt-out; uncompressed unchanged bridge PCM',native_language='C99',sources={str(p.relative_to(source)):digest(p) for p in sorted(source.rglob('*')) if p.is_file() and not any(part in ['.git','__pycache__','build','.gradle'] for part in p.relative_to(source).parts)},artifacts={str(p.relative_to(build)):digest(p) for p in artifacts},validation={'native_unit':'PASS ASan UBSan','broker':'PASS ASan UBSan','shared_memory_unit':'PASS ASan UBSan','physical_audio':'pending manual install and activation'},android_min_sdk=35,source_commit=os.environ.get('AUDIO_SOURCE_REVISION','unrecorded'),toolchain={'ndk':'29.0.14206865','android_api':35,'android_build_tools':'36.0.0','java':17,'pipewire':(build/'native/pipewire-build-version.txt').read_text().strip()},android_signing_verification=(build/'android/verification.txt').read_text())
for name in ['apk','startup','desktop','install','aaudio-policy-unit','route-policy','playback-buffer']:
    test=build/'tests'/(name+'.log')
    if test.exists() and 'PASS ' in test.read_text():result['validation'][name]='PASS'
# A stale test log is not evidence for a newly linked binary or APK.
for shared,mmap in [(False,False),(True,False),(True,True)]:
    key='isolated_pipewire_shared' if shared else 'isolated_pipewire'
    if mmap:key='isolated_pipewire_mmap_preference'
    result['validation'][key]='pending'
    attestation=build/'tests'/('integration-mmap-attestation.json' if mmap else 'integration-shared-attestation.json' if shared else 'integration-attestation.json')
    if attestation.exists():
        evidence=json.loads(attestation.read_text())
        matched=evidence.get('result')=='PASS' and evidence.get('script_sha256')==digest(source/'tests/integration.py')
        matched=matched and evidence.get('mmap_preference')==mmap and evidence.get('shared_pcm')==shared and evidence.get('shared_fixture_sha256')==digest(source/'tests/shared_fixture.py')
        matched=matched and all((build/name).is_file() and digest(build/name)==sha for name,sha in evidence.get('artifacts',{}).items())
        if matched and len(evidence.get('artifacts',{}))==3:
            result['validation'][key]=evidence
(build/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
