import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyThemeConfig, reapplyThemeConfig } from './theme';

class FakeImage {
  src = '/assets/softkbd.png';
}

function fakeElement() {
  return {
    style: {} as Record<string, string>,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() }
  };
}

let previewLoaded = false;
let image: FakeImage;

beforeEach(() => {
  previewLoaded = false;
  image = new FakeImage();
  vi.stubGlobal('HTMLImageElement', FakeImage);
  vi.stubGlobal('window', { matchMedia: () => ({ matches: true, onchange: null }) });
  vi.stubGlobal('document', {
    documentElement: fakeElement(),
    body: fakeElement(),
    querySelector: (selector: string) =>
      selector === '.screenkb-preview' && previewLoaded ? fakeElement() : null,
    querySelectorAll: () => [],
    getElementById: (id: string) => (id === 'screenKeyboardPreviewImage' && previewLoaded ? image : null)
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

it('themes a preview page that loads after the snapshot was applied', async () => {
  applyThemeConfig({ theme_mode: 'system', theme_screen_keyboard: 'follow' });
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd.png');

  previewLoaded = true;
  reapplyThemeConfig();
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd_light.png');
});
