// #content-container 上有 contain: layout paint，它因此成了 position: fixed 的包含块：
// 写在页面片段里的遮罩和提示条留在它内部就等同于绝对定位，会跟着内容一起滚——内容区滚下去
// 之后，弹窗离窗口底边的距离凭空多出一个 scrollTop。挂到 <body> 下，fixed 才真的相对窗口。
export function hoistOverlay(element: HTMLElement | null): void {
  if (element && element.parentElement !== document.body) document.body.appendChild(element);
}

// 代价是这些浮层不再随所属模块容器一起隐藏，切换页面时得自己收起。
export function dismissHoistedOverlays(): void {
  document.querySelectorAll<HTMLElement>('body > .dict-toast.visible')
    .forEach((toast) => toast.classList.remove('visible'));
  document.querySelectorAll<HTMLElement>('body > .dict-modal.open').forEach((modal) => {
    modal.classList.remove('open');
    modal.setAttribute('aria-hidden', 'true');
  });
}
