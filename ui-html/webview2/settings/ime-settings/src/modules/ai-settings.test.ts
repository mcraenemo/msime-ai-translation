import { afterEach, beforeEach, expect, it, vi } from 'vitest';

vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setupDropdownMenu: vi.fn(),
  setupToggleButton: vi.fn()
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn(), requestConfig: vi.fn() }));
vi.mock('./credential-test', () => ({ setupCredentialTest: vi.fn() }));

import { applyAiConfig, setupAiSettings } from './ai-settings';
import { updateConfig } from './config-sync';
import { setupToggleButton, applyToggleState } from './shared';

class StubElement {
  value = '';
  textContent = '';
  hidden = false;
  validity = '';
  setCustomValidity(value: string): void { this.validity = value; }
  reportValidity(): boolean { return this.validity === ''; }
  placeholder = '';
  listeners = new Map<string, (event: unknown) => void>();
  addEventListener(type: string, listener: (event: unknown) => void): void {
    this.listeners.set(type, listener);
  }
  select(value: string): void {
    this.listeners.get('click')?.({ target: { closest: () => ({ dataset: { value } }) } });
  }
}

let elements: Map<string, StubElement>;

beforeEach(() => {
  vi.clearAllMocks();
  elements = new Map(['aiToken', 'aiEndpoint', 'aiModel', 'aiProviderMenu', 'aiTranslationTarget',
    'aiTranslationClearCache', 'aiTranslationHotkey', 'aiTranslationSyncNow', 'aiTranslationSyncImport',
    'aiTranslationSyncAuthorize', 'aiTranslationSyncLastTime', 'aiTranslationSyncStatus',
    'aiTranslationCustomLanguage', 'aiTranslationCustomLanguageField', 'aiReadingTarget', 'aiReadingCustomLanguage', 'aiReadingCustomLanguageField'].map(id => [id, new StubElement()]));
  vi.stubGlobal('document', { getElementById: (id: string) => elements.get(id) ?? null });
  setupAiSettings();
});

afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers(); });

it('queues Google sync actions without changing existing API fields and shows status', () => {
  vi.useFakeTimers();
  elements.get('aiTranslationSyncNow')!.listeners.get('click')?.({});
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('ai_assistant.translation_sync_now', true);
  applyAiConfig({ translation_sync_enabled: true, translation_sync_last_time: '2026-10-04 12:00:00', translation_sync_status: '同步成功' });
  expect(elements.get('aiTranslationSyncLastTime')!.textContent).toBe('2026-10-04 12:00:00');
  expect(elements.get('aiTranslationSyncStatus')!.textContent).toBe('同步成功');
  expect(applyToggleState).toHaveBeenCalledWith('aiTranslationSyncEnabled', true);
  vi.advanceTimersByTime(180000);
  expect(vi.getTimerCount()).toBe(0);
});

it('keeps translation independent from Chinese AI candidates and restores its settings without writes', () => {
  applyAiConfig({ enabled: false, translation_enabled: true, translation_target_language: 'tl', translation_hotkey: 'Alt+F8' });
  expect(applyToggleState).toHaveBeenCalledWith('aiEnabled', false);
  expect(applyToggleState).toHaveBeenCalledWith('aiTranslationEnabled', true);
  expect(elements.get('aiTranslationTarget')!.value).toBe('tl');
  expect(elements.get('aiTranslationHotkey')!.value).toBe('Alt+F8');
  expect(updateConfig).not.toHaveBeenCalled();
  const toggle = vi.mocked(setupToggleButton).mock.calls.find(call => call[0] === 'aiTranslationEnabled')!;
  toggle[1]!(false);
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('ai_assistant.translation_enabled', false);
});

it('saves and restores languages beyond the original six without changing API configuration', () => {
  const target = elements.get('aiTranslationTarget')!;
  target.value = 'es'; target.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.translation_target_language', 'es');
  vi.mocked(updateConfig).mockClear();
  target.value = 'custom'; target.listeners.get('change')?.({});
  expect(elements.get('aiTranslationCustomLanguageField')!.hidden).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  const input = elements.get('aiTranslationCustomLanguage')!;
  input.value = '粤语'; input.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('ai_assistant.translation_target_language', '粤语');
  vi.mocked(updateConfig).mockClear();
  applyAiConfig({ translation_target_language: '粤语' });
  expect(target.value).toBe('custom');
  expect(input.value).toBe('粤语');
  expect(updateConfig).not.toHaveBeenCalled();
  input.value = ' '; input.listeners.get('change')?.({});
  expect(input.validity).not.toBe('');
  expect(updateConfig).not.toHaveBeenCalled();
});

it('changes only the translation language and clears only the local translation cache', () => {
  const target = elements.get('aiTranslationTarget')!;
  target.value = 'zh-Hant'; target.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.translation_target_language', 'zh-Hant');
  elements.get('aiTranslationClearCache')!.listeners.get('click')?.({});
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.translation_clear_cache', true);
  expect(vi.mocked(updateConfig).mock.calls.every(call => call[0].startsWith('ai_assistant.translation_'))).toBe(true);
});

it('switches custom endpoints, models and tokens together and restores them on return', () => {
  applyAiConfig({
    provider: 'deepseek', token: 'test-deepseek',
    endpoint: 'https://deepseek.example.test/chat/completions', model: 'custom-deepseek',
    tokens: { deepseek: 'test-deepseek', openai: 'test-openai' },
    endpoints: { deepseek: 'https://deepseek.example.test/chat/completions', openai: 'https://openai.example.test/v1/chat/completions' },
    models: { deepseek: 'custom-deepseek', openai: 'custom-openai' }
  });

  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiToken')!.value).toBe('test-openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://openai.example.test/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-openai');
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.provider', 'openai');

  elements.get('aiProviderMenu')!.select('deepseek');
  expect(elements.get('aiToken')!.value).toBe('test-deepseek');
  expect(elements.get('aiEndpoint')!.value).toBe('https://deepseek.example.test/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-deepseek');
});

it('keeps legacy custom values with their provider and uses defaults for an unconfigured provider', () => {
  applyAiConfig({
    provider: 'deepseek', endpoint: 'https://legacy.example.test/chat/completions', model: 'legacy-model'
  });
  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://api.openai.com/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('gpt-4o-mini');
  elements.get('aiProviderMenu')!.select('deepseek');
  expect(elements.get('aiEndpoint')!.value).toBe('https://legacy.example.test/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('legacy-model');
});

it('treats empty endpoints and models as provider defaults', () => {
  applyAiConfig({
    provider: 'deepseek', endpoint: '', model: '',
    endpoints: { deepseek: '', openai: '' }, models: { deepseek: '', openai: '' }
  });
  elements.get('aiProviderMenu')!.select('openai');
  expect(elements.get('aiEndpoint')!.value).toBe('https://api.openai.com/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('gpt-4o-mini');

  const endpoint = elements.get('aiEndpoint')!;
  endpoint.value = '';
  endpoint.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledWith('ai_assistant.endpoint', '');
  expect(endpoint.value).toBe('https://api.openai.com/v1/chat/completions');

  endpoint.value = '';
  elements.get('aiProviderMenu')!.select('deepseek');
  elements.get('aiProviderMenu')!.select('openai');
  expect(endpoint.value).toBe('https://api.openai.com/v1/chat/completions');
});

it('reloads provider-specific values from a new config snapshot', () => {
  applyAiConfig({
    provider: 'openai', endpoint: 'https://openai.example.test/v1/chat/completions', model: 'custom-openai',
    endpoints: { openai: 'https://openai.example.test/v1/chat/completions', groq: 'https://groq.example.test/v1/chat/completions' },
    models: { openai: 'custom-openai', groq: 'custom-groq' }
  });
  elements.get('aiProviderMenu')!.select('groq');
  expect(elements.get('aiEndpoint')!.value).toBe('https://groq.example.test/v1/chat/completions');
  expect(elements.get('aiModel')!.value).toBe('custom-groq');
});

it('keeps reading and sending target languages independent', () => {
  applyAiConfig({ translation_target_language: 'en', reading_target_language: 'zh-Hans' });
  expect(elements.get('aiReadingTarget')!.value).toBe('zh-Hans');
  expect(elements.get('aiTranslationTarget')!.value).toBe('en');
  expect(updateConfig).not.toHaveBeenCalled();
  const reading = elements.get('aiReadingTarget')!;
  reading.value = 'tl'; reading.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('ai_assistant.reading_target_language', 'tl');
  vi.mocked(updateConfig).mockClear();
  reading.value = 'custom'; reading.listeners.get('change')?.({});
  const input = elements.get('aiReadingCustomLanguage')!;
  input.value = '粤语'; input.listeners.get('change')?.({});
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('ai_assistant.reading_target_language', '粤语');
  expect(elements.get('aiTranslationTarget')!.value).toBe('en');
});
