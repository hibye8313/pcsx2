#pragma once
#include "stdafx.h"
#include "Renderers/Common/GSTexture.h"
//******************************
void SaveTexture(GSTexture* tex, const std::string& out_file);
void ReadPic(int i, int& width, int& height, std::vector<uint8>& data);
void UpdateTextureWindow();
void UpdateOtherWindow();
void WriteImageBits(std::string out_file_name, int width, int height, void* data);
void WriteImageBitsToBitmapRGB32(int width, int height, void* bits, const std::string& out_file_name);
void SaveMainScreenTexture(GSTexture* tex);
void SaveTextureTexture(GSTexture* tex);
void NextImage();

void DoDebugImages();
extern int image_count;
extern FILE* debug_out;

#define MY_PRINTF(s, ...) { fprintf(debug_out, "%d: %s: " s, image_count, __FUNCTION__, __VA_ARGS__); fflush(debug_out); }
#define FIX_X(x) (((x) - m_context->XYOFFSET.OFX) / 16.0f)
#define FIX_Y(y) (((y) - m_context->XYOFFSET.OFY) / 16.0f)
#define FIX_UV(u) ((u) / 16.0f)
//******************************