// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "webview_handler.h"

#include <sstream>
#include <string>
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <cstdint>
#include <cmath> // Para std::abs

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <sstream>

// std::to_string fails for ints on Ubuntu 24.04:
// webview_handler.cc:86:86: error: no matching function for call to 'to_string'
// webview_handler.cc:567:24: error: no matching function for call to 'to_string'
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

#include "webview_js_handler.h"

namespace
{
    // The only browser that currently get focused
    CefRefPtr<CefBrowser> current_focused_browser_ = nullptr;

    // Returns a data: URI with the specified contents.
    std::string GetDataURI(const std::string &data, const std::string &mime_type)
    {
        return "data:" + mime_type + ";base64," +
               CefURIEncode(CefBase64Encode(data.data(), data.size()), false)
                   .ToString();
    }

} // namespace

WebviewHandler::WebviewHandler()
{
}

WebviewHandler::~WebviewHandler()
{
    browser_map_.clear();
    js_callbacks_.clear();
}

bool WebviewHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    std::string message_name = message->GetName();
    if (message_name == kFocusedNodeChangedMessage)
    {
        current_focused_browser_ = browser;
        bool editable = message->GetArgumentList()->GetBool(0);
        onFocusedNodeChangeMessage(browser->GetIdentifier(), editable);
        if (editable)
        {
            onImeCompositionRangeChangedMessage(browser->GetIdentifier(), message->GetArgumentList()->GetInt(1), message->GetArgumentList()->GetInt(2));
        }
    }
    else if (message_name == kJSCallCppFunctionMessage)
    {
        CefString fun_name = message->GetArgumentList()->GetString(0);
        CefString param = message->GetArgumentList()->GetString(1);
        int js_callback_id = message->GetArgumentList()->GetInt(2);

        if (fun_name.empty() || !(browser.get()))
        {
            return false;
        }

        onJavaScriptChannelMessage(
            fun_name, param, stringpatch::to_string(js_callback_id), browser->GetIdentifier(), stringpatch::to_string(frame->GetIdentifier()));
    }
    else if (message_name == kEvaluateCallbackMessage)
    {
        CefString callbackId = message->GetArgumentList()->GetString(0);
        CefRefPtr<CefValue> param = message->GetArgumentList()->GetValue(1);

        if (!callbackId.empty())
        {
            auto it = js_callbacks_.find(callbackId.ToString());
            if (it != js_callbacks_.end())
            {
                it->second(param);
                js_callbacks_.erase(it);
            }
        }
    }
    return false;
}

void WebviewHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                   const CefString &title)
{
    // todo: title change
    if (onTitleChangedEvent)
    {
        onTitleChangedEvent(browser->GetIdentifier(), title);
    }
}

void WebviewHandler::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const CefString &url)
{
    if (onUrlChangedEvent)
    {
        onUrlChangedEvent(browser->GetIdentifier(), url);
    }
}

bool WebviewHandler::OnCursorChange(CefRefPtr<CefBrowser> browser,
                                    CefCursorHandle cursor,
                                    cef_cursor_type_t type,
                                    const CefCursorInfo &custom_cursor_info)
{
    if (onCursorChangedEvent)
    {
        onCursorChangedEvent(browser->GetIdentifier(), type);
        return true;
    }
    return false;
}

bool WebviewHandler::OnTooltip(CefRefPtr<CefBrowser> browser, CefString &text)
{
    if (onTooltipEvent)
    {
        onTooltipEvent(browser->GetIdentifier(), text);
        return true;
    }
    return false;
}

bool WebviewHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                      cef_log_severity_t level,
                                      const CefString &message,
                                      const CefString &source,
                                      int line)
{
    if (onConsoleMessageEvent)
    {
        onConsoleMessageEvent(browser->GetIdentifier(), level, message, source, line);
    }
    return false;
}

void WebviewHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    if (!browser->IsPopup())
    {
        browser_map_.emplace(browser->GetIdentifier(), browser_info());
        browser_map_[browser->GetIdentifier()].browser = browser;
    }
}

bool WebviewHandler::DoClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
    // Allow the close. For windowed browsers this will result in the OS close
    // event being sent.
    return false;
}

void WebviewHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();
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
    std::string url_str = target_url.ToString();

    // Permitir que Cloudflare y captchas abran popups reales
    if (url_str.find("cloudflare") != std::string::npos ||
        url_str.find("cf-challenge") != std::string::npos ||
        url_str.find("captcha") != std::string::npos ||
        url_str.find("turnstile") != std::string::npos ||
        url_str.find("recaptcha") != std::string::npos ||
        url_str.find("hcaptcha") != std::string::npos)
    {
        return false; // ¡PERMITIR QUE EL POPUP SE ABRA!
    }

    // Para otras URLs, redirigir al frame principal
    loadUrl(browser->GetIdentifier(), target_url);
    return true;
}

void WebviewHandler::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    executeJavaScript(browser->GetIdentifier(), "document.activeElement.blur()");
}

bool WebviewHandler::OnSetFocus(CefRefPtr<CefBrowser> browser, FocusSource source)
{
    return false;
}

void WebviewHandler::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
}

void WebviewHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 ErrorCode errorCode,
                                 const CefString &errorText,
                                 const CefString &failedUrl)
{
    CEF_REQUIRE_UI_THREAD();

    // Allow Chrome to show the error page.
    if (IsChromeRuntimeEnabled())
        return;

    // Don't display an error for downloaded files.
    if (errorCode == ERR_ABORTED)
        return;

    // Display a load error message using a data: URI.
    std::stringstream ss;
    ss << "<html><body bgcolor=\"white\">"
          "<h2>Failed to load URL "
       << std::string(failedUrl) << " with error " << std::string(errorText)
       << " (" << errorCode << ").</h2></body></html>";

    frame->LoadURL(GetDataURI(ss.str(), "text/html"));
}

void WebviewHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                 CefLoadHandler::TransitionType transition_type)
{
    // Solo notificar cuando se trate del frame principal
    if (onLoadStart && frame->IsMain())
    {
        onLoadStart(browser->GetIdentifier(), frame->GetURL());
    }
    return;
}

void WebviewHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                               int httpStatusCode)
{
    if (frame->IsMain())
    {
        // Inyectar detector de eventos contextmenu para depuración
        std::string script = R"(
            (function() {
                if (window.__contextMenuHandlerInstalled) return;
                window.__contextMenuHandlerInstalled = true;
                
                console.log('Instalando manejador de contextmenu');
                document.addEventListener('contextmenu', function(e) {
                    console.log('¡Evento contextmenu detectado!', e);
                }, true);
                
                // Intentar prevenir el menú nativo del navegador
                document.addEventListener('contextmenu', function(e) {
                    e.preventDefault();
                    return false;
                }, false);
            })();
        )";

        frame->ExecuteJavaScript(script, frame->GetURL(), 0);

        if (onLoadEnd)
        {
            onLoadEnd(browser->GetIdentifier(), frame->GetURL());
        }
    }
}

void WebviewHandler::CloseAllBrowsers(bool force_close)
{
    if (browser_map_.empty())
    {
        return;
    }

    for (auto &it : browser_map_)
    {
        it.second.browser->GetHost()->CloseBrowser(force_close);
        it.second.browser = nullptr;
    }
    browser_map_.clear();
}

// static
bool WebviewHandler::IsChromeRuntimeEnabled()
{
    static int value = -1;
    if (value == -1)
    {
        CefRefPtr<CefCommandLine> command_line =
            CefCommandLine::GetGlobalCommandLine();
        value = command_line->HasSwitch("enable-chrome-runtime") ? 1 : 0;
    }
    return value == 1;
}

void WebviewHandler::closeBrowser(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.browser->GetHost()->CloseBrowser(true);
        it->second.browser = nullptr;
        browser_map_.erase(it);
    }
}

void WebviewHandler::createBrowser(std::string url, std::string profileId, std::function<void(int)> callback)
{
#ifndef OS_MAC
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::createBrowser, this, url, profileId, callback));
        return;
    }
#endif
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 30;
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    // Obtener o crear el contexto de solicitud para este perfil
    CefRefPtr<CefRequestContext> context;

    // Si no hay ID de perfil, usar el contexto global
    if (profileId.empty())
    {
        context = CefRequestContext::GetGlobalContext();
    }
    else
    {
        // Buscar si ya existe un contexto para este perfil
        auto it = profile_contexts_.find(profileId);
        if (it != profile_contexts_.end())
        {
            context = it->second;
        }
        else
        {
            // Crear un nuevo contexto para este perfil
            CefRequestContextSettings settings;

            // Configurar rutas específicas para el perfil
            std::string cachePath = "/cache/" + profileId;
            CefString(&settings.cache_path) = cachePath;

            // Crear y almacenar el contexto
            context = CefRequestContext::CreateContext(settings, nullptr);
            profile_contexts_[profileId] = context;
        }
    }

    // Crear el navegador con el contexto específico del perfil
    callback(CefBrowserHost::CreateBrowserSync(
                 window_info,
                 this,
                 url,
                 browser_settings,
                 nullptr,
                 context)
                 ->GetIdentifier());
}

// Método para obtener o crear un contexto de solicitud para un perfil específico
CefRefPtr<CefRequestContext> WebviewHandler::GetRequestContextForProfile(const std::string &profileId)
{
    // Si no hay ID de perfil, usar el contexto global
    if (profileId.empty())
    {
        return CefRequestContext::GetGlobalContext();
    }

    // Buscar si ya existe un contexto para este perfil
    auto it = profile_contexts_.find(profileId);
    if (it != profile_contexts_.end())
    {
        return it->second;
    }

    // Crear un nuevo contexto para este perfil
    CefRequestContextSettings settings;

    // Configurar rutas específicas para el perfil
    std::string cachePath = "/cache/" + profileId;
    CefString(&settings.cache_path) = cachePath;

    // Crear y almacenar el contexto sin un manejador personalizado
    CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, nullptr);
    profile_contexts_[profileId] = context;

    return context;
}

void WebviewHandler::sendScrollEvent(int browserId, int x, int y, int deltaX, int deltaY)
{

    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;

#ifndef __APPLE__
        // The scrolling direction on Windows and Linux is different from MacOS
        deltaY = -deltaY;
        // Flutter scrolls too slowly, it looks more normal by 10x default speed.
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX * 10, deltaY * 10);
#else
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX, deltaY);
#endif
    }
}

void WebviewHandler::changeSize(int browserId, float a_dpi, int w, int h)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.dpi = a_dpi;
        it->second.width = w;
        it->second.height = h;
        it->second.browser->GetHost()->WasResized();
    }
}

void WebviewHandler::cursorClick(int browserId, int x, int y, bool up, int button)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;

        // 0 = botón izquierdo, 1 = botón central, 2 = botón derecho
        if (button == 2)
        {
            // Configurar para clic derecho
            ev.modifiers = EVENTFLAG_RIGHT_MOUSE_BUTTON;

            if (!up)
            {
                // Para clic derecho NO enviamos eventos de mouse al CEF
                // Solo inyectamos código JavaScript

                // Si es clic derecho, registramos para depuración
                std::stringstream js;
                js << "console.log('Clic derecho detectado en (" << x << ", " << y << ")');";

                // Inyectar código JavaScript para crear un evento contextmenu
                js << "(() => {";
                js << "  try {";
                js << "    const evt = new MouseEvent('contextmenu', {";
                js << "      bubbles: true,";
                js << "      cancelable: true,";
                js << "      view: window,";
                js << "      button: 2,";
                js << "      buttons: 2,";
                js << "      clientX: " << x << ",";
                js << "      clientY: " << y;
                js << "    });";
                js << "    const element = document.elementFromPoint(" << x << ", " << y << ");";
                js << "    console.log('Enviando evento contextmenu a elemento:', element);";
                js << "    if (element) {";
                js << "      element.dispatchEvent(evt);";
                js << "      console.log('Evento contextmenu enviado al elemento');";
                js << "    } else {";
                js << "      document.body.dispatchEvent(evt);";
                js << "      console.log('Evento contextmenu enviado al body');";
                js << "    }";
                js << "  } catch(e) {";
                js << "    console.error('Error al enviar evento contextmenu:', e);";
                js << "  }";
                js << "})();";

                it->second.browser->GetMainFrame()->ExecuteJavaScript(
                    js.str(), it->second.browser->GetMainFrame()->GetURL(), 0);
            }
            // No hacemos nada para el evento 'up' del botón derecho
        }
        else
        {
            // Clic izquierdo - Implementar lógica de múltiples clics
            ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;

            if (!up) // Evento de presionar botón
            {
                // Obtener tiempo actual en milisegundos
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

                // Verificar si es un clic múltiple (mismo lugar y en tiempo cercano)
                bool isMultiClick = false;
                if (now - it->second.last_click_time < browser_info::MULTI_CLICK_TIME)
                {
                    // Verificar si el clic está cerca del anterior
                    int dx = std::abs(x - it->second.last_click_x);
                    int dy = std::abs(y - it->second.last_click_y);

                    if (dx <= browser_info::MULTI_CLICK_TOLERANCE &&
                        dy <= browser_info::MULTI_CLICK_TOLERANCE)
                    {
                        isMultiClick = true;
                    }
                }

                // Actualizar recuento de clics
                if (isMultiClick)
                {
                    it->second.click_count++;
                    if (it->second.click_count > 3)
                    {
                        it->second.click_count = 1; // Reiniciar después de triple clic
                    }
                }
                else
                {
                    it->second.click_count = 1; // Nuevo clic simple
                }

                // Guardar información del clic actual
                it->second.last_click_x = x;
                it->second.last_click_y = y;
                it->second.last_click_time = now;

                if (it->second.is_dragging)
                {
                    it->second.browser->GetHost()->DragTargetDrop(ev);
                    it->second.browser->GetHost()->DragSourceSystemDragEnded();
                    it->second.is_dragging = false;
                }
                else
                {
                    // Enviar evento de clic con el recuento correcto
                    it->second.browser->GetHost()->SendMouseClickEvent(
                        ev, CefBrowserHost::MouseButtonType::MBT_LEFT, up, it->second.click_count);
                }
            }
            else // Evento de soltar botón
            {
                if (it->second.is_dragging)
                {
                    it->second.browser->GetHost()->DragTargetDrop(ev);
                    it->second.browser->GetHost()->DragSourceSystemDragEnded();
                    it->second.is_dragging = false;
                }
                else
                {
                    // Enviar evento de soltar con el mismo recuento de clics
                    it->second.browser->GetHost()->SendMouseClickEvent(
                        ev, CefBrowserHost::MouseButtonType::MBT_LEFT, up, it->second.click_count);
                }
            }
        }
    }
}

void WebviewHandler::cursorMove(int browserId, int x, int y, bool dragging)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;
        if (dragging)
        {
            ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        }
        if (it->second.is_dragging && dragging)
        {
            it->second.browser->GetHost()->DragTargetDragOver(ev, DRAG_OPERATION_EVERY);
        }
        else
        {
            it->second.browser->GetHost()->SendMouseMoveEvent(ev, false);
        }
    }
}

bool WebviewHandler::StartDragging(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefDragData> drag_data,
                                   DragOperationsMask allowed_ops,
                                   int x,
                                   int y)
{
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end() && it->second.browser->IsSame(browser))
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;
        ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        it->second.browser->GetHost()->DragTargetDragEnter(drag_data, ev, DRAG_OPERATION_EVERY);
        it->second.is_dragging = true;
    }
    return true;
}

void WebviewHandler::OnImeCompositionRangeChanged(CefRefPtr<CefBrowser> browser, const CefRange &selection_range, const CefRenderHandler::RectList &character_bounds)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup())
    {
        return;
    }
    if (!character_bounds.empty())
    {
        if (it->second.is_ime_commit)
        {
            auto lastCharacter = character_bounds.back();
            it->second.prev_ime_position = lastCharacter;
            onImeCompositionRangeChangedMessage(browser->GetIdentifier(), lastCharacter.x + lastCharacter.width, lastCharacter.y + lastCharacter.height);
            it->second.is_ime_commit = false;
        }
        else
        {
            auto firstCharacter = character_bounds.front();
            if (firstCharacter != it->second.prev_ime_position)
            {
                it->second.prev_ime_position = firstCharacter;
                onImeCompositionRangeChangedMessage(browser->GetIdentifier(), firstCharacter.x, firstCharacter.y + firstCharacter.height);
            }
        }
    }
}

void WebviewHandler::sendKeyEvent(CefKeyEvent &ev)
{
    auto browser = current_focused_browser_;
    if (!browser.get())
    {
        return;
    }
    browser->GetHost()->SendKeyEvent(ev);
}

void WebviewHandler::loadUrl(int browserId, std::string url)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.browser->GetMainFrame()->LoadURL(url);
    }
}

void WebviewHandler::goForward(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.browser->GetMainFrame()->GetBrowser()->GoForward();
    }
}

void WebviewHandler::goBack(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.browser->GetMainFrame()->GetBrowser()->GoBack();
    }
}

void WebviewHandler::reload(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        it->second.browser->GetMainFrame()->GetBrowser()->Reload();
    }
}

void WebviewHandler::openDevTools(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        CefWindowInfo windowInfo;
#ifdef OS_WIN
        windowInfo.SetAsPopup(nullptr, "DevTools");
#endif
        it->second.browser->GetHost()->ShowDevTools(windowInfo, this, CefBrowserSettings(), CefPoint());
    }
}

void WebviewHandler::imeSetComposition(int browserId, std::string text)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
    {
        return;
    }

    CefString cTextStr = CefString(text);

    std::vector<CefCompositionUnderline> underlines;
    cef_composition_underline_t underline = {};
    underline.range.from = 0;
    underline.range.to = static_cast<int>(0 + cTextStr.length());
    underline.color = ColorUNDERLINE;
    underline.background_color = ColorBKCOLOR;
    underline.thick = 0;
    underline.style = CEF_CUS_DOT;
    underlines.push_back(underline);

    // Keeps the caret at the end of the composition
    auto selection_range_end = static_cast<int>(0 + cTextStr.length());
    CefRange selection_range = CefRange(0, selection_range_end);
    it->second.browser->GetHost()->ImeSetComposition(cTextStr, underlines, CefRange(UINT32_MAX, UINT32_MAX), selection_range);
}

void WebviewHandler::imeCommitText(int browserId, std::string text)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
    {
        return;
    }

    CefString cTextStr = CefString(text);
    it->second.is_ime_commit = true;

    std::vector<CefCompositionUnderline> underlines;
    auto selection_range_end = static_cast<int>(0 + cTextStr.length());
    CefRange selection_range = CefRange(selection_range_end, selection_range_end);
#ifndef _WIN32
    it->second.browser->GetHost()->ImeSetComposition(cTextStr, underlines, CefRange(UINT32_MAX, UINT32_MAX), selection_range);
#endif
    it->second.browser->GetHost()->ImeCommitText(cTextStr, CefRange(UINT32_MAX, UINT32_MAX), 0);
}

void WebviewHandler::setClientFocus(int browserId, bool focus)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
    {
        return;
    }
    it->second.browser->GetHost()->SetFocus(focus);
}

void WebviewHandler::setCookie(const std::string &domain, const std::string &key, const std::string &value)
{
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager)
    {
        CefCookie cookie;
        CefString(&cookie.path).FromASCII("/");
        CefString(&cookie.name).FromString(key.c_str());
        CefString(&cookie.value).FromString(value.c_str());

        if (!domain.empty())
        {
            CefString(&cookie.domain).FromString(domain.c_str());
        }

        cookie.httponly = true;
        cookie.secure = false;
        std::string httpDomain = "https://" + domain + "/cookiestorage";
        manager->SetCookie(httpDomain, cookie, nullptr);
    }
}

void WebviewHandler::deleteCookie(const std::string &domain, const std::string &key)
{
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager)
    {
        std::string httpDomain = "https://" + domain + "/cookiestorage";
        manager->DeleteCookies(httpDomain, key, nullptr);
    }
}

void WebviewHandler::visitAllCookies(std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback)
{
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
    {
        return;
    }

    CefRefPtr<WebviewCookieVisitor> cookieVisitor = new WebviewCookieVisitor();
    cookieVisitor->setOnVisitComplete(callback);

    manager->VisitAllCookies(cookieVisitor);
}

void WebviewHandler::visitUrlCookies(const std::string &domain, const bool &isHttpOnly, std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback)
{
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
    {
        return;
    }

    CefRefPtr<WebviewCookieVisitor> cookieVisitor = new WebviewCookieVisitor();
    cookieVisitor->setOnVisitComplete(callback);

    std::string httpDomain = "https://" + domain + "/cookiestorage";

    manager->VisitUrlCookies(httpDomain, isHttpOnly, cookieVisitor);
}

void WebviewHandler::setJavaScriptChannels(int browserId, const std::vector<std::string> channels)
{
    std::string extensionCode = "try{";
    for (auto &channel : channels)
    {
        extensionCode += channel;
        extensionCode += " = (e,r) => {external.JavaScriptChannel('";
        extensionCode += channel;
        extensionCode += "',e,r)};";
    }
    extensionCode += "}catch(e){console.log(e);}";
    executeJavaScript(browserId, extensionCode);
}

void WebviewHandler::sendJavaScriptChannelCallBack(const bool error, const std::string result, const std::string callbackId, const int browserId, const std::string frameId)
{
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kExecuteJsCallbackMessage);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetInt(0, atoi(callbackId.c_str()));
    args->SetBool(1, error);
    args->SetString(2, result);
    auto bit = browser_map_.find(browserId);
    if (bit != browser_map_.end())
    {
        int64_t frameIdInt = atoll(frameId.c_str());

        CefRefPtr<CefFrame> frame = bit->second.browser->GetMainFrame();

        // Return types for frame->GetIdentifier() changed, use the Linux way when updating MacOS or Windows
        // versions in download.cmake
#if __linux__
        bool identifierMatch = std::stoll(frame->GetIdentifier().ToString()) == frameIdInt;
#else
        bool identifierMatch = frame->GetIdentifier() == frameIdInt;
#endif
        if (identifierMatch)
        {
            frame->SendProcessMessage(PID_RENDERER, message);
        }
    }
}

static std::string GetCallbackId()
{
    auto time = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    time_t timestamp = time.time_since_epoch().count();
    return std::to_string(timestamp);
}

void WebviewHandler::executeJavaScript(int browserId, const std::string code, std::function<void(CefRefPtr<CefValue>)> callback)
{
    if (!code.empty())
    {
        auto bit = browser_map_.find(browserId);
        if (bit != browser_map_.end() && bit->second.browser.get())
        {
            CefRefPtr<CefFrame> frame = bit->second.browser->GetMainFrame();
            if (frame)
            {
                std::string finalCode = code;
                if (callback != nullptr)
                {
                    std::string callbackId = GetCallbackId();

                    finalCode = "external.EvaluateCallback('";
                    finalCode += callbackId;
                    finalCode += "',(function(){return ";
                    finalCode += code;
                    finalCode += "})());";
                    js_callbacks_[callbackId] = callback;
                }
                frame->ExecuteJavaScript(finalCode, frame->GetURL(), 0);
            }
        }
    }
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
    // Limpiar el modelo de menú para que no se muestre el menú contextual nativo
    model->Clear();
}

bool WebviewHandler::RunContextMenu(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefContextMenuParams> params,
                                    CefRefPtr<CefMenuModel> model,
                                    CefRefPtr<CefRunContextMenuCallback> callback)
{
    // Cancelar inmediatamente el menú contextual
    callback->Cancel();

    // Ahora vamos a inyectar JavaScript para simular un evento contextmenu
    std::stringstream js;
    js << "(() => {";
    js << "  const evt = new MouseEvent('contextmenu', {";
    js << "    bubbles: true,";
    js << "    cancelable: true,";
    js << "    clientX: " << params->GetXCoord() << ",";
    js << "    clientY: " << params->GetYCoord() << ",";
    js << "    button: 2,"; // Botón derecho
    js << "    buttons: 2"; // Estado de botones: botón derecho presionado
    js << "  });";
    js << "  const element = document.elementFromPoint(" << params->GetXCoord() << ", " << params->GetYCoord() << ");";
    js << "  if (element) {";
    js << "    element.dispatchEvent(evt);";
    js << "  } else {";
    js << "    document.body.dispatchEvent(evt);";
    js << "  }";
    js << "  console.log('Dispatched contextmenu event at (" << params->GetXCoord() << ", " << params->GetYCoord() << ")');";
    js << "})();";

    frame->ExecuteJavaScript(js.str(), frame->GetURL(), 0);

    // Devolvemos true para indicar que hemos manejado el menú contextual
    return true;
}
