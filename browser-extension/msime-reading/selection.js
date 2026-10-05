export function validSelection(text) {
  return typeof text === 'string' && text.length <= 16384 &&
    new TextEncoder().encode(text).length <= 16384 && (text.match(/\p{L}/gu) || []).length >= 2;
}
export function showTranslation(source, translation, pending = false) {
  if (document.activeElement instanceof HTMLInputElement && document.activeElement.type === 'password') return false;
  const id = '__msimeReadingPopup';
  document.getElementById(id)?.remove();
  const host = document.createElement('div');
  host.id = id;
  const root = host.attachShadow({ mode: 'closed' });
  const style = document.createElement('style');
  style.textContent = ':host{all:initial;position:fixed;right:20px;bottom:20px;z-index:2147483647;font:15px/1.6 system-ui;color:#202124}section{width:min(420px,calc(100vw - 48px));max-height:65vh;overflow:auto;background:white;border:1px solid #ddd;border-radius:10px;padding:16px;box-shadow:0 4px 24px #0003}p{white-space:pre-wrap;overflow-wrap:anywhere;margin:8px 0}button{cursor:pointer;margin:8px 8px 0 0;padding:5px 10px}';
  const card = document.createElement('section');
  const title = document.createElement('strong'); title.textContent = '水杉阅读翻译';
  const original = document.createElement('p'); original.textContent = source.slice(0, 512);
  const result = document.createElement('p'); result.textContent = translation;
  const copy = document.createElement('button'); copy.textContent = '复制译文'; copy.disabled = pending;
  copy.onclick = () => navigator.clipboard.writeText(translation).then(() => { copy.textContent = '已复制'; }).catch(() => { copy.textContent = '复制失败'; });
  const close = document.createElement('button'); close.textContent = '关闭'; close.onclick = () => host.remove();
  card.append(title, original, result, copy, close); root.append(style, card);
  document.documentElement.append(host);
  return true;
}
