#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <limits>
#include <cstddef>
#include <new>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

constexpr size_t HEADER_SIZE = 16;
constexpr int DEFAULT_WIDTH = 576;
constexpr int MIN_WIDTH = 1;
constexpr int MAX_SLIDER_WIDTH = 1200;
constexpr UINT UPDATE_TIMER_ID = 1;
constexpr UINT UPDATE_DELAY_MS = 150;

constexpr int PADDING = 10;
constexpr int CTRL_HEIGHT = 23;
constexpr int BTN_WIDTH = 120;
constexpr int SHORT_LBL_WIDTH = 40;
constexpr int EDIT_WIDTH = 50;
constexpr int CHK_WIDTH = 100;
constexpr int LBL_VERT_OFFSET = 5;


const std::vector<std::byte> SEQUENCE_TO_REMOVE1 = {
    std::byte{0x1B}, std::byte{0x4A}, std::byte{0x18}, std::byte{0x1D},
    std::byte{0x76}, std::byte{0x30}, std::byte{0x00}, std::byte{0x48},
    std::byte{0x00}, std::byte{0x18}, std::byte{0x00}
};
const std::vector<std::byte> SEQUENCE_TO_REMOVE2 = {
    std::byte{0x1B}, std::byte{0x4A}, std::byte{0x18}, std::byte{0x1D},
    std::byte{0x76}, std::byte{0x30}, std::byte{0x00}, std::byte{0x48},
    std::byte{0x00}, std::byte{0x10}, std::byte{0x00}
};


int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::unique_ptr<Gdiplus::ImageCodecInfo[]> pImageCodecInfo(new Gdiplus::ImageCodecInfo[size]);
    if (!pImageCodecInfo) return -1;

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo.get());
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

struct ProcessedBitmapResult {
    std::shared_ptr<Gdiplus::Bitmap> image = nullptr;
    int actual_width = 0;
    int actual_height = 0;
    size_t bytes_removed_count = 0;
    std::wstring error_message;
};

ProcessedBitmapResult load_and_process_bitmap(
    const std::wstring& filename,
    int width,
    bool msb_first = true,
    bool invert_polarity = false)
{
    ProcessedBitmapResult result;
    result.actual_width = width;

    if (filename.empty() || !std::filesystem::exists(filename)) {
        result.error_message = L"File not found or not specified.";
        return result;
    }
    if (width <= 0) {
        result.error_message = L"Width must be positive.";
        return result;
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        result.error_message = L"Failed to open file.";
        return result;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < HEADER_SIZE) {
        result.error_message = L"File is smaller than header size.";
        return result;
    }
    std::vector<char> header_buffer(HEADER_SIZE);
    if (!file.read(header_buffer.data(), HEADER_SIZE)) {
        result.error_message = L"Failed to read header.";
        return result;
    }

    std::streamsize pixel_data_size = size - HEADER_SIZE;
    std::vector<std::byte> pixel_data;
    if (pixel_data_size > 0) {
        try {
             pixel_data.resize(pixel_data_size);
        } catch (const std::bad_alloc&) {
             result.error_message = L"Failed to allocate memory for pixel data.";
             return result;
        }
        if (!file.read(reinterpret_cast<char*>(pixel_data.data()), pixel_data_size)) {
            pixel_data.resize(file.gcount());
            OutputDebugStringW(L"Warning: Partial pixel data read.\n");
        }
    }
    file.close();

    if (pixel_data.empty() && pixel_data_size > 0) {
         result.error_message = L"Failed to read pixel data (vector empty after read).";
         return result;
    }

    size_t removed_in_pass1 = 0;
    size_t removed_in_pass2 = 0;
    if (!SEQUENCE_TO_REMOVE1.empty() && !pixel_data.empty()) {
        auto it = pixel_data.begin();
        while ((it = std::search(it, pixel_data.end(), SEQUENCE_TO_REMOVE1.begin(), SEQUENCE_TO_REMOVE1.end())) != pixel_data.end()) {
            it = pixel_data.erase(it, it + SEQUENCE_TO_REMOVE1.size());
            removed_in_pass1 += SEQUENCE_TO_REMOVE1.size();
        }
    }
    if (!SEQUENCE_TO_REMOVE2.empty() && !pixel_data.empty()) {
        auto it = pixel_data.begin();
        while ((it = std::search(it, pixel_data.end(), SEQUENCE_TO_REMOVE2.begin(), SEQUENCE_TO_REMOVE2.end())) != pixel_data.end()) {
            it = pixel_data.erase(it, it + SEQUENCE_TO_REMOVE2.size());
             removed_in_pass2 += SEQUENCE_TO_REMOVE2.size();
        }
    }
    result.bytes_removed_count = removed_in_pass1 + removed_in_pass2;
    if (result.bytes_removed_count > 0) {
        std::wstringstream ss;
        ss << L"Debug: Removed " << result.bytes_removed_count << L" bytes matching sequences.\n";
        OutputDebugStringW(ss.str().c_str());
    }

    if (pixel_data.empty()) {
        result.error_message = L"No pixel data remaining after sequence removal.";
        return result;
    }

    std::vector<UINT32> pixels;
    try {
         pixels.reserve(pixel_data.size() * 8);
    } catch (const std::bad_alloc&) {
        result.error_message = L"Failed to allocate memory for pixel array.";
        return result;
    }

    UINT64 total_bits = 0;
    const UINT32 black = 0xFF000000;
    const UINT32 white = 0xFFFFFFFF;

    for (std::byte byte_val : pixel_data) {
        unsigned char current_byte = static_cast<unsigned char>(byte_val);
        if (msb_first) {
            for (int i = 7; i >= 0; --i) {
                bool bit = (current_byte >> i) & 1;
                UINT32 pixel_value = (bit == 1) ? (invert_polarity ? white : black) : (invert_polarity ? black : white);
                pixels.push_back(pixel_value);
                total_bits++;
            }
        } else {
            for (int i = 0; i < 8; ++i) {
                bool bit = (current_byte >> i) & 1;
                 UINT32 pixel_value = (bit == 1) ? (invert_polarity ? white : black) : (invert_polarity ? black : white);
                pixels.push_back(pixel_value);
                total_bits++;
            }
        }
    }

    if (width == 0) {
        result.error_message = L"Internal Error: Width became zero.";
        return result;
    }
    if (total_bits == 0) {
         result.error_message = L"No bits found in processed data.";
         return result;
    }

    result.actual_height = static_cast<int>(total_bits / width);
    if (result.actual_height == 0 && total_bits > 0) {
        result.actual_height = 1;
    }

    size_t num_pixels_to_use = static_cast<size_t>(width) * result.actual_height;

    if (num_pixels_to_use == 0) {
         result.error_message = L"Calculated image size is zero. Try adjusting width.";
         return result;
    }

    if (num_pixels_to_use > pixels.size()) {
         OutputDebugStringW(L"Warning: Truncating height due to insufficient processed pixel data.\n");
         result.actual_height = pixels.size() / width;
         num_pixels_to_use = static_cast<size_t>(width) * result.actual_height;
         if (num_pixels_to_use == 0) {
            result.error_message = L"Insufficient pixel data for even one row after truncation.";
            return result;
         }
         pixels.resize(num_pixels_to_use);
     } else if (num_pixels_to_use < pixels.size()) {
         pixels.resize(num_pixels_to_use);
     }

    auto gdi_bitmap = std::make_shared<Gdiplus::Bitmap>(width, result.actual_height, PixelFormat32bppARGB);

    if (gdi_bitmap == nullptr || gdi_bitmap->GetLastStatus() != Gdiplus::Ok) {
        Gdiplus::Status status = (gdi_bitmap) ? gdi_bitmap->GetLastStatus() : Gdiplus::OutOfMemory;
        std::wstringstream ssErr;
        ssErr << L"Failed to create GDI+ bitmap object. Status: " << status;
        result.error_message = ssErr.str();
        result.image = nullptr;
        return result;
    }

    Gdiplus::Rect rect(0, 0, width, result.actual_height);
    Gdiplus::BitmapData bitmapData;
    Gdiplus::Status lock_status = gdi_bitmap->LockBits(
        &rect,
        Gdiplus::ImageLockModeWrite,
        PixelFormat32bppARGB,
        &bitmapData);

    if (lock_status != Gdiplus::Ok) {
        std::wstringstream ssErr;
        ssErr << L"Failed to lock bitmap bits for writing. Status: " << lock_status;
        result.error_message = ssErr.str();
        result.image = nullptr;
        return result;
    }

    BYTE* dest_scan0 = static_cast<BYTE*>(bitmapData.Scan0);
    BYTE* src_pixels = reinterpret_cast<BYTE*>(pixels.data());
    UINT bytes_per_row_src = width * sizeof(UINT32);
    UINT stride_dest = bitmapData.Stride;

    if (pixels.size() != num_pixels_to_use) {
         gdi_bitmap->UnlockBits(&bitmapData);
         result.error_message = L"Internal error: Mismatch between pixel count and vector size before copy.";
         result.image = nullptr;
         return result;
    }

    for (int y = 0; y < result.actual_height; ++y) {
        memcpy(dest_scan0 + (static_cast<size_t>(y) * stride_dest),
               src_pixels + (static_cast<size_t>(y) * bytes_per_row_src),
               bytes_per_row_src);
    }

    gdi_bitmap->UnlockBits(&bitmapData);
    result.image = gdi_bitmap;
    return result;
}


struct AppState {
    HINSTANCE hInstance = nullptr;
    HWND hMainWnd = nullptr;
    HWND hBtnSelectFile = nullptr;
    HWND hBtnSavePng = nullptr;
    HWND hLblFile = nullptr;
    HWND hLblFileName = nullptr;
    HWND hLblWidth = nullptr;
    HWND hEditWidth = nullptr;
    HWND hSliderWidth = nullptr;
    HWND hChkMsbFirst = nullptr;
    HWND hChkInvert = nullptr;
    HWND hCanvas = nullptr;
    HWND hStatus = nullptr;

    std::wstring currentFilePath;
    int currentWidth = DEFAULT_WIDTH;
    bool msbFirst = true;
    bool invertPolarity = false;

    std::shared_ptr<Gdiplus::Bitmap> currentBitmap = nullptr;
    int bitmapHeight = 0;
    size_t bytesRemoved = 0;


    int scrollX = 0;
    int scrollY = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;

    UINT_PTR updateTimer = 0;
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK CanvasProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void RegisterCanvasClass(HINSTANCE hInstance);
void UpdateImage(AppState* pState);
void UpdateScrollbars(AppState* pState);
void ScheduleUpdate(AppState* pState);
void OnPaintCanvas(HWND hWnd, AppState* pState);
void OnScrollCanvas(HWND hWnd, AppState* pState, int bar, WPARAM wParam);


int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::Status gdiStat = Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    if (gdiStat != Gdiplus::Ok) {
         MessageBoxW(NULL, L"GDI+ Initialization Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
         return 1;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&icex)) {
         MessageBoxW(NULL, L"Common Controls Initialization Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
         Gdiplus::GdiplusShutdown(gdiplusToken);
         return 1;
    }

    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.cbClsExtra    = 0;
    wcex.cbWndExtra    = sizeof(AppState*);
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName  = nullptr;
    wcex.lpszClassName = L"PrintDataViewerClass";
    wcex.hIconSm       = LoadIcon(wcex.hInstance, IDI_APPLICATION);

    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Main Window Registration Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    RegisterCanvasClass(hInstance);

    AppState* pState = new (std::nothrow) AppState();
    if (!pState) {
         MessageBoxW(NULL, L"Failed to allocate app state!", L"Error - Out of Memory", MB_ICONEXCLAMATION | MB_OK);
         Gdiplus::GdiplusShutdown(gdiplusToken);
         return 1;
    }
    pState->hInstance = hInstance;

	HWND hWnd = CreateWindowExW(
        0,
        wcex.lpszClassName,
        L"Printer Data Viewer",
        WS_OVERLAPPEDWINDOW,

        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,

        NULL,
        NULL,
        hInstance,
        pState
    );

    if (!hWnd) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"Main Window Creation Failed! Error code: " << error;
        MessageBoxW(NULL, ss.str().c_str(), L"Error", MB_ICONEXCLAMATION | MB_OK);
        delete pState;
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    delete pState;
    Gdiplus::GdiplusShutdown(gdiplusToken);

    return (int)msg.wParam;
}

void RegisterCanvasClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = CanvasProc;
    wcex.cbClsExtra    = 0;
    wcex.cbWndExtra    = sizeof(AppState*);
    wcex.hInstance     = hInstance;
    wcex.hIcon         = nullptr;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_APPWORKSPACE + 1);
    wcex.lpszMenuName  = nullptr;
    wcex.lpszClassName = L"BitmapCanvasClass";
    wcex.hIconSm       = nullptr;
    RegisterClassExW(&wcex);
}

#define IDC_BTN_SELECT      1001
#define IDC_BTN_SAVE        1002
#define IDC_LBL_FILE        1003
#define IDC_LBL_FILENAME    1004
#define IDC_LBL_WIDTH       1005
#define IDC_EDIT_WIDTH      1006
#define IDC_SLIDER_WIDTH    1007
#define IDC_CHK_MSB         1008
#define IDC_CHK_INVERT      1009
#define IDC_CANVAS          1010
#define IDC_STATUS          1011

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* pState = nullptr;

    if (message == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pState = reinterpret_cast<AppState*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pState);
        pState->hMainWnd = hWnd;
    } else {
        pState = reinterpret_cast<AppState*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    }

    if (!pState && message != WM_CREATE) {
         return DefWindowProc(hWnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE: {
        int ctrlY = PADDING;

        RECT clientRect;
        GetClientRect(hWnd, &clientRect);
        int clientWidth = clientRect.right - clientRect.left;
        int clientHeight = clientRect.bottom - clientRect.top;
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        pState->hBtnSelectFile = CreateWindowW(L"BUTTON", L"Select Bitmap File", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            PADDING, ctrlY, BTN_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_BTN_SELECT, pState->hInstance, NULL);
        SendMessage(pState->hBtnSelectFile, WM_SETFONT, (WPARAM)hFont, TRUE);

        pState->hBtnSavePng = CreateWindowW(L"BUTTON", L"Save as PNG...", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
            PADDING + BTN_WIDTH + PADDING, ctrlY, BTN_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_BTN_SAVE, pState->hInstance, NULL);
        SendMessage(pState->hBtnSavePng, WM_SETFONT, (WPARAM)hFont, TRUE);

        int fileLblX = PADDING + BTN_WIDTH * 2 + PADDING * 2;
        pState->hLblFile = CreateWindowW(L"STATIC", L"File:", WS_VISIBLE | WS_CHILD | SS_RIGHT,
             fileLblX, ctrlY + LBL_VERT_OFFSET, SHORT_LBL_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_LBL_FILE, pState->hInstance, NULL);
        SendMessage(pState->hLblFile, WM_SETFONT, (WPARAM)hFont, TRUE);

        int fileNameX = fileLblX + SHORT_LBL_WIDTH + PADDING;
        int fileNameW = clientWidth - fileNameX - PADDING;
        pState->hLblFileName = CreateWindowW(L"STATIC", L"Select a file...", WS_VISIBLE | WS_CHILD | SS_LEFT | WS_BORDER | SS_ENDELLIPSIS,
            fileNameX, ctrlY, std::max(50, fileNameW), CTRL_HEIGHT, hWnd, (HMENU)IDC_LBL_FILENAME, pState->hInstance, NULL);
        SendMessage(pState->hLblFileName, WM_SETFONT, (WPARAM)hFont, TRUE);

        ctrlY += CTRL_HEIGHT + PADDING;

        pState->hLblWidth = CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD | SS_LEFT,
            PADDING, ctrlY + LBL_VERT_OFFSET, SHORT_LBL_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_LBL_WIDTH, pState->hInstance, NULL);
         SendMessage(pState->hLblWidth, WM_SETFONT, (WPARAM)hFont, TRUE);

        int editWidthX = PADDING + SHORT_LBL_WIDTH + 5;
        pState->hEditWidth = CreateWindowW(L"EDIT", std::to_wstring(DEFAULT_WIDTH).c_str(), WS_TABSTOP | WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
            editWidthX, ctrlY, EDIT_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_EDIT_WIDTH, pState->hInstance, NULL);
         SendMessage(pState->hEditWidth, WM_SETFONT, (WPARAM)hFont, TRUE);

        int sliderX = editWidthX + EDIT_WIDTH + PADDING;
        int sliderW = clientWidth - sliderX - PADDING;
        pState->hSliderWidth = CreateWindowW(TRACKBAR_CLASSW, L"", WS_TABSTOP | WS_VISIBLE | WS_CHILD | TBS_AUTOTICKS | TBS_HORZ,
            sliderX, ctrlY, std::max(50, sliderW), CTRL_HEIGHT, hWnd, (HMENU)IDC_SLIDER_WIDTH, pState->hInstance, NULL);
        SendMessage(pState->hSliderWidth, TBM_SETRANGE, (WPARAM)TRUE, (LPARAM)MAKELONG(MIN_WIDTH, MAX_SLIDER_WIDTH));
        SendMessage(pState->hSliderWidth, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)DEFAULT_WIDTH);
        SendMessage(pState->hSliderWidth, TBM_SETPAGESIZE, 0, (LPARAM) (MAX_SLIDER_WIDTH/20));

        ctrlY += CTRL_HEIGHT + PADDING;

        pState->hChkMsbFirst = CreateWindowW(L"BUTTON", L"MSB First", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            PADDING, ctrlY, CHK_WIDTH, CTRL_HEIGHT, hWnd, (HMENU)IDC_CHK_MSB, pState->hInstance, NULL);
        SendMessage(pState->hChkMsbFirst, WM_SETFONT, (WPARAM)hFont, TRUE);
        Button_SetCheck(pState->hChkMsbFirst, BST_CHECKED);

        pState->hChkInvert = CreateWindowW(L"BUTTON", L"Invert Polarity", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            PADDING + CHK_WIDTH + PADDING, ctrlY, CHK_WIDTH + 20, CTRL_HEIGHT, hWnd, (HMENU)IDC_CHK_INVERT, pState->hInstance, NULL);
        SendMessage(pState->hChkInvert, WM_SETFONT, (WPARAM)hFont, TRUE);
        Button_SetCheck(pState->hChkInvert, BST_UNCHECKED);

        ctrlY += CTRL_HEIGHT + PADDING;

        int statusHeight = 0;
        pState->hStatus = CreateWindowW(STATUSCLASSNAMEW, L"Width: ? Height: ?", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS, pState->hInstance, NULL);
        if (pState->hStatus) {
             SendMessage(pState->hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

             SendMessage(pState->hStatus, WM_SIZE, 0, 0);
             RECT statusRect;
             GetWindowRect(pState->hStatus, &statusRect);
             statusHeight = statusRect.bottom - statusRect.top;
        } else {
             MessageBoxW(hWnd, L"Status Bar Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
             statusHeight = 20;
        }

        int canvasY = 50; // Fixed vertical position
        int canvasW = clientWidth - PADDING * 2;
        int canvasH = clientHeight - canvasY - statusHeight - PADDING;
        canvasH = std::max(50, canvasH);

        pState->hCanvas = CreateWindowExW(
             WS_EX_CLIENTEDGE,
             L"BitmapCanvasClass",
             L"",
             WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
             PADDING, canvasY,
             std::max(50, canvasW),
             canvasH,
             hWnd,
             (HMENU)IDC_CANVAS,
             pState->hInstance,
             pState);

        if (pState->hCanvas) {
             RECT canvasRect;
             GetClientRect(pState->hCanvas, &canvasRect);
             pState->canvasWidth = canvasRect.right;
             pState->canvasHeight = canvasRect.bottom;
        } else {
            DWORD error = GetLastError();
            std::wstringstream ss;
            ss << L"Canvas Creation Failed! Error code: " << error;
            MessageBoxW(hWnd, ss.str().c_str(), L"Error", MB_ICONEXCLAMATION | MB_OK);
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }

        EnableWindow(pState->hBtnSavePng, FALSE);
        UpdateScrollbars(pState);
        SetWindowTextW(pState->hStatus, L"Select a file...");

    } break;

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        switch (wmId) {
        case IDC_BTN_SELECT:
            if (wmEvent == BN_CLICKED) {
                WCHAR szFile[MAX_PATH] = { 0 };
                OPENFILENAMEW ofn = { sizeof(ofn) };
                ofn.hwndOwner = hWnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;

                ofn.lpstrFilter = L"Bitmap/Binary (*.bmp;*.bin)\0*.bmp;*.bin\0All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.lpstrInitialDir = NULL;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

                if (GetOpenFileNameW(&ofn) == TRUE) {
                    pState->currentFilePath = szFile;


                    pState->currentWidth = DEFAULT_WIDTH;
                    pState->msbFirst = true;
                    pState->invertPolarity = false;
                    pState->scrollX = 0;
                    pState->scrollY = 0;


                    SetWindowTextW(pState->hLblFileName, std::filesystem::path(pState->currentFilePath).filename().c_str());
                    SetWindowTextW(pState->hEditWidth, std::to_wstring(DEFAULT_WIDTH).c_str());
                    SendMessage(pState->hSliderWidth, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)DEFAULT_WIDTH);
                    Button_SetCheck(pState->hChkMsbFirst, BST_CHECKED);
                    Button_SetCheck(pState->hChkInvert, BST_UNCHECKED);


                    if (pState->updateTimer) { KillTimer(hWnd, pState->updateTimer); pState->updateTimer = 0; }
                    UpdateImage(pState);
                }
            }
            break;

        case IDC_BTN_SAVE:
             if (wmEvent == BN_CLICKED) {
                if (pState->currentBitmap && !pState->currentFilePath.empty()) {
                    WCHAR szFile[MAX_PATH] = { 0 };
                    std::filesystem::path srcPath(pState->currentFilePath);

                    std::wstring defaultName = srcPath.stem().wstring() + L"_processed.png";

                    wcsncpy_s(szFile, MAX_PATH, defaultName.c_str(), _TRUNCATE);

                    OPENFILENAMEW ofn = { sizeof(ofn) };
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrDefExt = L"png";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

                    if (GetSaveFileNameW(&ofn) == TRUE) {
                        CLSID pngClsid;

                        int result = GetEncoderClsid(L"image/png", &pngClsid);
                        if (result != -1) {

                            Gdiplus::Status status = pState->currentBitmap->Save(szFile, &pngClsid, NULL);
                            if (status == Gdiplus::Ok) {
                                std::wstring msg = L"Saved: " + std::filesystem::path(szFile).filename().wstring();
                                SetWindowTextW(pState->hStatus, msg.c_str());
                            } else {
                                MessageBoxW(hWnd, L"Failed to save PNG image.", L"Save Error", MB_ICONERROR);
                                SetWindowTextW(pState->hStatus, L"Save failed.");
                            }
                        } else {
                            MessageBoxW(hWnd, L"PNG Image Encoder not found. Cannot save.", L"Save Error", MB_ICONERROR);
                            SetWindowTextW(pState->hStatus, L"Save failed (no encoder).");
                        }
                    }
                } else {
                    MessageBoxW(hWnd, L"No image loaded to save.", L"Save Error", MB_ICONWARNING);
                }
             }
            break;

        case IDC_EDIT_WIDTH:

            if (wmEvent == EN_KILLFOCUS) {
                WCHAR buffer[10];
                GetWindowTextW(pState->hEditWidth, buffer, 10);
                std::wstring widthStr = buffer;
                int newWidth = pState->currentWidth;

                try {

                    size_t processed = 0;
                    newWidth = std::stoi(widthStr, &processed);

                    if (processed != widthStr.length()) {
                        throw std::invalid_argument("Non-numeric characters present");
                    }


                    newWidth = std::clamp(newWidth, MIN_WIDTH, MAX_SLIDER_WIDTH);

                    if (newWidth != pState->currentWidth) {
                        pState->currentWidth = newWidth;

                        SendMessage(pState->hSliderWidth, TBM_SETPOS, TRUE, (LPARAM)newWidth);

                        if (pState->updateTimer) { KillTimer(hWnd, pState->updateTimer); pState->updateTimer = 0; }
                        UpdateImage(pState);
                    }

                    SetWindowTextW(pState->hEditWidth, std::to_wstring(newWidth).c_str());

                } catch (const std::invalid_argument&) {

                    SetWindowTextW(pState->hEditWidth, std::to_wstring(pState->currentWidth).c_str());
                    MessageBoxW(hWnd, L"Invalid width value entered. Please enter a number.", L"Input Error", MB_ICONWARNING);
                } catch (const std::out_of_range&) {

                    SetWindowTextW(pState->hEditWidth, std::to_wstring(pState->currentWidth).c_str());
                    MessageBoxW(hWnd, L"Width value entered is too large.", L"Input Error", MB_ICONWARNING);
                }
            }
            break;

        case IDC_CHK_MSB:
        case IDC_CHK_INVERT:
             if (wmEvent == BN_CLICKED) {
                 pState->msbFirst = (Button_GetCheck(pState->hChkMsbFirst) == BST_CHECKED);
                 pState->invertPolarity = (Button_GetCheck(pState->hChkInvert) == BST_CHECKED);
                 if (pState->updateTimer) { KillTimer(hWnd, pState->updateTimer); pState->updateTimer = 0; }
                 UpdateImage(pState);
             }
            break;

        }
    } break;

    case WM_HSCROLL: {
        if ((HWND)lParam == pState->hSliderWidth) {
            int newPos = SendMessage(pState->hSliderWidth, TBM_GETPOS, 0, 0);
            if (newPos != pState->currentWidth) {
                pState->currentWidth = newPos;
                SetWindowTextW(pState->hEditWidth, std::to_wstring(newPos).c_str());
                ScheduleUpdate(pState);
            }
        } else if ((HWND)lParam == pState->hCanvas) {
             OnScrollCanvas(pState->hCanvas, pState, SB_HORZ, wParam);
        } else {

             return DefWindowProc(hWnd, message, wParam, lParam);
        }
    } break;

     case WM_VSCROLL: {

         if ((HWND)lParam == pState->hCanvas) {

             OnScrollCanvas(pState->hCanvas, pState, SB_VERT, wParam);
         } else {

             return DefWindowProc(hWnd, message, wParam, lParam);
         }
     } break;

    case WM_TIMER: {

        if (wParam == UPDATE_TIMER_ID) {
            KillTimer(hWnd, UPDATE_TIMER_ID);
            pState->updateTimer = 0;
            UpdateImage(pState);
        }
    } break;

    case WM_SIZE: {
        int newWidth = LOWORD(lParam);
        int newHeight = HIWORD(lParam);
        int statusHeight = 0;

        // --- Calculate Status Bar Height ---
        // (Do this early so it's available for canvas height calculation)
        if (pState->hStatus) {
            // Send WM_SIZE to the status bar first so it resizes itself
            SendMessage(pState->hStatus, WM_SIZE, 0, 0);
            RECT statusRect;
            // Get its window rect *after* it has potentially resized
            GetWindowRect(pState->hStatus, &statusRect);
            statusHeight = statusRect.bottom - statusRect.top;
        } else {
            statusHeight = 20; // Fallback
        }

        // --- Reposition Horizontally Dependent Controls ---
        HWND lblFile = pState->hLblFile;
        HWND editWidth = pState->hEditWidth;
        // HWND chkInvert = pState->hChkInvert; // No longer needed as vertical reference

        // Reposition File Name Label
        if (pState->hLblFileName && lblFile) {
            RECT rcLblFile;
            // Get client coordinates directly if possible, otherwise map
            // For simplicity, using GetWindowRect + MapWindowPoints as before
            GetWindowRect(lblFile, &rcLblFile);
            MapWindowPoints(HWND_DESKTOP, hWnd, (LPPOINT)&rcLblFile, 1); // Map only the top-left point

            int lblNameX = rcLblFile.right + PADDING; // Position relative to the fixed label
            int lblNameY = rcLblFile.top;             // Keep the same vertical position
            int lblNameW = newWidth - lblNameX - PADDING; // Adjust width
            MoveWindow(pState->hLblFileName, lblNameX, lblNameY, std::max(50, lblNameW), CTRL_HEIGHT, TRUE);
        }

        // Reposition Width Slider
        if (pState->hSliderWidth && editWidth) {
            RECT rcEditWidth;
            GetWindowRect(editWidth, &rcEditWidth);
            MapWindowPoints(HWND_DESKTOP, hWnd, (LPPOINT)&rcEditWidth, 1);

            int sliderX = rcEditWidth.right + PADDING; // Position relative to fixed edit box
            int sliderY = rcEditWidth.top;           // Keep same vertical position
            int sliderW = newWidth - sliderX - PADDING; // Adjust width
            MoveWindow(pState->hSliderWidth, sliderX, sliderY, std::max(50, sliderW), CTRL_HEIGHT, TRUE);
        }

        // --- Reposition Canvas ---
        if (pState->hCanvas) {
            // Calculate the *fixed* vertical position based on control layout
            int fixedCanvasY = PADDING;                      // Top padding
            fixedCanvasY += CTRL_HEIGHT + PADDING;           // After Button row
            fixedCanvasY += CTRL_HEIGHT + PADDING;           // After Width row
            fixedCanvasY += CTRL_HEIGHT + PADDING;           // After Checkbox row

            int canvasX = PADDING;                           // Fixed horizontal position
            int canvasW = newWidth - PADDING * 2;            // Width fills remaining space
            int canvasH = newHeight - fixedCanvasY - statusHeight - PADDING; // Height fills remaining space

            // Ensure minimum dimensions
            canvasW = std::max(50, canvasW);
            canvasH = std::max(50, canvasH);

            // Move the canvas window
            MoveWindow(pState->hCanvas, canvasX, fixedCanvasY, canvasW, canvasH, TRUE);

            // Update canvas internal dimensions and scrollbars after resize
            RECT canvasClientRect;
            GetClientRect(pState->hCanvas, &canvasClientRect);
            pState->canvasWidth = canvasClientRect.right;
            pState->canvasHeight = canvasClientRect.bottom;

            UpdateScrollbars(pState); // Update scrollbars based on new canvas size and image size
        }

    } break;
	
    case WM_DESTROY:

         if (pState && pState->updateTimer) {
             KillTimer(hWnd, pState->updateTimer);
             pState->updateTimer = 0;
         }
        PostQuitMessage(0);
        break;

    case WM_ERASEBKGND:


         return 1;

    case WM_PAINT: {

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        EndPaint(hWnd, &ps);
    } break;

    default:

        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK CanvasProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
     AppState* pState = nullptr;
     if (message == WM_NCCREATE) {

          CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
          pState = reinterpret_cast<AppState*>(pCreate->lpCreateParams);
          SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pState);
          return DefWindowProc(hWnd, message, wParam, lParam);
     } else {
          pState = reinterpret_cast<AppState*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
     }


     if (!pState) {
         OutputDebugStringW(L"Warning: CanvasProc retrieving null pState!\n");
         return DefWindowProc(hWnd, message, wParam, lParam);
     }

    switch (message) {

    case WM_PAINT:
         OnPaintCanvas(hWnd, pState);
        break;

    case WM_ERASEBKGND:
         {
            HDC hdc = (HDC)wParam;
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            FillRect(hdc, &rcClient, (HBRUSH)(COLOR_APPWORKSPACE + 1));
            return 1;
         }

    case WM_HSCROLL:
         OnScrollCanvas(hWnd, pState, SB_HORZ, wParam);
        break;
    case WM_VSCROLL:
         OnScrollCanvas(hWnd, pState, SB_VERT, wParam);
        break;

    default:

        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}


void ScheduleUpdate(AppState* pState) {
     if (!pState || !pState->hMainWnd) return;


     if (pState->updateTimer) {
         KillTimer(pState->hMainWnd, pState->updateTimer);

     }

     pState->updateTimer = SetTimer(pState->hMainWnd,
                                    UPDATE_TIMER_ID,
                                    UPDATE_DELAY_MS,
                                    NULL);
    if (!pState->updateTimer) {
         OutputDebugStringW(L"Error: Failed to set update timer!\n");

         UpdateImage(pState);
    }
}



void UpdateImage(AppState* pState) {
    if (!pState || !pState->hMainWnd) return;


    if (pState->currentFilePath.empty()) {
         pState->currentBitmap = nullptr;
         pState->bitmapHeight = 0;
         pState->bytesRemoved = 0;
         EnableWindow(pState->hBtnSavePng, FALSE);
         SetWindowTextW(pState->hStatus, L"Select a file...");
         UpdateScrollbars(pState);
         if (pState->hCanvas) {
            InvalidateRect(pState->hCanvas, NULL, TRUE);
            UpdateWindow(pState->hCanvas);
         }
        return;
    }


    SetWindowTextW(pState->hStatus, L"Processing...");
    UpdateWindow(pState->hStatus);


    ProcessedBitmapResult result = load_and_process_bitmap(
        pState->currentFilePath,
        pState->currentWidth,
        pState->msbFirst,
        pState->invertPolarity);


    pState->currentBitmap = result.image;
    pState->bitmapHeight = result.actual_height;
    pState->bytesRemoved = result.bytes_removed_count;


    std::wstringstream ssStatus;
     if (pState->currentBitmap) {
         ssStatus << L"W:" << result.actual_width
                  << L" H:" << result.actual_height
                  << L" (" << (pState->msbFirst ? L"MSB" : L"LSB")
                  << (pState->invertPolarity ? L", INV" : L"") << L")";
         if (result.bytes_removed_count > 0) {
             ssStatus << L" (" << result.bytes_removed_count << L" bytes removed)";
         }
         EnableWindow(pState->hBtnSavePng, TRUE);
     } else {

         if (!result.error_message.empty()) {
            ssStatus << L"Error: " << result.error_message;
         } else {
             ssStatus << L"Load/Process Failed.";
         }

          if (result.bytes_removed_count > 0) {
              ssStatus << L" (" << result.bytes_removed_count << L" bytes removed)";
          }
          EnableWindow(pState->hBtnSavePng, FALSE);

          MessageBoxW(pState->hMainWnd, ssStatus.str().c_str(), L"Processing Error", MB_ICONWARNING | MB_OK);
     }
     SetWindowTextW(pState->hStatus, ssStatus.str().c_str());


    pState->scrollX = 0;
    pState->scrollY = 0;
    UpdateScrollbars(pState);


    if (pState->hCanvas) {
         InvalidateRect(pState->hCanvas, NULL, TRUE);
         UpdateWindow(pState->hCanvas);
    }
}

void UpdateScrollbars(AppState* pState) {
     if (!pState || !pState->hCanvas) return;

     SCROLLINFO si = { sizeof(SCROLLINFO) };
     si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;


     int imageWidth = (pState->currentBitmap) ? pState->currentBitmap->GetWidth() : 0;

     RECT canvasClientRect;
     GetClientRect(pState->hCanvas, &canvasClientRect);
     pState->canvasWidth = canvasClientRect.right;

     if (pState->currentBitmap && imageWidth > pState->canvasWidth) {

         si.nMin = 0;
         si.nMax = imageWidth - 1;
         si.nPage = pState->canvasWidth;
         si.nPos = pState->scrollX;
     } else {

         si.nMin = 0;
         si.nMax = 0;
         si.nPage = 1;
         si.nPos = 0;
         pState->scrollX = 0;
     }
     SetScrollInfo(pState->hCanvas, SB_HORZ, &si, TRUE);


      int imageHeight = (pState->currentBitmap) ? pState->currentBitmap->GetHeight() : 0;
      pState->canvasHeight = canvasClientRect.bottom;

      if (pState->currentBitmap && imageHeight > pState->canvasHeight) {

         si.nMin = 0;
         si.nMax = imageHeight -1;
         si.nPage = pState->canvasHeight;
         si.nPos = pState->scrollY;
      } else {

         si.nMin = 0;
         si.nMax = 0;
         si.nPage = 1;
         si.nPos = 0;
          pState->scrollY = 0;
      }
      SetScrollInfo(pState->hCanvas, SB_VERT, &si, TRUE);
}


void OnPaintCanvas(HWND hWnd, AppState* pState) {
    PAINTSTRUCT ps;
    HDC hdcScreen = BeginPaint(hWnd, &ps);


     RECT rcClient;
     GetClientRect(hWnd, &rcClient);
     int clientWidth = rcClient.right - rcClient.left;
     int clientHeight = rcClient.bottom - rcClient.top;


     HDC hdcMem = CreateCompatibleDC(hdcScreen);
     if (!hdcMem) {
          EndPaint(hWnd, &ps);
          return;
     }

     HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, clientWidth, clientHeight);
     if (!hbmMem) {
          DeleteDC(hdcMem);
          EndPaint(hWnd, &ps);
          return;
     }

     HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);


     Gdiplus::Graphics graphicsMem(hdcMem);
     graphicsMem.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
     graphicsMem.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);




     Gdiplus::Color bkgColor;
     bkgColor.SetFromCOLORREF(GetSysColor(COLOR_APPWORKSPACE));
     Gdiplus::SolidBrush backgroundBrush(bkgColor);
     graphicsMem.FillRectangle(&backgroundBrush, 0, 0, clientWidth, clientHeight);


     if (pState->currentBitmap) {

          Gdiplus::Rect destRect(0, 0, clientWidth, clientHeight);

          Gdiplus::Rect srcRect(pState->scrollX, pState->scrollY,
                                 clientWidth,
                                 clientHeight);

         graphicsMem.DrawImage(
             pState->currentBitmap.get(),
             destRect,
             srcRect.X, srcRect.Y,
             srcRect.Width, srcRect.Height,
             Gdiplus::UnitPixel,
             nullptr);

     } else {

         std::wstring placeholder = L"Select a file...";
         if (!pState->currentFilePath.empty()) {
             placeholder = L"Failed to load/process image.";
         }
         Gdiplus::FontFamily fontFamily(L"Segoe UI");
         Gdiplus::Font font(&fontFamily, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
         Gdiplus::SolidBrush textBrush(Gdiplus::Color(0, 0, 0));
         Gdiplus::StringFormat strFormat;
         strFormat.SetAlignment(Gdiplus::StringAlignmentNear);
         strFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
         graphicsMem.DrawString(placeholder.c_str(), -1, &font, Gdiplus::PointF((Gdiplus::REAL)PADDING, (Gdiplus::REAL)PADDING), &strFormat, &textBrush);
     }

     BitBlt(hdcScreen,
            ps.rcPaint.left, ps.rcPaint.top,
            ps.rcPaint.right - ps.rcPaint.left,
            ps.rcPaint.bottom - ps.rcPaint.top,
            hdcMem,
            ps.rcPaint.left, ps.rcPaint.top,
            SRCCOPY);

     SelectObject(hdcMem, hbmOld);
     DeleteObject(hbmMem);
     DeleteDC(hdcMem);

    EndPaint(hWnd, &ps);
}


void OnScrollCanvas(HWND hWnd, AppState* pState, int bar, WPARAM wParam) {

     if (!pState || !pState->currentBitmap) return;

     SCROLLINFO si = { sizeof(si) };
     si.fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
     GetScrollInfo(hWnd, bar, &si);

     int currentPos = si.nPos;
     int maxScroll = si.nMax - (si.nPage > 0 ? si.nPage - 1: 0);
     maxScroll = std::max(0, maxScroll);

     int scrollAmount = 0;
     int lineAmount = 40;
     int pageAmount = (si.nPage > 0) ? si.nPage : ((bar == SB_HORZ) ? pState->canvasWidth : pState->canvasHeight);

     switch (LOWORD(wParam)) {
         case SB_LINELEFT:
             scrollAmount = -lineAmount;
             break;
         case SB_LINERIGHT:
             scrollAmount = lineAmount;
             break;
         case SB_PAGELEFT:
             scrollAmount = -pageAmount;
             break;
         case SB_PAGERIGHT:
             scrollAmount = pageAmount;
             break;
         case SB_THUMBTRACK:
         case SB_THUMBPOSITION:
             scrollAmount = HIWORD(wParam) - currentPos;
             break;
         case SB_LEFT:
              scrollAmount = -currentPos;
             break;
         case SB_RIGHT:
              scrollAmount = maxScroll - currentPos;
             break;
         default:
             scrollAmount = 0;
             break;
     }

     int newPos = currentPos + scrollAmount;
     newPos = std::max(0, std::min(newPos, maxScroll));

     if (newPos != currentPos) {
          if (bar == SB_HORZ) {
             pState->scrollX = newPos;
         } else {
             pState->scrollY = newPos;
         }

         si.fMask = SIF_POS;
         si.nPos = newPos;
         SetScrollInfo(hWnd, bar, &si, TRUE);

         ScrollWindowEx(hWnd,
                        (bar == SB_HORZ ? currentPos - newPos : 0),
                        (bar == SB_VERT ? currentPos - newPos : 0),
                        NULL, NULL,
                        NULL,
                        NULL,
                        SW_INVALIDATE | SW_ERASE);

         UpdateWindow(hWnd);
     }
}
