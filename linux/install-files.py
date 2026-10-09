#!/usr/bin/env python3
"""Replace owned runtime files atomically, preserving running executable inodes."""
import os,pathlib,shutil,sys,tempfile
source,target=map(pathlib.Path,sys.argv[1:])
for file in sorted(source.rglob('*')):
    destination=target/file.relative_to(source)
    if file.is_dir():destination.mkdir(parents=True,exist_ok=True);continue
    if not file.is_file() or file.is_symlink():raise SystemExit('Unexpected runtime entry: '+str(file))
    destination.parent.mkdir(parents=True,exist_ok=True)
    descriptor,name=tempfile.mkstemp(prefix='.audio-install-',dir=destination.parent)
    os.close(descriptor)
    try:
        shutil.copy2(file,name)
        os.replace(name,destination)
    finally:
        if os.path.exists(name):os.unlink(name)
