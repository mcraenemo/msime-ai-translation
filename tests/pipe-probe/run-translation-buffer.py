import json, os, re, shutil, subprocess, threading
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT=Path(__file__).resolve().parents[2]
stage=ROOT/'target/buffer-test-stage/user-local/metasequoiaime'
stage.mkdir(parents=True,exist_ok=True)
old=Path(os.environ.get('MSIME_PROBE_ASSET_DIR',str(ROOT/'target/translation-test-stage/user-local/metasequoiaime')))
if not (old/'msime.db').exists():raise SystemExit('Set MSIME_PROBE_ASSET_DIR to an IME dictionary fixture directory; only dictionaries are copied, never config or credentials.')
for name in ['msime.db','others.db','english.db','dict_pinyin.dat','dict_japanese.dat','pinyin.txt','helpcode.txt']:
    if (old/name).exists() and not (stage/name).exists():shutil.copy2(old/name,stage/name)
if not (stage/'helpcodes').exists():shutil.copytree(old/'helpcodes',stage/'helpcodes')
for name in ['ai-translations.db','ai-translations.db-wal','ai-translations.db-shm']:
    if (stage/name).exists():(stage/name).unlink()
class Handler(BaseHTTPRequestHandler):
    calls=0;failure_sent=False;sources=[]
    def log_message(self,*args):pass
    def do_GET(self):
        data=str(type(self).calls).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        source=json.loads(body['messages'][-1]['content'])['source_text']
        assert 'max_completion_tokens' in body and body['max_completion_tokens']>=4096
        assert 'max_tokens' not in body
        type(self).calls+=1;type(self).sources.append(source)
        if source=='你好' and not type(self).failure_sent:
            type(self).failure_sent=True;self.send_response(500);self.end_headers();self.wfile.write(b'{}');return
        translation='x'*450 if len(source)>200 else 'Translated sentence.'
        data=json.dumps({'choices':[{'finish_reason':'stop','message':{'content':json.dumps({'translation':translation})}}]}).encode()
        self.send_response(200);self.send_header('Content-Type','application/json');self.end_headers();self.wfile.write(data)
http=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=http.serve_forever,daemon=True).start()
config=(ROOT/'installer/default_config/config.default.toml').read_text(encoding='utf-8-sig')
start=config.index('[ai_assistant]');end=config.find('\n[',start+1);end=len(config) if end<0 else end
section=config[start:end]
changes={'translation_enabled':('false' if os.environ.get('MSIME_PROBE_NORMAL') else 'true'),'enabled':'false','translation_sync_enabled':'false','provider':'"openai"','token':'"MSIME_LOCAL_TEST"','token_openai':'"MSIME_LOCAL_TEST"','endpoint':f'"http://127.0.0.1:{http.server_port}/chat/completions"','endpoint_openai':f'"http://127.0.0.1:{http.server_port}/chat/completions"','model':'"gpt-6-luna"','model_openai':'"gpt-6-luna"'}
for k,v in changes.items():section=re.sub(r'(?m)^'+re.escape(k)+r'\s*=.*$',k+' = '+v,section)
config=config[:start]+section+config[end:]
config=re.sub(r'(?m)^cloud_candidates\s*=.*$','cloud_candidates = false',config)
config=re.sub(r'(?m)^candidate_translations\s*=.*$','candidate_translations = false',config)
if os.environ.get('MSIME_PROBE_TRAD'):
    config=re.sub(r'(?m)^character_set\s*=.*$','character_set = "traditional"',config)
config=re.sub(r'(?m)^default_ime_mode\s*=.*$','default_ime_mode = "chinese"',config)
(stage/'config.toml').write_text(config,encoding='utf-8')
env=os.environ.copy();env['METASEQUOIA_IME_DATA_DIR']=str(stage);env['METASEQUOIA_IME_CONFIG_DIR']=str(stage);env['LOCALAPPDATA']=str(stage.parent);env['MSIME_PROBE_HTTP_PORT']=str(http.server_port)
server=subprocess.Popen([str(ROOT/'server/build-release/bin/Release/MetasequoiaImeServer.exe'),'--watchdog-managed','--pipe-probe-isolated'],env=env,creationflags=subprocess.CREATE_NO_WINDOW)
try:
    probe=subprocess.run([str(ROOT/'target/buffer-probe/Release/msime-translation-buffer-probe.exe')],env=env,capture_output=True,text=True,timeout=90)
    print('Test server running:',server.poll() is None);print(probe.stdout);print(probe.stderr)
    print('Mock API count:',Handler.calls)
    if probe.returncode:print('Artificial probe source records:',json.dumps(Handler.sources,ensure_ascii=True))
    (ROOT/'target/buffer-test-result.json').write_text(json.dumps({'passed':probe.returncode==0,'calls':Handler.calls}),encoding='utf-8')
    raise SystemExit(probe.returncode)
finally:
    server.terminate();server.wait(timeout=10);http.shutdown()
