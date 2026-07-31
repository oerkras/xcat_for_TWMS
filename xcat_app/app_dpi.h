#pragma once

#include <Windows.h>

// 96 DPI 设计稿 → 运行时物理像素
float AppDpi_Scale();
void  AppDpi_SetScale(float scale);
float AppDpi_Px(float designPx);

void AppDpi_Init(HWND hwnd);
void AppDpi_Refresh(HWND hwnd, bool reloadFont);
void AppDpi_ApplyStyle(float scale);
// 主题切换后调用：把当前 ImGui style（未缩放设计稿）重新抓为 base，避免旧色被冻住。
void AppDpi_RecaptureStyleBase();
