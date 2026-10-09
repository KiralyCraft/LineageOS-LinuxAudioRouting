#!/usr/bin/env python3
"""Reversible user-session integration; preserve unrelated desktop settings."""
import argparse
import datetime
import os
import pathlib
import re
import shlex
import shutil
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('prefix', type=pathlib.Path)
parser.add_argument('--remove-legacy-termux', action='store_true', help='Remove obsolete Termux Pulse exports from this user configuration')
args = parser.parse_args()
prefix = args.prefix.resolve()
home = pathlib.Path.home()
config = pathlib.Path(os.environ.get('XDG_CONFIG_HOME', str(home / '.config')))
runtime = pathlib.Path('/tmp') / ('linux-audio-' + str(os.getuid()))
environment = {
    'PIPEWIRE_RUNTIME_DIR': str(runtime),
    'PIPEWIRE_REMOTE': 'linux-audio',
    'PULSE_RUNTIME_PATH': str(runtime / 'pulse'),
    'PULSE_SERVER': 'unix:' + str(runtime / 'pulse/native'),
}
backup = None
files = []


def write(file, text):
    global backup
    if file.exists() and file.read_text() == text:
        return
    relative = file.relative_to(home)
    if backup is None:
        backup = home / '.local/state/linux-audio-routing' / ('backup-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        backup.mkdir(parents=True)
    if file.exists():
        destination = backup / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, destination)
    files.append(str(relative))
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text)


def read(file, fallback=None):
    if file.exists():
        return file.read_text()
    if fallback is not None and fallback.exists():
        return fallback.read_text()
    return ''


def block(file, text, prepend=False):
    current = read(file)
    current = re.sub(r'(?m)^# BEGIN LINUX AUDIO\n.*?^# END LINUX AUDIO\n?', '', current, flags=re.S)
    if args.remove_legacy_termux:
        current = ''.join(line for line in current.splitlines(keepends=True) if not re.match(r'^\s*#?\s*(?:export\s+)?PULSE_SERVER=', line) or not any(old in line for old in ('pulseAudio.socket', 'tcp:127.0.0.1:4713', 'unix:$XDG_RUNTIME_DIR/pulse/native')))
    if file.name in ['.bashrc', 'environment']:
        current = ''.join('# Previous audio setting: ' + line if any(re.match(r'^\s*(?:export\s+)?' + re.escape(key) + r'=', line) for key in environment) else line for line in current.splitlines(keepends=True))
    managed = '# BEGIN LINUX AUDIO\n' + text + '\n# END LINUX AUDIO\n'
    if prepend:
        write(file, managed + current)
    else:
        write(file, current.rstrip('\n') + '\n\n' + managed)


def section(text, name, entries):
    """Edit selected INI keys, retaining comments and all unrelated sections."""
    lines = text.splitlines(keepends=True)
    header = '[' + name + ']'
    start = next((index for index, line in enumerate(lines) if line.strip() == header), None)
    if start is None:
        return text.rstrip('\n') + '\n\n' + header + '\n' + ''.join(key + '=' + value + '\n' for key, value in entries.items())
    end = next((index for index in range(start + 1, len(lines)) if lines[index].lstrip().startswith('[')), len(lines))
    preserved = [line for line in lines[start + 1:end] if not any(re.match(r'^\s*' + re.escape(key) + r'\s*=', line) for key in entries)]
    updated = lines[:start + 1] + preserved + [key + '=' + value + '\n' for key, value in entries.items()] + lines[end:]
    return ''.join(updated)


exports = '\n'.join('export ' + key + '=' + shlex.quote(value) for key, value in environment.items())
# Before .bashrc's interactive early return, so login-shell desktop launchers
# also receive the endpoint. Keep DISPLAY, DBus and XDG_RUNTIME_DIR untouched.
block(home / '.bashrc', exports, prepend=True)
block(config / 'openbox/environment', exports)

# LXSession does not source Openbox's environment file. Its native export
# section accepts literal values, and runs before the panel/application launch.
desktop = config / 'lxsession/LXDE/desktop.conf'
write(desktop, section(read(desktop, pathlib.Path('/etc/xdg/lxsession/LXDE/desktop.conf')), 'Environment_variable', environment))

# The tray launcher starts the server, updates DBus audio activation variables,
# and then becomes pasystray. LXSession supervises only the tray process.
autostart = config / 'lxsession/LXDE/autostart'
current = read(autostart, pathlib.Path('/etc/xdg/lxsession/LXDE/autostart'))
current = re.sub(r'(?m)^# BEGIN LINUX AUDIO\n.*?^# END LINUX AUDIO\n?', '', current, flags=re.S)
current = ''.join(line for line in current.splitlines(keepends=True) if line.strip().lstrip('@') != 'pasystray')
write(autostart, current.rstrip('\n') + '\n\n# BEGIN LINUX AUDIO\n@' + str(prefix / 'bin/linux-audio') + ' tray\n# END LINUX AUDIO\n')

# Configuration fallbacks cover applications without inherited shell exports.
pulse = config / 'pulse/client.conf'
current = ''.join(line for line in read(pulse).splitlines(keepends=True) if not re.match(r'^\s*(default-server|autospawn)\s*=', line))
current = re.sub(r'(?m)^# BEGIN LINUX AUDIO\n.*?^# END LINUX AUDIO\n?', '', current, flags=re.S)
write(pulse, current.rstrip('\n') + '\n\n# BEGIN LINUX AUDIO\ndefault-server = ' + environment['PULSE_SERVER'] + '\nautospawn = no\n# END LINUX AUDIO\n')

socket = str(runtime / 'linux-audio')
write(config / 'pipewire/client.conf.d/99-linux-audio.conf', '# Linux Audio Routing: native client default; does not configure the server.\ncontext.properties = {\n    remote.name = "' + socket + '"\n}\n')
block(home / '.asoundrc', 'defaults.pipewire.server "' + socket + '"\npcm.!default {\n    type pipewire\n    server "' + socket + '"\n}\nctl.!default {\n    type pipewire\n    server "' + socket + '"\n}')

for name in ['lxde-rc.xml', 'rc.xml']:
    rc = config / 'openbox' / name
    if not rc.exists():
        continue
    tree = ET.parse(rc)
    root = tree.getroot()
    ns = root.tag.split('}')[0] + '}' if '}' in root.tag else ''
    keyboard = root.find(ns + 'keyboard')
    if keyboard is None:
        continue
    changed = False
    for key, operation in [('XF86AudioRaiseVolume', 'up'), ('XF86AudioLowerVolume', 'down'), ('XF86AudioMute', 'mute')]:
        if any(binding.get('key') == key for binding in keyboard):
            continue
        binding = ET.SubElement(keyboard, ns + 'keybind', {'key': key})
        action = ET.SubElement(binding, ns + 'action', {'name': 'Execute'})
        ET.SubElement(action, ns + 'command').text = str(prefix / 'bin/linux-audio') + ' volume ' + operation
        changed = True
    if changed:
        if ns:
            ET.register_namespace('', ns[1:-1])
        write(rc, ET.tostring(root, encoding='unicode') + '\n')

if backup is not None:
    (backup / 'files.txt').write_text('\n'.join(files) + '\n')
    print('Desktop audio enabled. Backup:', backup)
else:
    print('Desktop audio already configured; no files changed.')
print('Next LXDE session starts the tray automatically. Existing applications retain their current connection.')
