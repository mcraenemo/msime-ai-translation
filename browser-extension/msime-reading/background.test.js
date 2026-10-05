import {strict as assert} from 'node:assert';
let click, calls=0, finish;
globalThis.chrome={
 runtime:{onInstalled:{addListener(){}},sendNativeMessage(){calls++;return new Promise(resolve=>{finish=resolve;});}},
 contextMenus:{onClicked:{addListener(callback){click=callback;}}},
 scripting:{async executeScript(){return [{result:true}];}},
 action:{async setBadgeText(){},async setTitle(){}}
};
await import('./background.js');
assert.equal(calls,0);
await click({menuItemId:'translate',selectionText:'1234 ! ?'},{id:7});
assert.equal(calls,0);
const request=click({menuItemId:'translate',selectionText:'I have to work tomorrow.'},{id:7});
await new Promise(resolve=>setImmediate(resolve));assert.equal(calls,1);
await click({menuItemId:'translate',selectionText:'I have to work tomorrow.'},{id:7});assert.equal(calls,1);
finish({ok:true,translation:'我明天得上班。'});await request;
console.log('PASS explicit click only, invalid text zero calls, same-tab in-flight dedupe');
