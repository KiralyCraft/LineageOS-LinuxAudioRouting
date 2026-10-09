#!/usr/bin/env python3
"""Configuration migration and rollback evidence using a disposable home only."""
import configparser,os,pathlib,subprocess,sys,tempfile,xml.etree.ElementTree as ET
source=pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='audio-desktop-test-') as directory:
    root=pathlib.Path(directory)
    (root/'.config/openbox').mkdir(parents=True)
    bash=root/'.bashrc';bash.write_text('[[ $- != *i* ]] && return\nexport PULSE_SERVER=unix:/original/audio\n')
    lxde=root/'.config/lxsession/LXDE';lxde.mkdir(parents=True)
    desktop=lxde/'desktop.conf';desktop.write_text('[GTK]\nsNet/ThemeName=UserTheme\n[Environment_variable]\nUSER_SETTING=retained\nPULSE_SERVER=unix:/old\n')
    autostart=lxde/'autostart';autostart.write_text('@lxpanel --profile LXDE\n@pcmanfm --desktop --profile LXDE\n@pasystray\n# BEGIN LINUX AUDIO\nold/private/bin/linux-audio start\n# END LINUX AUDIO\n')
    rc=root/'.config/openbox/rc.xml';original='<openbox_config xmlns="http://openbox.org/3.4/rc"><keyboard><keybind key="XF86AudioMute"><action name="Execute"><command>user-mute</command></action></keybind></keyboard></openbox_config>';rc.write_text(original)
    lxde_rc=root/'.config/openbox/lxde-rc.xml';lxde_rc.write_text(original)
    env=os.environ.copy();env['HOME']=directory;env['XDG_CONFIG_HOME']=str(root/'.config')
    prefix=root/'private'
    script=source/'linux/configure-desktop.py'
    subprocess.run([sys.executable,str(script),str(prefix)],env=env,check=True,stdout=subprocess.DEVNULL)
    assert 'export PULSE_SERVER=unix:/original/audio' in bash.read_text()
    assert bash.read_text().count('# BEGIN LINUX AUDIO')==1
    tree=ET.parse(rc);ns={'o':'http://openbox.org/3.4/rc'}
    bindings=tree.findall('.//o:keybind',ns)
    assert len(bindings)==3
    assert tree.find('.//o:keybind[@key="XF86AudioMute"]//o:command',ns).text=='user-mute'
    assert len(ET.parse(lxde_rc).findall('.//o:keybind',ns))==3
    parsed=configparser.ConfigParser();parsed.read(desktop)
    endpoint='unix:/tmp/linux-audio-'+str(os.getuid())+'/pulse/native'
    assert parsed['GTK']['sNet/ThemeName']=='UserTheme'
    assert parsed['Environment_variable']['USER_SETTING']=='retained'
    assert parsed['Environment_variable']['PULSE_SERVER']==endpoint
    assert '@lxpanel --profile LXDE' in autostart.read_text()
    assert '@pcmanfm --desktop --profile LXDE' in autostart.read_text()
    assert autostart.read_text().count('linux-audio tray')==1 and '@pasystray' not in autostart.read_text()
    shell_env=env.copy();shell_env['PULSE_SERVER']='unix:/inherited/stale';shell_env['DISPLAY']=':protected';shell_env['XDG_RUNTIME_DIR']='/tmp/protected';shell_env['DBUS_SESSION_BUS_ADDRESS']='unix:path=/tmp/protected/bus'
    actual=subprocess.check_output(['bash','--noprofile','--norc','-c','source "$HOME/.bashrc"; printf "%s|%s|%s|%s" "$PULSE_SERVER" "$DISPLAY" "$XDG_RUNTIME_DIR" "$DBUS_SESSION_BUS_ADDRESS"'],env=shell_env,text=True)
    assert actual==endpoint+'|:protected|/tmp/protected|unix:path=/tmp/protected/bus'
    snapshot=rc.read_bytes();subprocess.run([sys.executable,str(script),str(prefix)],env=env,check=True,stdout=subprocess.DEVNULL)
    assert rc.read_bytes()==snapshot and bash.read_text().count('# BEGIN LINUX AUDIO')==1
    backups=sorted((root/'.local/state/linux-audio-routing').glob('backup-*'))
    assert len(backups)==1
    assert (backups[0]/'.bashrc').read_text()=='[[ $- != *i* ]] && return\nexport PULSE_SERVER=unix:/original/audio\n'
    assert (backups[0]/'.config/openbox/rc.xml').read_text()==original
    with bash.open('a') as f:f.write('export PULSE_SERVER=unix:/hostMounts/chrootBind/pulseAudio.socket\n')
    subprocess.run([sys.executable,str(script),str(prefix),'--remove-legacy-termux'],env=env,check=True,stdout=subprocess.DEVNULL)
    assert 'pulseAudio.socket' not in bash.read_text()
print('PASS reversible migration, LXSession export, noninteractive shell inheritance, panel autostart, both Openbox profiles, stale Termux cleanup, idempotence')
