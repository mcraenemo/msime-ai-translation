export const DICTIONARY_PAGE_SIZE = 100;

export function createDictionaryPager(table: HTMLElement, load: (offset: number) => void) {
  let offset = 0;
  let hasMore = false;
  let pending = false;
  const footer = document.createElement('div');
  footer.className = 'dict-pagination';
  const previous = document.createElement('button');
  const next = document.createElement('button');
  const status = document.createElement('span');
  previous.type = next.type = 'button';
  previous.className = next.className = 'dict-button secondary';
  previous.textContent = '上一页'; next.textContent = '下一页';
  status.className = 'dict-pagination-status';
  status.setAttribute('aria-live', 'polite');
  // 状态文本在左、两个翻页按钮并排靠右；状态为空时按钮仍靠右（见 dict.css 的 margin-right: auto）。
  footer.append(status, previous, next); table.after(footer);
  const sync = () => {
    previous.disabled = pending || offset === 0;
    next.disabled = pending || !hasMore;
  };
  // 翻页后表格滚动条回到最上方：新一页的第一条应该是可见的，而不是停在上一页的滚动位置。
  // 结果是异步回来的，但此刻内容还没换，先置 0 不会被后面的 replaceChildren 顶回去。
  const turnTo = (target: number) => { table.scrollTop = 0; load(target); };
  previous.addEventListener('click', () => turnTo(Math.max(0, offset - DICTIONARY_PAGE_SIZE)));
  next.addEventListener('click', () => turnTo(offset + DICTIONARY_PAGE_SIZE));
  sync();
  return {
    get offset() { return offset; },
    loading() { pending = true; status.textContent = '查询中…'; sync(); },
    failed() { pending = false; status.textContent = '查询失败，请重试'; sync(); },
    reset() { offset = 0; hasMore = false; pending = false; status.textContent = ''; sync(); },
    update(start: number, count: number, more: boolean) {
      offset = start; hasMore = more; pending = false;
      status.textContent = count ? `第 ${start + 1}–${start + count} 条${more ? '，后面还有结果' : ''}` : '没有更多结果';
      sync();
    },
  };
}
