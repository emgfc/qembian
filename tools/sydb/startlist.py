"""The N95 Starter's dynamic startup list (C:\\private\\100059C9\\start.dat), host side.

Format (derived from Starter.exe, see docs/sydb.md "Autostart"): the file is DES-ECB encrypted
with Starter's fixed 8-byte key; the last block is padded with '0' characters, which Starter
strips when decrypting. The plaintext is a series of records
    <exe path>;<w0 recovery policy>;<w1 item type: 1, 7 or 8>;<w2>;<base64 MD5 of the exe>;}
Starter adds/removes records itself on StarterServer op 5/6 (what the installer's
SWInstStartupTask.dll calls); at boot, once the global state reaches 0xCB, it starts every record
whose exe still has the recorded MD5. This module writes the same format, for offline install
while QEMU is off (no StarterServer to ask).
"""
import base64, hashlib

KEY = bytes.fromhex('38a76766e5316673')        # Starter.exe literal (TLitC8 @0x83904810)
PATH = 'private/100059C9/start.dat'


def _des(encrypt, data):
    try:
        from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
    except ImportError:
        from cryptography.hazmat.primitives.ciphers.algorithms import TripleDES
    from cryptography.hazmat.primitives.ciphers import Cipher, modes
    c = Cipher(TripleDES(KEY * 3), modes.ECB())        # 3DES with K1=K2=K3 is single DES
    t = c.encryptor() if encrypt else c.decryptor()
    return t.update(data) + t.finalize()


def decrypt(data):
    txt = _des(False, data[:len(data) // 8 * 8]).decode('latin-1')
    return txt.rstrip('0')


def encrypt(txt):
    b = txt.encode('latin-1')
    b += b'0' * (-len(b) % 8)
    return _des(True, b)


def records(txt):
    out = []
    for rec in txt.split('}'):
        rec = rec.strip()
        if not rec:
            continue
        f = rec.rstrip(';').split(';')
        if len(f) >= 5:
            out.append({'path': f[0], 'recovery': int(f[1]), 'type': int(f[2]), 'w2': int(f[3]), 'md5': f[4]})
    return out


def render(recs):
    return ''.join('%s;%d;%d;%d;%s;}' % (r['path'], r['recovery'], r['type'], r['w2'], r['md5']) for r in recs)


def md5_b64(exe_bytes):
    return base64.b64encode(hashlib.md5(exe_bytes).digest()).decode()


def add(data, path, exe_bytes, recovery=0, typ=1, w2=0):
    recs = [r for r in records(decrypt(data)) if r['path'].lower() != path.lower()] if data else []
    recs.append({'path': path.lower(), 'recovery': recovery, 'type': typ, 'w2': w2, 'md5': md5_b64(exe_bytes)})
    return encrypt(render(recs))


def remove(data, path):
    recs = [r for r in records(decrypt(data)) if r['path'].lower() != path.lower()] if data else []
    return encrypt(render(recs)) if recs else None      # None: delete the file
