import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { setupCredentialTest } from './credential-test';

class TestElement extends EventTarget {
  textContent = '';
  disabled = false;
  dataset: Record<string, string> = {};
}

let button: TestElement;
let status: TestElement;
let postMessage: ReturnType<typeof vi.fn>;
let hostListener: ((event: Event & { data?: unknown }) => void) | undefined;

beforeEach(() => {
  button = new TestElement();
  button.textContent = '测试配置';
  status = new TestElement();
  postMessage = vi.fn();
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'testButton' ? button : status
  });
  vi.stubGlobal('window', {
    chrome: { webview: {
      postMessage,
      addEventListener: (_type: string, listener: (event: Event & { data?: unknown }) => void) => {
        hostListener = listener;
      }
    } }
  });
  vi.stubGlobal('MsimeProtocol', {
    version: 1,
    validate: () => true,
    serializeClientMessage: (message: unknown) => JSON.stringify(message)
  });
});

afterEach(() => vi.unstubAllGlobals());

it('posts current field values and renders the matching result', () => {
  setupCredentialTest(
    'testButton',
    'testStatus',
    () => 'ai.assistant',
    () => ({ token: 'current-token', model: 'current-model' })
  );
  button.dispatchEvent(new Event('click'));
  expect(button.disabled).toBe(true);
  expect(button.textContent).toBe('测试中…');
  const request = JSON.parse(postMessage.mock.calls[0]![0]);
  expect(request).toMatchObject({
    type: 'apiCredentialTest',
    data: { service: 'ai.assistant', config: { token: 'current-token', model: 'current-model' } }
  });

  hostListener?.({ data: {
    type: 'apiCredentialTestResult', requestId: request.data.requestId,
    ok: true, message: '连接成功'
  } } as Event & { data?: unknown });
  expect(button.disabled).toBe(false);
  expect(button.textContent).toBe('测试配置');
  expect(status.textContent).toBe('连接成功');
  expect(status.dataset.kind).toBe('success');
});
