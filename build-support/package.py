#!/usr/bin/env python3
"""Package only matched, built audio artifacts. No install or device mutation."""
import hashlib,json,pathlib,shutil,subprocess,sys,zipfile
source,build,output=map(pathlib.Path,sys.argv[1:])
output.mkdir(parents=True,exist_ok=True)
module=output/'magisk';module.mkdir(exist_ok=True)
shutil.copytree(source/'module',module,dirs_exist_ok=True)
(module/'app').mkdir(exist_ok=True)
shutil.copy2(build/'native/linux-audiod',module/'bin/linux-audiod')
shutil.copy2(build/'android/LinuxAudio.apk',module/'app/LinuxAudio.apk')
shutil.copy2(build/'manifest.json',module/'manifest.json')
with zipfile.ZipFile(output/'LinuxAudioRouting-0.1.0.zip','w',zipfile.ZIP_DEFLATED) as archive:
    for file in sorted(module.rglob('*')):
        if file.is_file():archive.write(file,file.relative_to(module))
shutil.copytree(source/'linux',output/'linux',dirs_exist_ok=True)
for name in ['linux-audioctl','linux-audio-bridge']:
    shutil.copy2(build/'native'/name,output/'linux/bin'/name)
shutil.copy2(build/'android/LinuxAudio.apk',output/'LinuxAudio.apk')
shutil.copy2(build/'manifest.json',output/'manifest.json')
for file in ['README.md','PROTOCOL.md','install-linux.sh']:
    shutil.copy2(source/file,output/file)
shutil.copytree(build/'tests',output/'validation',dirs_exist_ok=True,ignore=shutil.ignore_patterns('broker-host','broker-arm','unit','transport-fixture'))
files=[p for p in sorted(output.rglob('*')) if p.is_file() and p.name!='SHA256SUMS']
(output/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(output))+'\n' for p in files))
print(output)
