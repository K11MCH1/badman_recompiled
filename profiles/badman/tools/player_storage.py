"""Independent player storage preparation. Never manufactures a game save.

The game's existing private receipts/blobs and PSP-format files are copied as
opaque user data. Originals remain in place. Only an exclusively published,
owned Saves directory is selected; an existing foreign destination is preserved.
"""
from contextlib import contextmanager
from datetime import datetime,timezone
from pathlib import Path
import ctypes,hashlib,json,os,stat,tempfile,uuid

OWNER_NAME='.badman-saves-owner'
OWNER=b'PSPRecomp Badman player storage\nschema=1\n'
PROFILE_NAME='.badman-player.json'
RECEIPT_NAME='.badman-migration.json'
LEGACY_PARTS=('.badman-state','memory-stick')
NATIVE_OWNER=b'PSPRecomp Badman private native saves\nschema=1\nNot compatible with PSP or PPSSPP saves. Plaintext guest payload; not PSP secure encryption.\n'
MAX_FILES=20000;MAX_BYTES=2*1024*1024*1024
class StorageError(RuntimeError):pass
def present(path):
    try:path.lstat();return True
    except FileNotFoundError:return False
def native_running():
    if os.name!='nt':raise StorageError('The player storage process guard requires Windows')
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_=[('dwSize',w.DWORD),('cntUsage',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('name',w.WCHAR*260)]
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD];kernel.CreateToolhelp32Snapshot.restype=w.HANDLE
    kernel.Process32FirstW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)];kernel.Process32FirstW.restype=w.BOOL;kernel.Process32NextW.argtypes=kernel.Process32FirstW.argtypes;kernel.Process32NextW.restype=w.BOOL
    kernel.CloseHandle.argtypes=[w.HANDLE];kernel.CloseHandle.restype=w.BOOL
    handle=kernel.CreateToolhelp32Snapshot(2,0)
    if handle==ctypes.c_void_p(-1).value:raise StorageError('Cannot establish that legacy native game processes are closed')
    try:
        entry=Entry();entry.dwSize=ctypes.sizeof(entry);ok=kernel.Process32FirstW(handle,ctypes.byref(entry))
        while ok:
            if entry.name.casefold()=='badmannative.exe':return True
            ok=kernel.Process32NextW(handle,ctypes.byref(entry))
        if ctypes.get_last_error()!=18:raise StorageError('Cannot complete the legacy game process inventory; migration refused')
        return False
    finally:kernel.CloseHandle(handle)

def linked(path):
    s=path.lstat()
    return stat.S_ISLNK(s.st_mode)or bool(getattr(s,'st_file_attributes',0)&0x400)
def directory(path):
    if linked(path)or not path.is_dir():raise StorageError(f'Preserving linked or non-directory storage path: {path}')
def regular(path):
    s=path.lstat()
    if linked(path)or not stat.S_ISREG(s.st_mode)or s.st_nlink!=1:raise StorageError(f'Preserving linked/non-regular storage file: {path}')
    return s
def digest(path):
    with path.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
def safe_root(value):
    p=Path(value).expanduser().absolute()
    for ancestor in reversed((p,*p.parents)):
        if present(ancestor):directory(ancestor)
    if not p.exists():raise StorageError('Prepare game data before creating player storage')
    if p.resolve()!=p:raise StorageError('Storage root does not resolve to its exact path')
    return p
def inventory(root,ignore_lock=False):
    directory(root);files={};dirs=[];total=0
    pending=[root]
    while pending:
        parent=pending.pop()
        for p in sorted(parent.iterdir(),key=lambda x:x.name.casefold()):
            relative=p.relative_to(root).as_posix()
            if ignore_lock and relative=='BADMAN-NATIVE-V1/.commit-lock':continue
            if p.name=='.commit-lock':raise StorageError('Close the running game/save operation before migrating storage')
            if linked(p):raise StorageError(f'Preserving a linked storage entry: {p}')
            if p.is_dir():directory(p);dirs.append(relative);pending.append(p)
            else:
                s=regular(p);total+=s.st_size
                if total>MAX_BYTES or len(files)>=MAX_FILES:raise StorageError('Storage exceeds the bounded automatic migration budget; originals preserved')
                hash_value=digest(p);after=regular(p)
                identity=(s.st_size,s.st_mtime_ns,s.st_ctime_ns,s.st_ino,s.st_dev)
                if identity!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns,after.st_ino,after.st_dev):raise StorageError('Storage changed while it was inspected')
                files[relative]={'bytes':s.st_size,'sha256':hash_value,'mtime_ns':s.st_mtime_ns,'identity':identity}
    return {'directories':sorted(dirs),'files':files}
def write_new(path,data):
    with path.open('xb')as f:f.write(data);f.flush();os.fsync(f.fileno())
def json_bytes(value):return (json.dumps(value,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode('utf8')
def publish_directory(source,destination):
    # Windows MoveFileEx without REPLACE_EXISTING is an atomic no-overwrite
    # same-volume publication. No unsafe POSIX empty-directory replacement.
    if os.name!='nt':raise StorageError('Automatic player storage publication requires the Windows launcher')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.MoveFileExW.argtypes=[ctypes.c_wchar_p,ctypes.c_wchar_p,ctypes.c_uint32];kernel.MoveFileExW.restype=ctypes.c_int
    if not kernel.MoveFileExW(str(source),str(destination),8):raise StorageError(f'Cannot exclusively publish Saves (Windows error {ctypes.get_last_error()}); existing paths and staging evidence preserved')
@contextmanager
def exclusive_file(path):
    if os.name!='nt':raise StorageError('The native player storage lease requires Windows')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p];kernel.CreateFileW.restype=ctypes.c_void_p
    kernel.CloseHandle.argtypes=[ctypes.c_void_p];kernel.CloseHandle.restype=ctypes.c_int
    handle=kernel.CreateFileW(str(path),0xc0000000,0,None,1,0x04000100,None)
    if handle==ctypes.c_void_p(-1).value:raise StorageError(f'Player storage is busy or its lock is occupied: {path}; no existing files changed')
    try:yield
    finally:kernel.CloseHandle(handle)
def read_small(path,limit=32768):
    try:s=regular(path)
    except OSError as e:raise StorageError(f'Preserving player storage with unavailable metadata: {path}')from e
    if s.st_size>limit:raise StorageError(f'Preserving oversized player metadata: {path}')
    return path.read_bytes()
def existing(root):
    directory(root)
    if read_small(root/OWNER_NAME)!=OWNER:raise StorageError('Preserving an existing unrecognized Saves directory')
    try:profile=json.loads(read_small(root/PROFILE_NAME).decode('utf8'))
    except (ValueError,UnicodeError)as e:raise StorageError('Preserving invalid player profile metadata')from e
    if profile.get('producer')!='PSPRecomp Badman host profile'or profile.get('schema')!=1 or profile.get('profile_id')!='default' or not isinstance(profile.get('nickname'),str):raise StorageError('Preserving an unrecognized player profile')
    for part in (root/'PSP',root/'PSP/SAVEDATA'):directory(part)
    return profile
@contextmanager
def legacy_save_guard(legacy):
    store=legacy/'BADMAN-NATIVE-V1'
    if not present(store):yield False;return
    directory(store)
    if read_small(store/'OWNER.txt',4096)!=NATIVE_OWNER:raise StorageError('Preserving an unrecognized legacy native save store; no migration attempted')
    with exclusive_file(store/'.commit-lock'):yield True
def copy_snapshot(source,stage,before):
    for relative in before['directories']:(stage/relative).mkdir()
    for relative,info in before['files'].items():
        target=stage/relative
        with (source/relative).open('rb')as reader,target.open('xb')as writer:
            while chunk:=reader.read(1024*1024):writer.write(chunk)
            writer.flush();os.fsync(writer.fileno())
        if target.stat().st_size!=info['bytes']or digest(target)!=info['sha256']:raise StorageError('Copied save data failed byte verification; originals retained')
        os.utime(target,ns=((source/relative).stat().st_atime_ns,info['mtime_ns']))
def prepare_player_storage(data_root,*,verify_only=False,before_publish=None,active_game_check=None):
    root=safe_root(data_root);target=root/'Saves';legacy=root.joinpath(*LEGACY_PARTS)
    active_game_check=active_game_check or native_running
    if present(target):return {'saves':str(target),'profile':existing(target),'migration':'existing_owned_storage','read_only':verify_only}
    if verify_only:return {'saves':str(target),'migration':'not_prepared','read_only':True,'profile':None}
    if present(legacy)and active_game_check():raise StorageError('Close the running game before preparing or moving player saves')
    with exclusive_file(root/'.badman-player-session.lock'):
        if present(target):return {'saves':str(target),'profile':existing(target),'migration':'existing_owned_storage','read_only':False}
        has_legacy=present(legacy)
        for ancestor in (root/'.badman-state',legacy):
            if present(ancestor):directory(ancestor)
        guard=legacy_save_guard(legacy)if has_legacy else _empty_guard()
        with guard as ignore_lock:
            before=inventory(legacy,ignore_lock)if has_legacy else {'directories':[],'files':{}}
            if any(n in before['files']or n in before['directories']for n in (OWNER_NAME,PROFILE_NAME,RECEIPT_NAME)):raise StorageError('Legacy root contains reserved migration metadata; all data preserved')
            stage=Path(tempfile.mkdtemp(prefix='.badman-saves-stage-',dir=root))
            if has_legacy:copy_snapshot(legacy,stage,before)
            for part in (stage/'PSP',stage/'PSP/SAVEDATA'):
                if part.exists():directory(part)
                else:part.mkdir()
            created=datetime.now(timezone.utc).isoformat()
            profile={'producer':'PSPRecomp Badman host profile','schema':1,'profile_id':'default','nickname':'Player','preferred_native_slot':None,'created_at':created,
                'game_payload_created_by_setup':False}
            write_new(stage/OWNER_NAME,OWNER);write_new(stage/PROFILE_NAME,json_bytes(profile))
            receipt={'producer':'PSPRecomp Badman Saves migration','schema':1,'created_at':created,'legacy':str(Path(*LEGACY_PARTS))if has_legacy else None,
                'legacy_files':{name:{k:info[k]for k in ('bytes','sha256','mtime_ns')}for name,info in before['files'].items()},'source_originals_retained':True}
            write_new(stage/RECEIPT_NAME,json_bytes(receipt))
            staged_before=inventory(stage)
            if before_publish:before_publish(stage,target)
            if present(legacy)!=has_legacy:raise StorageError('Legacy storage appeared during preparation; originals and staging evidence preserved')
            if has_legacy and active_game_check():raise StorageError('A game started during storage preparation; copied staging evidence and originals preserved')
            if has_legacy and inventory(legacy,ignore_lock)!=before:raise StorageError('Legacy data changed before publication; copied staging evidence and originals preserved')
            if inventory(stage)!=staged_before:raise StorageError('Staged data changed before publication; copied evidence and source originals preserved')
            publish_directory(stage,target)
            existing(target)
            return {'saves':str(target),'profile':profile,'migration':'copied_legacy_originals_retained'if has_legacy else 'new_empty_storage','copied_files':len(before['files']),'read_only':False}
@contextmanager
def _empty_guard():yield False
