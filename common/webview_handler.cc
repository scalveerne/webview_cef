#include "webview_handler.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <iostream>
#include <functional>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdlib> // Para atoi, atoll

// CEF Includes
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h" // Asumiendo que podrías necesitarlos
#include "include/views/cef_window.h"       // Asumiendo que podrías necesitarlos
#include "include/wrapper/cef_helpers.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_values.h"
#include "include/cef_cookie.h"
#include "include/cef_process_message.h"
#include "include/cef_command_line.h"

// Project Includes
#include "webview_cookieVisitor.h" // Asegúrate que este archivo existe
#include "webview_js_handler.h"

// Platform Includes
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// stringpatch for Ubuntu 24.04 to_string issue
namespace stringpatch
{
    template <typename T>
    std::string to_string(const T &n)
    {
        std::ostringstream stm;
        stm << n;
        return stm.str();
    }
}

namespace
{
    CefRefPtr<CefBrowser> current_focused_browser_ = nullptr;

    std::string GetDataURI(const std::string &data, const std::string &mime_type)
    {
        return "data:" + mime_type + ";base64," +
               CefURIEncode(CefBase64Encode(data.data(), data.size()), false)
                   .ToString();
    }

} // namespace

WebviewHandler::WebviewHandler() {}

WebviewHandler::~WebviewHandler()
{
    browser_map_.clear();
    js_callbacks_.clear();
    profile_contexts_.clear();
}

// --- Implementación de CefClient y sus Handlers ---

// CefLifeSpanHandler methods
void WebviewHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    if (!browser->IsPopup())
    {
        browser_map_.emplace(browser->GetIdentifier(), browser_info());
        if (browser_map_.count(browser->GetIdentifier()))
        { // Check if emplace succeeded
            browser_map_[browser->GetIdentifier()].browser = browser;
        }
    }
}

bool WebviewHandler::DoClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    return false; // Allow close
}

void WebviewHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    if (!browser->IsPopup())
    {
        auto it = browser_map_.find(browser->GetIdentifier());
        if (it != browser_map_.end())
        {
            // Release the browser reference
            it->second.browser = nullptr;
            browser_map_.erase(it);
        }
        // Check if this was the last browser using a specific context and clean up if desired
        // (Requires tracking context usage count or iterating contexts)
    }
}

bool WebviewHandler::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   const CefString &target_url,
                                   const CefString &target_frame_name,
                                   WindowOpenDisposition target_disposition,
                                   bool user_gesture,
                                   const CefPopupFeatures &popupFeatures,
                                   CefWindowInfo &windowInfo,
                                   CefRefPtr<CefClient> &client,
                                   CefBrowserSettings &settings,
                                   CefRefPtr<CefDictionaryValue> &extra_info,
                                   bool *no_javascript_access)
{
    CEF_REQUIRE_UI_THREAD();
    std::string url_str = target_url.ToString();

    if (url_str.find("cloudflare") != std::string::npos ||
        url_str.find("cf-challenge") != std::string::npos ||
        url_str.find("captcha") != std::string::npos ||
        url_str.find("turnstile") != std::string::npos ||
        url_str.find("recaptcha") != std::string::npos ||
        url_str.find("hcaptcha") != std::string::npos ||
        url_str.find("challenge-platform") != std::string::npos ||
        url_str.find("security-check") != std::string::npos ||
        url_str.find("__cf_chl") != std::string::npos)
    {
        if (no_javascript_access)
        {
            *no_javascript_access = false;
        }
        return false; // Allow Cloudflare popups
    }

    // Redirect other popups to main frame
    if (frame && frame->IsValid())
    { // Check if frame is valid before loading URL
        frame->LoadURL(target_url);
    }
    else if (browser && browser->GetMainFrame())
    {
        browser->GetMainFrame()->LoadURL(target_url); // Fallback to main frame of current browser
    }
    return true; // Prevent popup window
}

// CefLoadHandler methods
void WebviewHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                 CefLoadHandler::TransitionType transition_type)
{
    CEF_REQUIRE_UI_THREAD();
    if (onLoadStart && frame && frame->IsMain())
    {
        onLoadStart(browser->GetIdentifier(), frame->GetURL().ToString());
    }
}

void WebviewHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                               int httpStatusCode)
{
    CEF_REQUIRE_UI_THREAD();
    if (frame && frame->IsMain())
    {
        // Inject JS (Consider if this is always needed or only on success)
        std::string script = R"((function() { /* ... context menu js ... */ })();)";
        std::string cloudflareSupport = R"((function() { /* ... cloudflare helper js ... */ })();)";

        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        frame->ExecuteJavaScript(cloudflareSupport, frame->GetURL(), 0);

        if (onLoadEnd)
        {
            onLoadEnd(browser->GetIdentifier(), frame->GetURL().ToString());
        }
    }
}

void WebviewHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 ErrorCode errorCode,
                                 const CefString &errorText,
                                 const CefString &failedUrl)
{
    CEF_REQUIRE_UI_THREAD();

    if (IsChromeRuntimeEnabled())
        return;
    if (errorCode == ERR_ABORTED)
        return; // Ignore aborted loads (e.g., downloads)

    // Display simple error message
    std::stringstream ss;
    ss << "<html><body bgcolor=\"white\">"
          "<h2>Failed to load URL "
       << std::string(failedUrl) << " with error " << std::string(errorText)
       << " (" << errorCode << ").</h2></body></html>";

    if (frame && frame->IsValid())
    { // Check validity before loading
        frame->LoadURL(GetDataURI(ss.str(), "text/html"));
    }
}

// CefDisplayHandler methods
void WebviewHandler::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title)
{
    CEF_REQUIRE_UI_THREAD();
    if (onTitleChangedEvent)
    {
        onTitleChangedEvent(browser->GetIdentifier(), title.ToString());
    }
}

void WebviewHandler::OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &url)
{
    CEF_REQUIRE_UI_THREAD();
    // Usually interested in main frame URL changes
    if (onUrlChangedEvent && frame && frame->IsMain())
    {
        onUrlChangedEvent(browser->GetIdentifier(), url.ToString());
    }
}

bool WebviewHandler::OnCursorChange(CefRefPtr<CefBrowser> browser,
                                    CefCursorHandle cursor,
                                    cef_cursor_type_t type,
                                    const CefCursorInfo &custom_cursor_info)
{
    CEF_REQUIRE_UI_THREAD();
    if (onCursorChangedEvent)
    {
        onCursorChangedEvent(browser->GetIdentifier(), type);
        return true;
    }
    return false;
}

bool WebviewHandler::OnTooltip(CefRefPtr<CefBrowser> browser, CefString &text)
{
    CEF_REQUIRE_UI_THREAD();
    if (onTooltipEvent)
    {
        // CefString needs conversion for std::string callback
        onTooltipEvent(browser->GetIdentifier(), text.ToString());
        return true; // We handled the tooltip (prevent default)
    }
    return false;
}

bool WebviewHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                      cef_log_severity_t level,
                                      const CefString &message,
                                      const CefString &source,
                                      int line)
{
    // Note: This runs on the UI thread, but logging might be better offloaded
    if (onConsoleMessageEvent)
    {
        onConsoleMessageEvent(browser->GetIdentifier(), level, message.ToString(), source.ToString(), line);
    }
    return false; // Let default logging proceed if any
}

// CefFocusHandler methods
void WebviewHandler::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    CEF_REQUIRE_UI_THREAD();
    // Optionally blur active element if focus moves away
    // executeJavaScript(browser->GetIdentifier(), "document.activeElement.blur()");
}

bool WebviewHandler::OnSetFocus(CefRefPtr<CefBrowser> browser, FocusSource source)
{
    CEF_REQUIRE_UI_THREAD();
    // Returning false allows default focus handling
    return false;
}

void WebviewHandler::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    current_focused_browser_ = browser; // Track focused browser
}

// CefRenderHandler methods
void WebviewHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup())
    {
        rect = CefRect(0, 0, 1, 1); // Default small rect if not found
        return;
    }
    rect.x = 0;
    rect.y = 0;
    rect.width = (it->second.width > 0) ? it->second.width : 1;
    rect.height = (it->second.height > 0) ? it->second.height : 1;
}

bool WebviewHandler::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end())
    {
        screen_info.device_scale_factor = it->second.dpi > 0 ? it->second.dpi : 1.0f;
    }
    else
    {
        screen_info.device_scale_factor = 1.0f; // Default DPI
    }
    // Returning false uses default screen info detection where possible
    return false;
}

void WebviewHandler::OnPaint(CefRefPtr<CefBrowser> browser, CefRenderHandler::PaintElementType type,
                             const CefRenderHandler::RectList &dirtyRects, const void *buffer, int w, int h)
{
    // This can be called frequently, ensure callback is efficient
    if (!browser->IsPopup() && onPaintCallback)
    {
        onPaintCallback(browser->GetIdentifier(), buffer, w, h);
    }
}

void WebviewHandler::OnImeCompositionRangeChanged(CefRefPtr<CefBrowser> browser, const CefRange &selection_range, const CefRenderHandler::RectList &character_bounds)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup())
    {
        return;
    }

    if (onImeCompositionRangeChangedMessage && !character_bounds.empty())
    {
        // Use the first character bound's bottom-left corner as anchor point
        const CefRect &firstChar = character_bounds.front();
        onImeCompositionRangeChangedMessage(browser->GetIdentifier(), firstChar.x, firstChar.y + firstChar.height);
    }
}

// CefContextMenuHandler methods
void WebviewHandler::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         CefRefPtr<CefContextMenuParams> params,
                                         CefRefPtr<CefMenuModel> model)
{
    CEF_REQUIRE_UI_THREAD();
    if (model)
    {
        model->Clear(); // Prevent default menu items
    }
}

bool WebviewHandler::RunContextMenu(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefContextMenuParams> params,
                                    CefRefPtr<CefMenuModel> model,
                                    CefRefPtr<CefRunContextMenuCallback> callback)
{
    CEF_REQUIRE_UI_THREAD();
    if (callback)
    {
        callback->Cancel(); // Cancel the menu display
    }

    // Inject JS to simulate contextmenu event at the specified coordinates
    if (frame && params)
    {
        std::stringstream js;
        js << "(() => { try { const evt = new MouseEvent('contextmenu', {"
           << " bubbles: true, cancelable: true, clientX: " << params->GetXCoord()
           << ", clientY: " << params->GetYCoord() << ", button: 2, buttons: 2 });"
           << " let el = document.elementFromPoint(" << params->GetXCoord() << ", " << params->GetYCoord() << ");"
           << " if (el) { el.dispatchEvent(evt); } else { document.dispatchEvent(evt); }"
           << " } catch(e) { console.error('RunContextMenu JS Error:', e); } })();";
        frame->ExecuteJavaScript(js.str(), frame->GetURL(), 0);
    }

    return true; // We handled the context menu
}

// CefDragHandler method (if needed)
bool WebviewHandler::OnDragEnter(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefDragData> dragData,
                                 CefDragHandler::DragOperationsMask mask)
{
    CEF_REQUIRE_UI_THREAD();
    // Return false to allow drag-and-drop by default
    return false;
}

// --- Custom Methods ---

void WebviewHandler::CloseAllBrowsers(bool force_close)
{
    CEF_REQUIRE_UI_THREAD();
    if (browser_map_.empty())
        return;

    // Iterate safely as CloseBrowser might modify the map indirectly via OnBeforeClose
    std::vector<int> browserIds;
    for (const auto &pair : browser_map_)
    {
        browserIds.push_back(pair.first);
    }

    for (int id : browserIds)
    {
        auto it = browser_map_.find(id);
        if (it != browser_map_.end() && it->second.browser.get())
        {
            it->second.browser->GetHost()->CloseBrowser(force_close);
            // OnBeforeClose should handle map removal
        }
    }
    // browser_map_.clear(); // Alternatively, clear forcefully here if OnBeforeClose doesn't remove
}

bool WebviewHandler::IsChromeRuntimeEnabled()
{
    // This check might need CefCommandLine::GetGlobalCommandLine()
    // Ensure CefCommandLine is included: #include "include/cef_command_line.h"
    static int value = -1;
    if (value == -1)
    {
        CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
        if (command_line)
        {
            value = command_line->HasSwitch("enable-chrome-runtime") ? 1 : 0;
        }
        else
        {
            value = 0; // Assume false if command line is not available
        }
    }
    return value == 1;
}

void WebviewHandler::closeBrowser(int browserId)
{
    // Ensure this is called on the UI thread or posts a task to it
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::closeBrowser, this, browserId));
        return;
    }

    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        it->second.browser->GetHost()->CloseBrowser(true); // Force close
        // Map removal should happen in OnBeforeClose
    }
}

void WebviewHandler::createBrowser(std::string url, std::string profileId, std::function<void(int)> callback)
{
    // Post to UI thread if not already on it
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::createBrowser, this, url, profileId, callback));
        return;
    }

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 30;
    // browser_settings.javascript = STATE_ENABLED; // Enabled by default

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0); // OSR requires a parent window handle (0 might be okay on some platforms)

    CefRefPtr<CefRequestContext> context = GetRequestContextForProfile(profileId);
    if (!context)
    {
        std::cerr << "[CEF Create] Failed to get/create request context for profile: " << profileId << std::endl;
        // Decide how to handle: return error, use global context?
        // For now, let CreateBrowserSync potentially fail or use global implicitly.
        context = CefRequestContext::GetGlobalContext(); // Fallback to global if necessary
    }

    CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create(); // Not used currently

    // CreateBrowserSync blocks, ensure it's okay for your UI thread
    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info,
        this,                              // CefClient
        url.empty() ? "about:blank" : url, // Ensure URL is not empty
        browser_settings,
        extra_info,
        context);

    if (browser)
    {
        callback(browser->GetIdentifier());
    }
    else
    {
        std::cerr << "[CEF Create] CefBrowserHost::CreateBrowserSync failed!" << std::endl;
        callback(-1); // Indicate failure with an invalid ID
    }
}

CefRefPtr<CefRequestContext> WebviewHandler::GetRequestContextForProfile(const std::string &profileId)
{
    // Assumes called on UI Thread due to potential map access/modification

    if (profileId.empty())
    {
        std::cout << "[CEF Context] Using global context for empty profile ID." << std::endl;
        return CefRequestContext::GetGlobalContext();
    }

    auto it = profile_contexts_.find(profileId);
    if (it != profile_contexts_.end())
    {
        std::cout << "[CEF Context] Reusing context for profile: " << profileId << std::endl;
        return it->second;
    }

    std::cout << "[CEF Context] Creating new context for profile: " << profileId << std::endl;
    CefRequestContextSettings settings;

    std::hash<std::string> hasher;
    std::string safeProfileDirName = "profile_" + stringpatch::to_string(hasher(profileId) % 1000000);

    std::string rootCachePathBase;
#ifdef _WIN32
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) > 0)
    {
        std::string uniqueAppId = "ScalBrowser_1234";
        rootCachePathBase = std::string(tempPath) + uniqueAppId;
    }
    else
    {
        std::cerr << "[CEF Context] ERROR: Failed to get Windows temp path." << std::endl;
        rootCachePathBase = "C:\\ScalBrowserTemp\\ScalBrowser_1234"; // Bad fallback
    }
#else
    const char *homeDir = getenv("HOME");
    std::string uniqueAppId = "ScalBrowser_1234";
    if (homeDir)
    {
        rootCachePathBase = std::string(homeDir) + "/.cache/" + uniqueAppId;
    }
    else
    {
        rootCachePathBase = "/tmp/" + uniqueAppId; // Fallback
    }
#endif

    // Ensure base path doesn't end with separator
    if (!rootCachePathBase.empty() && (rootCachePathBase.back() == '/' || rootCachePathBase.back() == '\\'))
    {
        rootCachePathBase.pop_back();
    }
#ifdef _WIN32
    std::string profileCachePath = rootCachePathBase + "\\profiles\\" + safeProfileDirName;
#else
    std::string profileCachePath = rootCachePathBase + "/profiles/" + safeProfileDirName;
#endif

    std::cout << "[CEF Context] Setting cache path: " << profileCachePath << std::endl;

    CefString(&settings.cache_path) = profileCachePath;
    settings.persist_session_cookies = true;
    // settings.persist_user_preferences = true; // REMOVED due to potential version incompatibility / compile error

    // Create the context. CEF will create the directory if root_cache_path was set correctly.
    CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, nullptr);

    if (context)
    {
        profile_contexts_[profileId] = context;
        std::cout << "[CEF Context] Context created successfully for profile: " << profileId << std::endl;
        return context;
    }
    else
    {
        std::cerr << "[CEF Context] CRITICAL ERROR: Failed to create context for profile: "
                  << profileId << " Path: " << profileCachePath << std::endl;
        std::cerr << "[CEF Context]            Check root_cache_path in CefInitialize and base directory permissions." << std::endl;
        std::cerr << "[CEF Context]            Falling back to global context." << std::endl;
        return CefRequestContext::GetGlobalContext(); // Fallback
    }
}

void WebviewHandler::sendScrollEvent(int browserId, int x, int y, int deltaX, int deltaY)
{
    // Ensure on UI thread or post task
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::sendScrollEvent, this, browserId, x, y, deltaX, deltaY));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;
        // Modifiers could be added if needed (e.g., shift key during scroll)

#ifndef __APPLE__
        // Windows/Linux often need inverted deltaY and potentially scaling
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX * 3, -deltaY * 3);
#else
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX, deltaY);
#endif
    }
}

void WebviewHandler::changeSize(int browserId, float a_dpi, int w, int h)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::changeSize, this, browserId, a_dpi, w, h));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        it->second.dpi = a_dpi > 0 ? a_dpi : 1.0f; // Ensure DPI is positive
        it->second.width = w > 0 ? w : 1;          // Ensure width is positive
        it->second.height = h > 0 ? h : 1;         // Ensure height is positive
        it->second.browser->GetHost()->WasResized();
    }
}

// keyboard modifier conversion (keep as is)
/* uint32_t convertKeyModifiers(...) { ... } */

void WebviewHandler::cursorClick(int browserId, int x, int y, bool up, int button)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::cursorClick, this, browserId, x, y, up, button));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
        return;

    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;

    // Right Click (button == 2) handled via JS in RunContextMenu, maybe redundant here?
    // If you want separate handling:
    if (button == 2)
    {
        if (!up)
        { // Trigger on mouse down
            CefRefPtr<CefFrame> frame = it->second.browser->GetMainFrame();
            if (frame)
            {
                // Inject JS context menu event (same as RunContextMenu)
                std::stringstream js;
                js << "/* JS for context menu event */"; // Simplified for brevity
                frame->ExecuteJavaScript(js.str(), frame->GetURL(), 0);
            }
        }
        // Do not send MBT_RIGHT click event to CEF? Depends on desired behavior.
    }
    else
    { // Left Click (button == 0 or default)
        ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        CefBrowserHost::MouseButtonType cefButton = MBT_LEFT;

        // Simplified click logic (multi-click can be complex)
        int clickCount = 1; // Default to single click
        // Add multi-click detection logic here if needed, updating clickCount

        it->second.browser->GetHost()->SendMouseClickEvent(ev, cefButton, up, clickCount);
    }
}

void WebviewHandler::cursorMove(int browserId, int x, int y, bool dragging)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::cursorMove, this, browserId, x, y, dragging));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;
        bool mouseLeave = false; // Set true if cursor leaves the view area

        if (dragging)
        {
            ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
            // Drag-specific events might be needed if implementing full drag/drop
            // it->second.browser->GetHost()->DragTargetDragOver(ev, DRAG_OPERATION_EVERY);
            it->second.browser->GetHost()->SendMouseMoveEvent(ev, mouseLeave); // Send move event even while dragging
        }
        else
        {
            it->second.browser->GetHost()->SendMouseMoveEvent(ev, mouseLeave);
        }
    }
}

void WebviewHandler::sendKeyEvent(CefKeyEvent &ev)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        // Sending events across threads is tricky. Best to generate on UI thread if possible.
        // If called from another thread, consider if it's safe or needed.
        // CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::sendKeyEvent, this, ev)); // Might copy ev incorrectly
        return;
    }
    // Send to the currently focused browser if tracked
    if (current_focused_browser_ && current_focused_browser_->GetHost())
    {
        current_focused_browser_->GetHost()->SendKeyEvent(ev);
    }
    else
    {
        // Fallback or log error if no browser has focus
        // Maybe send to the *first* available browser?
        // if (!browser_map_.empty() && browser_map_.begin()->second.browser.get()) {
        //     browser_map_.begin()->second.browser->GetHost()->SendKeyEvent(ev);
        // }
    }
}

void WebviewHandler::loadUrl(int browserId, std::string url)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::loadUrl, this, browserId, url));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get() && it->second.browser->GetMainFrame())
    {
        it->second.browser->GetMainFrame()->LoadURL(url);
    }
}

void WebviewHandler::goForward(int browserId)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::goForward, this, browserId));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get() && it->second.browser->CanGoForward())
    {
        it->second.browser->GoForward();
    }
}

void WebviewHandler::goBack(int browserId)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::goBack, this, browserId));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get() && it->second.browser->CanGoBack())
    {
        it->second.browser->GoBack();
    }
}

void WebviewHandler::reload(int browserId)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::reload, this, browserId));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        it->second.browser->Reload();
    }
}

void WebviewHandler::openDevTools(int browserId)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::openDevTools, this, browserId));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        CefWindowInfo windowInfo;
#ifdef OS_WIN
        windowInfo.SetAsPopup(nullptr, "DevTools"); // Requires parent HWND for proper popup
#endif
        CefBrowserSettings settings;
        // Use 'this' as the CefClient for DevTools to handle its lifecycle if needed
        it->second.browser->GetHost()->ShowDevTools(windowInfo, this, settings, CefPoint());
    }
}

void WebviewHandler::imeSetComposition(int browserId, std::string text)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::imeSetComposition, this, browserId, text));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
        return;

    CefString cTextStr(text);
    std::vector<CefCompositionUnderline> underlines;
    // Define underline style if needed
    // cef_composition_underline_t underline = { ... };
    // underlines.push_back(underline);
    CefRange replacement_range(UINT32_MAX, UINT32_MAX);     // Default range
    CefRange selection_range(text.length(), text.length()); // Caret at end

    it->second.browser->GetHost()->ImeSetComposition(cTextStr, underlines, replacement_range, selection_range);
}

void WebviewHandler::imeCommitText(int browserId, std::string text)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::imeCommitText, this, browserId, text));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
        return;

    CefString cTextStr(text);
    CefRange replacement_range(UINT32_MAX, UINT32_MAX);
    int relative_cursor_pos = 0; // Cursor position relative to the committed text end

    it->second.browser->GetHost()->ImeCommitText(cTextStr, replacement_range, relative_cursor_pos);
    it->second.browser->GetHost()->ImeFinishComposingText(false); // Finalize composition
}

void WebviewHandler::setClientFocus(int browserId, bool focus)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::setClientFocus, this, browserId, focus));
        return;
    }
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        it->second.browser->GetHost()->SetFocus(focus);
        if (focus)
        {
            current_focused_browser_ = it->second.browser;
        }
        else if (current_focused_browser_ && current_focused_browser_->GetIdentifier() == browserId)
        {
            current_focused_browser_ = nullptr;
        }
    }
}

void WebviewHandler::setCookie(const std::string &domain, const std::string &key, const std::string &value)
{
    // Cookies can often be set from any thread, but using UI thread is safest
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::setCookie, this, domain, key, value));
        return;
    }

    // Use the context associated with a browser, or global if none specific?
    // For simplicity, using global manager here. Per-profile requires getting context first.
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
        return;

    CefCookie cookie;
    CefString(&cookie.name).FromString(key);
    CefString(&cookie.value).FromString(value);
    CefString(&cookie.domain).FromString(domain);
    CefString(&cookie.path).FromString("/"); // Default path
    cookie.httponly = false;                 // Adjust as needed
    cookie.secure = false;                   // Adjust based on domain/protocol if needed
    // expiration date can be set too: cookie.expires = ...

    // Construct a URL on the domain to set the cookie
    std::string url = "http" + std::string(cookie.secure ? "s" : "") + "://" + domain + "/";
    manager->SetCookie(url, cookie, nullptr); // Use callback for result if needed
}

void WebviewHandler::deleteCookie(const std::string &domain, const std::string &key)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::deleteCookie, this, domain, key));
        return;
    }

    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
        return;

    std::string url = "http://" + domain + "/"; // URL for deletion context
    manager->DeleteCookies(url, key, nullptr);  // Use callback for result
}

void WebviewHandler::visitAllCookies(std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::visitAllCookies, this, callback));
        return;
    }

    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
    {
        callback({});
        return;
    } // Return empty map on error

    CefRefPtr<WebviewCookieVisitor> visitor = new WebviewCookieVisitor(callback);
    manager->VisitAllCookies(visitor);
}

void WebviewHandler::visitUrlCookies(const std::string &domain, bool isHttpOnly, std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::visitUrlCookies, this, domain, isHttpOnly, callback));
        return;
    }

    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
    {
        callback({});
        return;
    }

    std::string url = "http://" + domain + "/";
    CefRefPtr<WebviewCookieVisitor> visitor = new WebviewCookieVisitor(callback);
    manager->VisitUrlCookies(url, isHttpOnly, visitor);
}

void WebviewHandler::setJavaScriptChannels(int browserId, const std::vector<std::string> channels)
{
    // JS execution must be on Render thread or posted via frame
    // This function likely needs to interact with the frame after creation or load end
    // For simplicity, execute directly if possible (assumes frame exists)
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::setJavaScriptChannels, this, browserId, channels));
        return;
    }

    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get() || !it->second.browser->GetMainFrame())
        return;

    CefRefPtr<CefFrame> frame = it->second.browser->GetMainFrame();
    std::string extensionCode = "if(!window.external){window.external={};} if(!window.external.JavaScriptChannel){window.external.JavaScriptChannel = (name, data, cbId) => { CefSharp.PostMessage({ Type: 'JSChannel', Name: name, Data: data, CallbackId: cbId }); };}"; // Example, adapt JS bridge

    for (const auto &channel : channels)
    {
        // Example JS bridge setup - adapt based on your actual JS handler mechanism
        extensionCode += "window['" + channel + "'] = (data, callback) => { let cbId = external.RegisterCallback(callback); external.JavaScriptChannel('" + channel + "', JSON.stringify(data), cbId); };";
    }

    frame->ExecuteJavaScript(extensionCode, frame->GetURL(), 0);
}

void WebviewHandler::sendJavaScriptChannelCallBack(bool error, const std::string result, const std::string callbackId, int browserId, const std::string frameIdStr)
{
    // This sends a message *TO* the Renderer process to execute a JS callback
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::sendJavaScriptChannelCallBack, this, error, result, callbackId, browserId, frameIdStr));
        return;
    }

    auto browser_it = browser_map_.find(browserId);
    if (browser_it == browser_map_.end() || !browser_it->second.browser.get())
        return;

    int64_t frameId = -1;
    try
    {
        frameId = std::stoll(frameIdStr);
    }
    catch (...)
    { /* Invalid frame ID format */
        return;
    }

    CefRefPtr<CefFrame> targetFrame = browser_it->second.browser->GetFrame(frameId);
    if (!targetFrame)
        return; // Frame not found or invalid

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kExecuteJsCallbackMessage);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    int cbIdInt = -1;
    try
    {
        cbIdInt = std::stoi(callbackId);
    }
    catch (...)
    { /* Invalid callback ID format */
        return;
    }

    args->SetInt(0, cbIdInt);
    args->SetBool(1, error);
    args->SetString(2, result);

    targetFrame->SendProcessMessage(PID_RENDERER, message);
}

static std::string GetCallbackId()
{
    // Using timestamp for callback IDs - potential for collision under high load
    auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    return stringpatch::to_string(time.time_since_epoch().count());
}

void WebviewHandler::executeJavaScript(int browserId, const std::string code, std::function<void(CefRefPtr<CefValue>)> callback)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::executeJavaScript, this, browserId, code, callback));
        return;
    }

    if (code.empty())
        return;

    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
        return;

    CefRefPtr<CefFrame> frame = it->second.browser->GetMainFrame();
    if (!frame)
        return;

    std::string finalCode = code;
    if (callback)
    {
        std::string callbackId = GetCallbackId();
        js_callbacks_[callbackId] = callback; // Store callback

        // Wrap code to call back via external.EvaluateCallback
        // Assumes external.EvaluateCallback exists in JS (needs setup in Render Process)
        finalCode = "try { let result = (function(){" + code + "})(); external.EvaluateCallback('" + callbackId + "', result); } catch(e) { external.EvaluateCallback('" + callbackId + "', { error: e.message }); }";
    }

    frame->ExecuteJavaScript(finalCode, frame->GetURL(), 0);
}
void WebviewHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup())
    {
        return;
    }
    rect.x = rect.y = 0;

    if (it->second.width < 1)
    {
        rect.width = 1;
    }
    else
    {
        rect.width = it->second.width;
    }

    if (it->second.height < 1)
    {
        rect.height = 1;
    }
    else
    {
        rect.height = it->second.height;
    }
}

bool WebviewHandler::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info)
{
    // todo: hi dpi support
    screen_info.device_scale_factor = browser_map_[browser->GetIdentifier()].dpi;
    return false;
}

void WebviewHandler::OnPaint(CefRefPtr<CefBrowser> browser, CefRenderHandler::PaintElementType type,
                             const CefRenderHandler::RectList &dirtyRects, const void *buffer, int w, int h)
{
    if (!browser->IsPopup() && onPaintCallback != nullptr)
    {
        onPaintCallback(browser->GetIdentifier(), buffer, w, h);
    }
}

// CefContextMenuHandler methods
void WebviewHandler::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         CefRefPtr<CefContextMenuParams> params,
                                         CefRefPtr<CefMenuModel> model)
{
    // Asegurar que click_count siempre sea al menos 1 para el navegador actual
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end())
    {
        if (it->second.click_count < 1)
        {
            it->second.click_count = 1;
        }
    }

    // Limpiar el modelo de menú para que no se muestre el menú contextual nativo
    if (model)
    {
        model->Clear();
    }
}

bool WebviewHandler::RunContextMenu(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefContextMenuParams> params,
                                    CefRefPtr<CefMenuModel> model,
                                    CefRefPtr<CefRunContextMenuCallback> callback)
{
    // Asegurar que click_count siempre sea al menos 1 para el navegador actual
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end())
    {
        if (it->second.click_count < 1)
        {
            it->second.click_count = 1;
        }
    }

    // Cancelar inmediatamente el menú contextual
    if (callback)
    {
        callback->Cancel();
    }

    try
    {
        // Solo inyectar JavaScript si tenemos un frame válido
        if (frame)
        {
            // Ahora vamos a inyectar JavaScript para simular un evento contextmenu
            std::stringstream js;
            js << "(() => {";
            js << "  try {";
            js << "    const evt = new MouseEvent('contextmenu', {";
            js << "      bubbles: true,";
            js << "      cancelable: true,";
            js << "      clientX: " << params->GetXCoord() << ",";
            js << "      clientY: " << params->GetYCoord() << ",";
            js << "      button: 2,"; // Botón derecho
            js << "      buttons: 2"; // Estado de botones: botón derecho presionado
            js << "    });";
            js << "    let element = document.elementFromPoint(" << params->GetXCoord() << ", " << params->GetYCoord() << ");";
            js << "    if (element) {";
            js << "      try {";
            js << "        element.dispatchEvent(evt);";
            js << "      } catch(e) {";
            js << "        console.error('Error dispatching contextmenu event:', e);";
            js << "        if (document.body) document.body.dispatchEvent(evt);";
            js << "        else document.dispatchEvent(evt);";
            js << "      }";
            js << "    } else if (document.body) {";
            js << "      document.body.dispatchEvent(evt);";
            js << "    } else {";
            js << "      document.dispatchEvent(evt);";
            js << "    }";
            js << "    console.log('Dispatched contextmenu event at (" << params->GetXCoord() << ", " << params->GetYCoord() << ")');";
            js << "  } catch(e) {";
            js << "    console.error('Error in contextmenu handler:', e);";
            js << "  }";
            js << "})();";

            frame->ExecuteJavaScript(js.str(), frame->GetURL(), 0);
        }
    }
    catch (...)
    {
        // Evitar que cualquier excepción durante el manejo del menú contextual cause el cierre
    }

    // Devolvemos true para indicar que hemos manejado el menú contextual
    return true;
}
