import {validSelection, showTranslation} from './selection.js';
const active = new Map();
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => chrome.contextMenus.create({id:'translate',title:'翻译选中文字（水杉）',contexts:['selection']}));
});
async function render(tabId, frameId, source, translated, pending = false) {
  const result = await chrome.scripting.executeScript({target:{tabId,frameIds:[frameId]},func:showTranslation,args:[source,translated,pending]});
  return result[0]?.result === true;
}
chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if(info.menuItemId !== 'translate' || !tab?.id || !validSelection(info.selectionText)) return;
  const source = info.selectionText, frameId = info.frameId || 0;
  if(active.has(tab.id)) return; // one explicit in-flight request per tab
  const job = {};
  active.set(tab.id,job);
  try {
    if(!await render(tab.id,frameId,source,'正在翻译…',true)) return;
    const response = await chrome.runtime.sendNativeMessage('org.metasequoiaime.reading',{version:1,command:'translate',text:source});
    await render(tab.id,frameId,source,response.ok ? response.translation : response.error || '翻译失败。');
    await chrome.action.setBadgeText({tabId:tab.id,text:''});
  } catch(error) {
    const message = '无法连接或显示翻译。请确认本机桥接已安装、MSIME 正在运行，并处于简译/繁译模式。';
    await render(tab.id,frameId,source,message).catch(()=>{});
    await chrome.action.setBadgeText({tabId:tab.id,text:'!'}).catch(()=>{});
    await chrome.action.setTitle({tabId:tab.id,title:message}).catch(()=>{});
  } finally { if(active.get(tab.id) === job) active.delete(tab.id); }
});
