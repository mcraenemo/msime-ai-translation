import { strict as assert } from 'node:assert';
import { validSelection } from './selection.js';
assert(validSelection('I have to work tomorrow.'));
assert(validSelection('Kailangan kong magtrabaho bukas.'));
assert(validSelection('明日は仕事です。'));
for(const text of ['', ' ', '1234', '?!', 'a', 'x'.repeat(16385)]) assert.equal(validSelection(text), false);
assert.equal(validSelection('中'.repeat(6000)), false);
console.log('PASS browser selection validation (letters, noise, byte limit)');
