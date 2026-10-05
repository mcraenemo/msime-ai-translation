# Candidate window templates

候选窗口的 DOM、测量脚本和交互脚本只维护两份：

- `horizontal_candidate_window.html`
- `vertical_candidate_window.html`

内置皮肤采用“一皮肤一文件夹”的结构：

```text
skins/
├─ fluent/
├─ wechat/
├─ graphite/
├─ willow_green/
├─ autumn_osmanthus/
└─ microsoft/
```

每个皮肤目录包含 `horizontal_dark.css`、`horizontal_light.css`、`vertical_dark.css` 和 `vertical_light.css`。Server 根据基础皮肤、候选框布局和明暗模式组合资源路径，并以内联 `<style>` 注入共享 HTML；启用外部皮肤时，Server 再根据其 `skin.toml` 生成一段 CSS，以内联 `<style>` 追加在后面，用于覆盖基础皮肤。

`ApplyCandidateFrame` 只隐藏用不到的候选行、不删除它们，所以 `:last-child` 不一定是最后一个可见行；需要定位尾项的皮肤用它维护的 `.row-wrapper.last-visible`。

翻页箭头 `.page-arrows` 不在 body 模板里：Server 每次用模板换掉容器内容后调用页面的 `SetCandidatePager(hasPrevious, hasNext)`，由它追加到 `#realContainer` 与 `#measureContainer` 末尾（测量容器调用时不带参数，只补元素不改状态），同时给最后一个候选行标上 `.last-visible`；竖排时按最后一个序号的位置算出左外边距，让“‹”与序号左对齐。设置页的预览用的是 `ui-html/webview2/settings/ime-settings/src/styles/modules/candidate/page-arrows.css` 里的同一套尺寸。是否显示由 Server 写在 `<html>` 上的 `data-page-arrows="on|off"` 决定，取值来自皮肤的 `page_arrows`（内置皮肤读数据目录 `skins/default/<id>/skin.toml`）。`microsoft` 皮肤在自己的 CSS 里把箭头改成实心三角、横排前加分隔线（D2D 端对应 `PagerArrows` 的 `Glyph::Triangle` 与 divider）。点击发出 `candidatePage` 消息，数据为 `previous` / `next`。样式在两份 HTML 里各有一份，改动要两边一起改。

外部皮肤使用 `skin.toml` 声明几何与候选配色，可选 `[toolbar]`、`[toolbar.dark]`、`[toolbar.light]` 覆盖悬浮工具栏的圆角与配色（D2D 与 WebView2 两个渲染器都生效）。不提供候选框 HTML、`cand.css` 或工具栏 CSS。

候选配色写在 `[candidate.dark]` / `[candidate.light]` 下，键都可省略：`accent`、`selected`、`hover`、`surface`、`border`、`text`、`number`、`translation` 和布尔值 `show_selected_bar`。`translation` 是候选后面的翻译文本：不写时沿用候选文字色并降到 62% 不透明度；写了就按原值绘制（选中行另写 `selected_translation` 时用后者），D2D 与 WebView2 两个后端一致。

细分配色 `candidate_text`、`preedit_text`、`preedit_caret`、`selected_text`、`selected_number`、`selected_translation`、`selected_bar`、`preedit_background`、`preedit_divider`，右键菜单 `[candidate.<theme>.menu]`，以及 `[candidate_window]` 下的 `border_width_dip`、`item_corner_radius_dip`、`shadow`、`font_family`，字段和回落规则见 [examples/skin-examples/schema/README.md](../../../examples/skin-examples/schema/README.md)。WebView2 一侧由 Server 的 `BuildExternalCandidateSkinCss` 生成 CSS，追加在 base 皮肤样式之后；用户设置的候选文字色通过 `--msime-user-text` 压过皮肤的 `candidate_text` / `preedit_text`。

```toml
[candidate.dark]
text = "#e9e8e8"
translation = "#e6a817"
```
