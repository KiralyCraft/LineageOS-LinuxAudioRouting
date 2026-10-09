#!/usr/bin/env python3
"""Explicit reversible user-session migration; never edit Android configuration."""
import datetime,os,pathlib,shutil,sys,xml.etree.ElementTree as ET
prefix=pathlib.Path(sys.argv[1]);home=pathlib.Path.home()
backup=home/'.local/state/linux-audio-routing'/('backup-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
backup.mkdir(parents=True)
files=[]
def save(file):
    relative=file.relative_to(home);files.append(str(relative))
    if file.exists():
        dest=backup/relative;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,dest)
    file.parent.mkdir(parents=True,exist_ok=True)
def block(file,text):
    current=file.read_text() if file.exists() else ''
    if '# BEGIN LINUX AUDIO' in current:return
    save(file);file.write_text(current+'\n# BEGIN LINUX AUDIO\n'+text+'\n# END LINUX AUDIO\n')
# Overrides an earlier legacy PULSE_SERVER export without removing it.
block(home/'.bashrc','export PIPEWIRE_RUNTIME_DIR=/tmp/linux-audio-$(id -u)\nexport PIPEWIRE_REMOTE=linux-audio\nexport PULSE_SERVER=unix:$PIPEWIRE_RUNTIME_DIR/pulse/native')
block(home/'.config/openbox/environment','export PIPEWIRE_RUNTIME_DIR=/tmp/linux-audio-$(id -u)\nexport PIPEWIRE_REMOTE=linux-audio\nexport PULSE_SERVER=unix:$PIPEWIRE_RUNTIME_DIR/pulse/native')
block(home/'.config/lxsession/LXDE/autostart',str(prefix/'bin/linux-audio')+' start')
rc=home/'.config/openbox/rc.xml'
if rc.exists():
    tree=ET.parse(rc);root=tree.getroot();ns=root.tag.split('}')[0]+'}' if '}' in root.tag else ''
    keyboard=root.find(ns+'keyboard')
    if keyboard is not None:
        changed=False
        for key,op in [('XF86AudioRaiseVolume','up'),('XF86AudioLowerVolume','down'),('XF86AudioMute','mute')]:
            existing=[b for b in keyboard if b.get('key')==key]
            if existing: continue # Preserve user bindings; report instead of replacing.
            b=ET.SubElement(keyboard,ns+'keybind',{'key':key});a=ET.SubElement(b,ns+'action',{'name':'Execute'});ET.SubElement(a,ns+'command').text=str(prefix/'bin/linux-audio')+' volume '+op;changed=True
        if changed:
            save(rc);ET.register_namespace('',ns[1:-1]) if ns else None;tree.write(rc,encoding='unicode',xml_declaration=True)
(backup/'files.txt').write_text('\n'.join(files)+'\n')
print('Session defaults enabled; restart the desktop when convenient. Backup:',backup)
print('Existing applications keep their old audio connection. Restore saved files to roll back.')
