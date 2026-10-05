import argparse,base64,hashlib,json,struct,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('host');p.add_argument('manifest');a=p.parse_args()
key=json.loads(Path(a.manifest).read_text(encoding='utf-8'))['key']
extid=''.join(chr(97+int(x,16)) for x in hashlib.sha256(base64.b64decode(key)).hexdigest()[:32])
origin='chrome-extension://'+extid+'/'
request=json.dumps({'version':1,'command':'translate','text':'1234 ! ?'}).encode()
result=subprocess.run([a.host,origin],input=struct.pack('<I',len(request))+request,capture_output=True,timeout=10)
assert result.returncode==0 and len(result.stdout)>=4
size=struct.unpack('<I',result.stdout[:4])[0];assert size==len(result.stdout)-4
reply=json.loads(result.stdout[4:]);assert reply['ok'] is False and 'translation' not in reply
print('PASS installed native host + live local Server framing: invalid selection rejected')
result=subprocess.run([a.host,'chrome-extension://unauthorized/'],input=b'',capture_output=True,timeout=3)
assert result.returncode!=0 and not result.stdout
print('PASS unauthorized extension origin rejected')
result=subprocess.run([a.host,origin],input=struct.pack('<I',65537),capture_output=True,timeout=3)
assert result.returncode!=0 and not result.stdout
print('PASS oversized native message rejected before payload read')
