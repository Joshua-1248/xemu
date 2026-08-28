// xemu custom fork - TAS / TAStudio frontend boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_TAS
bool TasWindowsOpen();
void ShowTasWindows();
void DrawTasMenu();
void TasNotifySnapshotCreated();
void FeatureTasDrawGeneralSettings();
#else
static inline bool TasWindowsOpen() { return false; }
static inline void ShowTasWindows() {}
static inline void DrawTasMenu() {}
static inline void TasNotifySnapshotCreated() {}
static inline void FeatureTasDrawGeneralSettings() {}
#endif
