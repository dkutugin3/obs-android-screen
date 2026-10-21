#include "placeholder.hpp"

#ifdef _WIN32
// GDI renderer: draws the same layout into a 24-bit DIB and copies the blue
// channel as the NV12 Y plane (all colors are grayscale).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

namespace placeholder {

static void drawTextLine(HDC dc, const std::string &textUtf8, HFONT font, COLORREF color, int x, int y,
	int w, bool alignRight)
{
	if (textUtf8.empty())
		return;
	int n = MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), -1, nullptr, 0);
	std::wstring wide((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), -1, &wide[0], n);

	SelectObject(dc, font);
	SetTextColor(dc, color);
	RECT rc{0, y, w, y + 1024};
	if (alignRight) {
		SIZE sz{};
		GetTextExtentPoint32W(dc, wide.c_str(), (int)wide.size() - 1, &sz);
		rc.left = x - sz.cx;
		rc.right = x;
	}
	DrawTextW(dc, wide.c_str(), (int)wide.size() - 1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
}

Frame render(const std::string &line1, const std::string &line2, uint32_t width, uint32_t height)
{
	Frame f;
	if (width < 2 || height < 2)
		return f;
	width &= ~1u;
	height &= ~1u;

	size_t ySize = (size_t)width * height;
	size_t uvSize = ySize / 2;
	f.width = width;
	f.height = height;
	f.data.assign(ySize + uvSize, 0);
	memset(f.data.data(), 0x10, ySize);
	memset(f.data.data() + ySize, 0x80, uvSize);

	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)width;
	bmi.bmiHeader.biHeight = -(LONG)height; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (!dc || !dib || !bits) {
		if (dib)
			DeleteObject(dib);
		if (dc)
			DeleteDC(dc);
		return f;
	}
	HGDIOBJ oldBmp = SelectObject(dc, dib);

	RECT all{0, 0, (LONG)width, (LONG)height};
	HBRUSH bg = CreateSolidBrush(RGB(0x10, 0x10, 0x10));
	FillRect(dc, &all, bg);
	DeleteObject(bg);

	SetBkMode(dc, TRANSPARENT);
	int margin = (int)(width * 0.08);
	int fontSize1 = (int)(height / 14.0f);
	int fontSize2 = (int)(height / 24.0f);
	int fontSize3 = (int)(height / 44.0f);

	HFONT f1 = CreateFontW(fontSize1, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
	HFONT f2 = CreateFontW(fontSize2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
	HFONT f3 = CreateFontW(fontSize3, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");

	int base1 = (int)(height * 0.32f);
	drawTextLine(dc, line1, f1, RGB(255, 255, 255), margin, base1, (int)width, false);
	drawTextLine(dc, line2, f2, RGB(255, 255, 255), margin, base1 + fontSize1 * 3 / 2, (int)width, false);
	drawTextLine(dc, "by Garay", f3, RGB(115, 115, 115), (int)width - margin, (int)height - margin - fontSize3 * 2,
		(int)width, true);

	// 24-bit rows are padded to 4 bytes.
	size_t rowBytes = ((width * 3 + 3) & ~(size_t)3);
	uint8_t *yPlane = f.data.data();
	for (uint32_t r = 0; r < height; r++) {
		const uint8_t *src = (const uint8_t *)bits + rowBytes * r;
		for (uint32_t c = 0; c < width; c++)
			yPlane[(size_t)r * width + c] = src[c * 3]; // B of BGR — grayscale anyway
	}

	SelectObject(dc, oldBmp);
	DeleteObject(f1);
	DeleteObject(f2);
	DeleteObject(f3);
	DeleteObject(dib);
	DeleteDC(dc);
	return f;
}

} // namespace placeholder

#else // macOS: CoreText renderer

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

namespace placeholder {

static CFStringRef makeCFString(const std::string &s)
{
	return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

static void drawLine(CGContextRef ctx, const std::string &text, const std::string &fontName, CGFloat fontSize,
	CGFloat x, CGFloat y, CGFloat gray = 1.0, bool alignRight = false)
{
	if (text.empty())
		return;
	CFStringRef cfText = makeCFString(text);
	CFStringRef cfFont = makeCFString(fontName);

	CTFontRef font = CTFontCreateWithName(cfFont, fontSize, nullptr);
	CGColorRef color = CGColorCreateGenericGray(gray, 1.0);
	CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
	CFTypeRef values[] = {font, color};
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, (const void **)keys, (const void **)values,
		2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFAttributedStringRef attrString =
		CFAttributedStringCreate(kCFAllocatorDefault, cfText, attrs);
	CTLineRef line = CTLineCreateWithAttributedString(attrString);

	if (alignRight)
		x -= (CGFloat)CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
	CGContextSetTextPosition(ctx, x, y);
	CTLineDraw(line, ctx);

	CFRelease(line);
	CFRelease(attrString);
	CFRelease(attrs);
	CGColorRelease(color);
	CFRelease(font);
	CFRelease(cfFont);
	CFRelease(cfText);
}

Frame render(const std::string &line1, const std::string &line2, uint32_t width, uint32_t height)
{
	Frame f;
	if (width < 2 || height < 2)
		return f;
	width &= ~1u;
	height &= ~1u;

	size_t ySize = (size_t)width * height;
	size_t uvSize = ySize / 2;
	f.width = width;
	f.height = height;
	f.data.assign(ySize + uvSize, 0);

	// Dark background, neutral chroma
	memset(f.data.data(), 0x10, ySize);
	memset(f.data.data() + ySize, 0x80, uvSize);

	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	CGContextRef ctx = CGBitmapContextCreate(f.data.data(), width, height, 8, width, gray,
		kCGImageAlphaNone | kCGBitmapByteOrderDefault);
	if (!ctx) {
		CGColorSpaceRelease(gray);
		return f;
	}

	CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorBlack));
	CGContextFillRect(ctx, CGRectMake(0, 0, width, height));

	CGFloat margin = (CGFloat)width * 0.08;
	CGFloat fontSize1 = (CGFloat)height / 14.0f;
	CGFloat fontSize2 = (CGFloat)height / 24.0f;

	// CoreText in a CGBitmap context renders upright glyphs as-is with a
	// bottom-left origin (y up). Do NOT flip the CTM: the bitmap device space
	// already matches the buffer top-down, and a flip mirrors the glyphs
	// (verified empirically — bug #7). Positions are converted "from the top".
	CGContextSetShouldAntialias(ctx, true);
	CGContextSetShouldSmoothFonts(ctx, false);

	CGFloat base1 = (CGFloat)height - margin - (CGFloat)height * 0.32f;
	drawLine(ctx, line1, "Helvetica-Bold", fontSize1, margin, base1);
	drawLine(ctx, line2, "Helvetica", fontSize2, margin, base1 - fontSize1 * 1.5f);

	// Small dim credit in the bottom-right corner (brand caption).
	drawLine(ctx, "by Garay", "Helvetica", (CGFloat)height / 44.0f,
		(CGFloat)width - margin, margin, 0.45f, true);

	CGContextRelease(ctx);
	CGColorSpaceRelease(gray);
	return f;
}

} // namespace placeholder

#endif // _WIN32
