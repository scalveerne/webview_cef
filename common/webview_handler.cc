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
#include <cmath>      // Para std::abs
#include <filesystem> // C++17

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>                 // Para SHGetFolderPath
#pragma comment(lib, "shell32.lib") // Enlazar con shell32.lib para SHGetFolderPath
#endif

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
    CefString message_name = message->GetName();
    if (message_name.ToString() == kFocusedNodeChangedMessage)
    {
        current_focused_browser_ = browser;
        bool editable = message->GetArgumentList()->GetBool(0);
        onFocusedNodeChangeMessage(browser->GetIdentifier(), editable);
        if (editable)
        {
            onImeCompositionRangeChangedMessage(browser->GetIdentifier(), message->GetArgumentList()->GetInt(1), message->GetArgumentList()->GetInt(2));
        }
    }
    else if (message_name.ToString() == kJSCallCppFunctionMessage)
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
    else if (message_name.ToString() == kEvaluateCallbackMessage)
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

    // Mejorar detección de sitios de Cloudflare y permitir popups
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
        // Permitir javascript en el popup
        if (no_javascript_access)
        {
            *no_javascript_access = false;
        }

        // Conservar todas las características del popup para Cloudflare
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

        // Mejorar soporte para iframes de Cloudflare
        std::string cloudflareSupport = R"(
            (function() {
                // Permitir scripts en iframes (especialmente para Cloudflare)
                try {
                    // Función para ayudar con los desafíos de Cloudflare
                    window.__cfHelperFunction = function() {
                        try {
                            // Detectar iframes de Cloudflare
                            const observer = new MutationObserver(function(mutations) {
                                for (const mutation of mutations) {
                                    if (mutation.addedNodes) {
                                        mutation.addedNodes.forEach(function(node) {
                                            if (node.tagName === 'IFRAME') {
                                                try {
                                                    // Permitir permisos para iframes de Cloudflare
                                                    if (node.src && (
                                                        node.src.includes('cloudflare') ||
                                                        node.src.includes('captcha') ||
                                                        node.src.includes('challenge') ||
                                                        node.src.includes('turnstile') ||
                                                        node.src.includes('cf-') ||
                                                        node.src.includes('__cf')
                                                    )) {
                                                        console.log('Configurando iframe de Cloudflare:', node.src);
                                                        node.setAttribute('sandbox', 'allow-forms allow-modals allow-orientation-lock allow-pointer-lock allow-popups allow-popups-to-escape-sandbox allow-presentation allow-same-origin allow-scripts');
                                                    }
                                                } catch(e) {
                                                    console.error('Error configurando iframe:', e);
                                                }
                                            }
                                        });
                                    }
                                }
                            });
                            
                            observer.observe(document.documentElement, {
                                childList: true,
                                subtree: true
                            });

                            // Funciones de ayuda para Cloudflare
                            if (window.navigator && typeof navigator.userAgent === 'string' && navigator.userAgent.toLowerCase().includes('headless')) {
                                const originalUserAgent = navigator.userAgent;
                                Object.defineProperty(navigator, 'userAgent', {
                                    get: function() {
                                        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/101.0.4951.67 Safari/537.36";
                                    }
                                });
                            }

                            // Intentar prevenir detección de webdriver
                            if (navigator.webdriver === true) {
                                Object.defineProperty(navigator, 'webdriver', {
                                    get: () => false
                                });
                            }

                            // Prevenir detección de navegador automatizado
                            if (typeof navigator.plugins !== 'undefined') {
                                if (navigator.plugins.length === 0) {
                                    Object.defineProperty(navigator, 'plugins', {
                                        get: () => [1, 2, 3, 4, 5]
                                    });
                                }
                            }
                        } catch(e) {
                            console.error('Error en helper de Cloudflare:', e);
                        }
                    };

                    // Ejecutar inmediatamente
                    window.__cfHelperFunction();

                    // Ejecutar también cuando la página esté completamente cargada
                    if (document.readyState === 'complete') {
                        window.__cfHelperFunction();
                    } else {
                        window.addEventListener('load', window.__cfHelperFunction);
                    }
                } catch(e) {
                    console.error('Error en soporte de Cloudflare:', e);
                }
            })();
        )";

        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        frame->ExecuteJavaScript(cloudflareSupport, frame->GetURL(), 0);

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

void createCacheDirectory(const std::string &cachePath)
{
    try
    {
        std::filesystem::create_directories(cachePath);
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        std::cerr << "Error creating cache directory: " << e.what() << std::endl;
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
    browser_settings.javascript = STATE_ENABLED;

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    // Obtener contexto de solicitud utilizando la función existente
    CefRefPtr<CefRequestContext> context = GetRequestContextForProfile(profileId);

    // Crear diccionario para extra_info
    CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
    // extra_info->SetString("user-agent", user_agent);

    // Crear el navegador con el contexto específico del perfil
    callback(CefBrowserHost::CreateBrowserSync(
                 window_info,
                 this,
                 url,
                 browser_settings,
                 extra_info,
                 context)
                 ->GetIdentifier());
}

// Método para obtener o crear un contexto de solicitud para un perfil específico
CefRefPtr<CefRequestContext> WebviewHandler::GetRequestContextForProfile(const std::string &profileId)
{
    // Si no hay ID de perfil, usar el contexto global
    if (profileId.empty())
    {
        std::cout << "Usando contexto global para perfil vacío" << std::endl;
        return CefRequestContext::GetGlobalContext();
    }

    // Buscar si ya existe un contexto para este perfil
    auto it = profile_contexts_.find(profileId);
    if (it != profile_contexts_.end())
    {
        std::cout << "Reutilizando contexto existente para perfil: " << profileId << std::endl;
        return it->second;
    }

    std::cout << "Creando nuevo contexto para perfil: " << profileId << std::endl;

    // Crear un nuevo contexto para este perfil
    CefRequestContextSettings settings;

    // Configurar rutas específicas para el perfil
    std::string cachePath;

#ifdef _WIN32
    // Obtener directorio de la aplicación en lugar de AppData
    char appPath[MAX_PATH];
    GetModuleFileNameA(NULL, appPath, MAX_PATH);
    std::string exePath(appPath);
    size_t lastSlash = exePath.find_last_of("\\/");

    // Crear una ruta que sea estable y con permisos garantizados
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath))
    {
        // Usar TEMP que siempre tiene permisos
        cachePath = std::string(tempPath) + "ScalBrowser\\profiles";
    }
    else
    {
        // Fallback al directorio de la app
        cachePath = exePath.substr(0, lastSlash) + "\\Cache";
    }

    // Usar directamente el ID del perfil (limitado a 20 caracteres por seguridad)
    std::string safeProfileId = profileId;
    if (safeProfileId.length() > 20)
    {
        safeProfileId = safeProfileId.substr(0, 20);
    }

    // Reemplazar caracteres problemáticos
    std::replace(safeProfileId.begin(), safeProfileId.end(), '/', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '\\', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), ':', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '*', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '?', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '"', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '<', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '>', '_');
    std::replace(safeProfileId.begin(), safeProfileId.end(), '|', '_');

    cachePath += "\\" + safeProfileId;

    std::cout << "Intentando crear caché en: " << cachePath << std::endl;
#else
    // Código para Linux/Mac similar...
#endif

    // Intentar crear el directorio con mejor manejo de errores
    bool directoryCreated = false;
    try
    {
        directoryCreated = std::filesystem::create_directories(cachePath);
        std::cout << "Directorio creado exitosamente: " << (directoryCreated ? "SÍ" : "NO (ya existía)") << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR creando directorio: " << cachePath << " - " << e.what() << std::endl;

        // Intentar con una ubicación alternativa
        char tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        cachePath = std::string(tempPath) + "ScalBrowser_fallback\\" + safeProfileId;

        std::cout << "Intentando ubicación alternativa: " << cachePath << std::endl;

        try
        {
            directoryCreated = std::filesystem::create_directories(cachePath);
            std::cout << "Directorio alternativo creado: " << (directoryCreated ? "SÍ" : "NO") << std::endl;
        }
        catch (...)
        {
            std::cerr << "ERROR FATAL: No se pudo crear ningún directorio para el perfil" << std::endl;
            return CefRequestContext::GetGlobalContext(); // Fallar de forma segura usando el contexto global
        }
    }

    CefString(&settings.cache_path) = cachePath;

    // Más configuraciones de perfil para mejorar estabilidad
    settings.persist_session_cookies = 1;
    settings.persist_user_preferences = 1;

    // Crear y almacenar el contexto
    std::cout << "Creando contexto de CEF con ruta: " << cachePath << std::endl;
    CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, nullptr);
    if (context)
    {
        std::cout << "Contexto creado exitosamente para perfil: " << profileId << std::endl;
        profile_contexts_[profileId] = context;
    }
    else
    {
        std::cerr << "FALLO al crear contexto para perfil: " << profileId << std::endl;
    }

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

        // Iniciar siempre el estado de desplazamiento con un evento de rueda de valor cero
        // Esto asegura que se inicialice correctamente el estado is_in_gesture_scroll_
        static bool gesture_initialized = false;

        if (!gesture_initialized)
        {
            // Enviar un evento inicial con delta cero para inicializar el estado de gesto
            it->second.browser->GetHost()->SendMouseWheelEvent(ev, 0, 0);
            gesture_initialized = true;
        }

#ifndef __APPLE__
        // The scrolling direction on Windows and Linux is different from MacOS
        deltaY = -deltaY;
        // Flutter scrolls too slowly, usar un multiplicador más conservador
        // Reducir de 10x a 3x para que el comportamiento sea más natural y menos detectable
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX * 3, deltaY * 3);
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

// Helper para convertir nuestros modificadores de teclado a los de CEF
uint32_t convertKeyModifiers(KeyboardModifiers modifiers)
{
    uint32_t cef_modifiers = 0;
    if (modifiers & kShiftKey)
        cef_modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (modifiers & kControlKey)
        cef_modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (modifiers & kAltKey)
        cef_modifiers |= EVENTFLAG_ALT_DOWN;
    if (modifiers & kMetaKey)
        cef_modifiers |= EVENTFLAG_COMMAND_DOWN;
    return cef_modifiers;
}

void WebviewHandler::cursorClick(int browserId, int x, int y, bool up, int button)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;

        // Asegurar que click_count siempre sea al menos 1
        if (it->second.click_count < 1)
        {
            it->second.click_count = 1;
        }

        // 0 = botón izquierdo, 1 = botón central, 2 = botón derecho
        if (button == 2)
        {
            // Para clic derecho, NO enviamos eventos de mouse al CEF
            // Solo inyectamos JavaScript para disparar el evento contextmenu

            if (!up) // Solo procesamos el evento de presionar, no el de soltar
            {
                try
                {
                    // Inyectar código JavaScript para crear un evento contextmenu
                    std::stringstream js;
                    js << "(() => {";
                    js << "  try {";
                    js << "    console.log('Right-click detected at (" << x << ", " << y << ")');";
                    js << "    const evt = new MouseEvent('contextmenu', {";
                    js << "      bubbles: true,";
                    js << "      cancelable: true,";
                    js << "      clientX: " << x << ",";
                    js << "      clientY: " << y << ",";
                    js << "      button: 2,"; // Botón derecho
                    js << "      buttons: 2"; // Estado de botones: botón derecho presionado
                    js << "    });";
                    js << "    let element = document.elementFromPoint(" << x << ", " << y << ");";
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
                    js << "  } catch(e) {";
                    js << "    console.error('Error in right-click handler:', e);";
                    js << "  }";
                    js << "})();";

                    CefRefPtr<CefFrame> frame = it->second.browser->GetMainFrame();
                    if (frame)
                    {
                        frame->ExecuteJavaScript(js.str(), frame->GetURL(), 0);
                    }
                }
                catch (...)
                {
                    // Capturar cualquier excepción para evitar que la aplicación se cierre
                }
            }
            // No hacemos nada para el evento 'up' del botón derecho
        }
        else
        {
            // Clic izquierdo - Implementar lógica de múltiples clics
            ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;

            if (!up) // Evento de presionar botón
            {
                try
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
                }
                catch (...)
                {
                    // Si algo falla, asegurar click_count = 1
                    it->second.click_count = 1;
                }

                // Asegurar que click_count siempre sea al menos 1
                if (it->second.click_count < 1)
                {
                    it->second.click_count = 1;
                }

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
                // Asegurar que click_count siempre sea al menos 1
                if (it->second.click_count < 1)
                {
                    it->second.click_count = 1;
                }

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

        // Generar un evento intermedio de inicio de desplazamiento para evitar errores de is_in_gesture_scroll_
        // Solo en caso de desplazamiento
        static bool last_dragging_state = false;
        if (dragging && !last_dragging_state)
        {
            // Iniciar el estado de desplazamiento con un evento de rueda simulado
            CefMouseEvent scroll_ev;
            scroll_ev.x = x;
            scroll_ev.y = y;
            it->second.browser->GetHost()->SendMouseWheelEvent(scroll_ev, 0, 0);
        }
        last_dragging_state = dragging;

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
            bool positionChanged = (lastCharacter.x != it->second.prev_ime_position.x) ||
                                   (lastCharacter.y != it->second.prev_ime_position.y);
            if (positionChanged)
            {
                it->second.prev_ime_position = lastCharacter;
                onImeCompositionRangeChangedMessage(browser->GetIdentifier(),
                                                    lastCharacter.x + lastCharacter.width,
                                                    lastCharacter.y + lastCharacter.height);
            }
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

#if defined(OS_WIN) || defined(OS_MAC)
        bool identifierMatch = frame->GetIdentifier().ToString() == std::to_string(frameIdInt);
#else
        bool identifierMatch = std::stoll(frame->GetIdentifier().ToString()) == frameIdInt;
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
