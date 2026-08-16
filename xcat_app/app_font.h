#pragma once

struct ImFont;

void AppFont_Load(float dpiScale = 1.f);
// 整数像素大号字（蓝字提示用）。未加载则为 nullptr，禁止用 SetWindowFontScale 冒充。
ImFont* AppFont_Hint();
