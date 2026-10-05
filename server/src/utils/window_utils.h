#pragma once

#include "defines/base_structures.h"
#include <memory>
#include <utility>
#include <windows.h>

FLOAT GetWindowScale(HWND);
FLOAT GetForegroundWindowScale();
// Prefer the monitor that contains the caret / composition anchor. Foreground
// HWND can disagree with the caret on Office extended-display setups.
FLOAT GetScaleForPoint(POINT pt);

// Where the candidate scale came from — diagnostic logging must be able to
// tell the RDP foreground path apart from the plain monitor path.
enum class CandidateScaleSource
{
    Monitor,       // caret's monitor DPI (local sessions, and RDP fallback)
    RdpForeground, // foreground window DPI inside an RDP session
};

struct ResolvedCandidateScale
{
    FLOAT scale = 0.0f;
    CandidateScaleSource source = CandidateScaleSource::Monitor;
};

// Candidate window scale authority. RDP syncs the client's display scaling
// into the session's monitor DPI metadata (150% client -> 144), while the
// focused application in the session usually still renders at 96 DPI, so the
// monitor metadata is unreliable there and GetDpiForWindow(foreground) is the
// best proxy for what the user actually sees:
//   - DPI-unaware / System Aware host -> virtualized 96, matches the app;
//   - PMv2 and correctly following -> current monitor DPI, matches the app;
//   - PMv2 but not following (pathological) -> mismatch, cannot be detected
//     via public APIs; documented as a known residue, not probed.
// Local sessions keep the caret-monitor convention (mixed-DPI multi-monitor
// setups depend on it). Falls back to the monitor path when the foreground
// window is gone (lock screen, focus switch) so behavior matches pre-fix.
ResolvedCandidateScale ResolveCandidateScaleForCaret(POINT caret);
// Candidate placement uses that monitor's work area to avoid taskbars/app bars.
MonitorCoordinates GetMonitorCoordinatesFromPoint(POINT pt);

MonitorCoordinates GetMonitorCoordinates();
MonitorCoordinates GetMainMonitorCoordinates();
int GetTaskbarHeight();

// Half of the target monitor in CSS DIPs (physical/2 / dpiScale). Single source
// of truth for FTB / menu / candidate max content size.
struct HalfScreenDipLimits
{
    FLOAT scale = 1.0f;
    double maxWidthDip = 0.0;
    double maxHeightDip = 0.0;
    MonitorCoordinates monitor{};
};
HalfScreenDipLimits QueryHalfScreenDipLimitsForHwnd(HWND hwnd);
HalfScreenDipLimits QueryHalfScreenDipLimitsForPoint(POINT pt);
double ClampWidthDipToHalfScreen(double widthDip, const HalfScreenDipLimits &limits);
double ClampHeightDipToHalfScreen(double heightDip, const HalfScreenDipLimits &limits);

int AdjustCandidateWindowPosition(        //
    const POINT *point,                   //
    const std::pair<double, double> &,    //
    std::shared_ptr<std::pair<int, int>>, //
    FLOAT layoutScale = 0.0f,             //
    double minWidthDip = 0.0              //
);

// Drop the "tallest list so far" flip memory. A bogus oversized measure would
// otherwise keep parking later cards at the top/left of the monitor.
void ResetCandidatePlacementMemory();

int AdjustWndPosition( //
    HWND hwnd,         //
    int crateX,        //
    int crateY,        //
    int width,         //
    int height,        //
    int properPos[2]   //
);
