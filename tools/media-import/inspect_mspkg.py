#!/usr/bin/env python3
import argparse,struct,zlib
from pathlib import Path
HEADER=struct.Struct('<4sBBBBIIIIII'); TLV=struct.Struct('<HBBI'); SEQ=struct.Struct('<4sHHIIIHHI')
p=Path(argparse.ArgumentParser().parse_args().file) if False else None
ap=argparse.ArgumentParser(); ap.add_argument('file',type=Path); a=ap.parse_args(); data=a.file.read_bytes()
h=HEADER.unpack_from(data); magic,maj,minr,ctype,flags,hbytes,mbytes,pbytes,mcrc,pcrc,minfw=h
assert magic==b'MSPK' and hbytes==HEADER.size and len(data)==hbytes+mbytes+pbytes
meta=data[hbytes:hbytes+mbytes]; payload=data[hbytes+mbytes:]
assert zlib.crc32(meta)&0xffffffff==mcrc; assert zlib.crc32(payload)&0xffffffff==pcrc
print('container',maj,minr,'type',ctype,'metadata',mbytes,'payload',pbytes,'crc OK')
pos=0
while pos<len(meta):
    tag,typ,fl,n=TLV.unpack_from(meta,pos); pos+=TLV.size; value=meta[pos:pos+n]; pos+=n
    if typ==1: value=value.decode()
    elif typ==2: value=struct.unpack('<I',value)[0]
    elif typ==3: value=struct.unpack('<i',value)[0]
    elif typ==5: value=bool(value[0])
    elif tag==5: value=value.hex()
    print('tlv',tag,value)
print('sequence_header',SEQ.unpack_from(payload))
