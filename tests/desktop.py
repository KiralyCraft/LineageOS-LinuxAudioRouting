#!/usr/bin/env python3
"""Configuration migration and rollback evidence using a disposable home only."""
import os,pathlib,subprocess,sys,tempfile,xml.etree.ElementTree as ET
source=pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='audio-desktop-test-') as directory:
    root=pathlib.Path(directory)
    (root/'.config/openbox').mkdir(parents=True)
    bash=root/'.bashrc';bash.write_text('export PULSE_SERVER=unix:/original/audio\n')
    rc=root/'.config/openbox/rc.xml';original='<openbox_config xmlns="http://openbox.org/3.4/rc"><keyboard><keybind key="XF86AudioMute"><action name="Execute"><command>user-mute</command></action></keybind></keyboard></openbox_config>';rc.write_text(original)
    env=os.environ.copy();env['HOME']=directory
    prefix=root/'private'
    script=source/'linux/configure-desktop.py'
    subprocess.run([sys.executable,str(script),str(prefix)],env=env,check=True,stdout=subprocess.DEVNULL)
    assert 'export PULSE_SERVER=unix:/original/audio' in bash.read_text()
    assert bash.read_text().count('# BEGIN LINUX AUDIO')==1
    tree=ET.parse(rc);ns={'o':'http://openbox.org/3.4/rc'}
    bindings=tree.findall('.//o:keybind',ns)
    assert len(bindings)==3
    assert tree.find('.//o:keybind[@key="XF86AudioMute"]//o:command',ns).text=='user-mute'
    snapshot=rc.read_bytes();subprocess.run([sys.executable,str(script),str(prefix)],env=env,check=True,stdout=subprocess.DEVNULL)
    assert rc.read_bytes()==snapshot and bash.read_text().count('# BEGIN LINUX AUDIO')==1
    backups=sorted((root/'.local/state/linux-audio-routing').glob('backup-*'))
    assert (backups[0]/'.bashrc').read_text()=='export PULSE_SERVER=unix:/original/audio\n'
    assert (backups[0]/'.config/openbox/rc.xml').read_text()==original
print('PASS reversible user-only migration, original Pulse export retained, existing keybind preserved, idempotence')
