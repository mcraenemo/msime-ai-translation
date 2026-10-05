#pragma once

#include "skin/candidate_skin_catalog.h"

#include <d2d1.h>
#include <string>
#include <windows.h>

struct CandidateSkinPalette
{
    D2D1_COLOR_F surface;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    // 候选文字与预编辑文字：皮肤的 candidate_text / preedit_text，不写时同 text；设置页的候选文字色三者一起覆盖。
    D2D1_COLOR_F candidateText{};
    D2D1_COLOR_F preeditText{};
};

// 基础皮肤的候选项高亮圆角，对应各皮肤 CSS 的 .cand border-radius；D2D 的 CandSkinTokens 用同一组值。
float CandidateSkinBaseItemRadiusDip(const std::string &baseSkinId);

D2D1_COLOR_F CandidateColorFromRgb(UINT rgb, float alpha = 1.0f);
D2D1_COLOR_F ParseCandidateCssColor(const std::string &text, D2D1_COLOR_F fallback);
CandidateSkinPalette ResolveCandidateSkinPalette(const std::string &skinId, bool light,
                                                 const std::string &configuredTextColor,
                                                 const CandidateSkinCatalog::CandidateColors *packageColors = nullptr,
                                                 const std::string &baseSkinId = {});
CandidateSkinPalette FlattenCandidateSkinPaletteForGdi(const CandidateSkinPalette &palette,
                                                       D2D1_COLOR_F fallbackSurface);
COLORREF FlattenCandidateColor(D2D1_COLOR_F color, D2D1_COLOR_F background);
