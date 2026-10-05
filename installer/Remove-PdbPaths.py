"""Remove directory names from unsigned PE CodeView PDB locators; leave code intact.
Run after staging and before signing: python installer/Remove-PdbPaths.py installer/server_exe
PDBs themselves must not be distributed in the public runtime package.
"""
from pathlib import Path, PureWindowsPath
import struct,sys

def scrub(path):
    data=bytearray(path.read_bytes())
    if data[:2]!=b'MZ': return
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0': raise ValueError('invalid PE')
    sections=struct.unpack_from('<H',data,pe+6)[0]
    size=struct.unpack_from('<H',data,pe+20)[0]
    optional=pe+24
    magic=struct.unpack_from('<H',data,optional)[0]
    directory=optional+(112 if magic==0x20b else 96)
    cert_offset,cert_size=struct.unpack_from('<II',data,directory+4*8)
    debug_rva,debug_size=struct.unpack_from('<II',data,directory+6*8)
    def offset(rva):
        for i in range(sections):
            s=optional+size+i*40
            virtual_size,address,raw_size,raw_offset=struct.unpack_from('<IIII',data,s+8)
            if address<=rva<address+max(virtual_size,raw_size):return raw_offset+rva-address
        raise ValueError('unmapped debug RVA')
    changed=False
    if debug_rva:
        start=offset(debug_rva)
        for at in range(start,start+debug_size,28):
            kind,length,_,pointer=struct.unpack_from('<IIII',data,at+12)
            if kind!=2 or data[pointer:pointer+4]!=b'RSDS':continue
            old=bytes(data[pointer+24:pointer+length]).split(b'\0',1)[0]
            if b'\\' not in old and b'/' not in old:continue
            if cert_size:raise ValueError('refusing to modify signed binary')
            name=PureWindowsPath(old.decode('utf-8')).name.encode('utf-8')
            data[pointer+24:pointer+length]=name+b'\0'*(length-24-len(name))
            changed=True
    if changed:
        # Ordinary application PE files do not require a checksum; zero marks it absent.
        struct.pack_into('<I',data,optional+64,0)
        path.write_bytes(data)
        print('Removed PDB directory metadata:',path.name)
for arg in sys.argv[1:]:
    root=Path(arg)
    for path in root.rglob('*'):
        if path.is_file() and path.suffix.lower() in ('.exe','.dll'):scrub(path)
