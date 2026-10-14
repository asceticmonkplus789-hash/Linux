#!/usr/bin/env python3
"""Two-phase board verification. All test artifacts are retained, no cleanup."""
import fcntl, hashlib, json, os, signal, subprocess, sys, time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'sparseget'
DIR=ROOT/'demo/repeat-validation-20261013-v2'
URL='http://127.0.0.1:18766/file.bin'
SOURCE=ROOT/'demo/report-manual/source.bin'
LOG=[]

def run(*args, code=0):
    p=subprocess.run([str(BIN),*map(str,args)],capture_output=True,text=True)
    LOG.append('$ sparseget '+' '.join(map(str,args))+'\n'+p.stdout+p.stderr)
    assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
    return p.stdout

def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def state(p): return list(map(int,Path(str(p)+'.resume').read_text().splitlines()[1].split()))

def pause_and_complete(p):
    child=subprocess.Popen([str(BIN),'download',URL,str(p),'--delay-ms','100'],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        time.sleep(.65); child.send_signal(signal.SIGINT)
        out,err=child.communicate(timeout=10)
    except BaseException:
        child.kill(); child.wait(); raise
    LOG.append('$ download with SIGINT\n'+out+err)
    assert child.returncode==130,(child.returncode,out,err)
    n=state(p)[3]
    assert 0<n<SOURCE.stat().st_size,n
    resumed=run('download',URL,p)
    assert 'resume_offset='+str(n) in resumed,resumed
    assert digest(p)==digest(SOURCE)
    assert state(p)[3]==SOURCE.stat().st_size
    return n

def save():
    (DIR/('prepare.log' if sys.argv[1]=='prepare' else 'verify.log')).write_text('\n'.join(LOG))

if sys.argv[1]=='prepare':
    DIR.mkdir(exist_ok=False)
    f=DIR/'repeat.bin'
    off=pause_and_complete(f)
    before=(f.stat().st_ino,digest(f),state(f))
    run('reset',f)
    assert (f.stat().st_ino,digest(f),state(f))==before
    assert not list(DIR.glob('repeat.bin.reset-*'))
    (DIR/'prepare.json').write_text(json.dumps({'paused_offset':off,'sha256':digest(f)}))
    save(); print('PREPARE PASS: pause, resume, SHA-256, reset preview (no files moved)')
elif sys.argv[1]=='verify':
    f=DIR/'repeat.bin'
    oldino=f.stat().st_ino
    out=run('reset',f,'--confirm')
    backup=Path(out.split('backup_dir=')[1].splitlines()[0])
    assert not f.exists() and not Path(str(f)+'.resume').exists()
    assert (backup/'data.bin').stat().st_ino==oldino
    assert digest(backup/'data.bin')==digest(SOURCE)
    off=pause_and_complete(f)
    run('restore',f,backup,code=1)
    assert digest(f)==digest(SOURCE)
    second=run('reset',f,'--confirm')
    backup2=Path(second.split('backup_dir=')[1].splitlines()[0])
    assert backup!=backup2
    run('restore',f,backup)
    assert f.stat().st_ino==oldino
    assert digest(f)==digest(SOURCE)
    out=run('download',URL,f)
    assert 'resume_offset='+str(SOURCE.stat().st_size) in out
    assert (backup2/'data.bin').exists()
    # Refusal cases do not remove any files.
    run('reset',SOURCE,'--confirm',code=1)
    alias=DIR/'source-symlink.bin'; alias.symlink_to(SOURCE)
    run('reset',alias,'--confirm',code=1)
    with f.open('rb') as held:
        fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB)
        run('reset',f,'--confirm',code=1)
    corrupt=DIR/'corrupt.bin'
    run('download',URL,corrupt)
    with corrupt.open('r+b') as stream: stream.write(b'BAD')
    run('reset',corrupt,'--confirm',code=1)
    assert corrupt.exists()
    incomplete=DIR/'incomplete.bin'
    child=subprocess.Popen([str(BIN),'download',URL,str(incomplete),'--delay-ms','100'],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    time.sleep(.4); child.send_signal(signal.SIGINT)
    out,err=child.communicate(timeout=10)
    assert child.returncode==130,(out,err)
    run('reset',incomplete,'--confirm',code=1)
    assert incomplete.exists()
    save()
    result={'result':'PASS','repeat_cycles':2,'second_paused_offset':off,
            'sha256':digest(f),'backups':[str(backup),str(backup2)],
            'checks':['preview unchanged','completed pair retained','restart from zero',
                      'pause and resume again','SHA-256 match','unique backups',
                      'restore refuses overwrite','restore inode unchanged',
                      'restored completed state usable','missing state refused',
                      'symlink refused','locked target refused','modified prefix refused',
                      'unfinished download refused']}
    (DIR/'result.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
else: raise SystemExit('use prepare or verify')
