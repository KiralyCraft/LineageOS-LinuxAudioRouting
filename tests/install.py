#!/usr/bin/env python3
"""Replacing an actively executing file must not fail or interrupt the old process."""
import pathlib,shutil,subprocess,sys,tempfile
source=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='audio-install-fixture-') as directory:
    root=pathlib.Path(directory);payload=root/'payload';target=root/'target'
    payload.mkdir();target.mkdir();shutil.copy2(shutil.which('sleep'),target/'program')
    process=subprocess.Popen([str(target/'program'),'15'])
    try:
        shutil.copy2(shutil.which('true'),payload/'program')
        subprocess.run([sys.executable,str(source/'linux/install-files.py'),str(payload),str(target)],check=True)
        assert process.poll() is None
        assert subprocess.run([str(target/'program')]).returncode==0
        assert (target/'program').read_bytes()==(payload/'program').read_bytes()
        assert not list(target.glob('.audio-install-*'))
    finally:process.terminate();process.wait(timeout=2)
print('PASS atomic live executable update preserves the running process and executable permissions')
