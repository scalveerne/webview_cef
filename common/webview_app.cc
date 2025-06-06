#include "webview_app.h"

#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>

#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

namespace
{

    // Función auxiliar para registrar eventos en un archivo log
    void LogEvent(const std::string &event_type, const std::string &details)
    {
        std::ofstream logfile("cef_windows.log", std::ios::app);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);

        // Usar una versión segura en Windows para evitar advertencia C4996
        char time_buf[26];
#ifdef _WIN32
        ctime_s(time_buf, sizeof(time_buf), &time);
        std::string time_str(time_buf);
#else
        std::string time_str(ctime(&time));
#endif

        // Quitar el salto de línea al final que añade ctime
        if (!time_str.empty() && time_str[time_str.length() - 1] == '\n')
        {
            time_str.erase(time_str.length() - 1);
        }

        logfile << time_str << " [" << event_type << "] " << details << std::endl;
        std::cout << "[CEF] " << event_type << ": " << details << std::endl;
    }

    // When using the Views framework this object provides the delegate
    // implementation for the CefWindow that hosts the Views-based browser.
    class SimpleWindowDelegate : public CefWindowDelegate
    {
    public:
        explicit SimpleWindowDelegate(CefRefPtr<CefBrowserView> browser_view)
            : browser_view_(browser_view)
        {
            LogEvent("WindowDelegate", "Nuevo delegado de ventana creado");
        }

        void OnWindowCreated(CefRefPtr<CefWindow> window) override
        {
            LogEvent("WindowCreated", "Ventana principal creada");

            // Add the browser view and show the window.
            window->AddChildView(browser_view_);
            LogEvent("WindowConfig", "Vista del navegador añadida a la ventana");

            window->Show();
            LogEvent("WindowShown", "Ventana mostrada al usuario");

            // Give keyboard focus to the browser view.
            browser_view_->RequestFocus();
        }

        void OnWindowDestroyed(CefRefPtr<CefWindow> window) override
        {
            LogEvent("WindowDestroyed", "Ventana destruida");
            browser_view_ = nullptr;
        }

        bool CanClose(CefRefPtr<CefWindow> window) override
        {
            // Allow the window to close if the browser says it's OK.
            CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
            if (browser)
                return browser->GetHost()->TryCloseBrowser();
            return true;
        }

        CefSize GetPreferredSize(CefRefPtr<CefView> view) override
        {
            return CefSize(1280, 720);
        }

    private:
        CefRefPtr<CefBrowserView> browser_view_;

        IMPLEMENT_REFCOUNTING(SimpleWindowDelegate);
        DISALLOW_COPY_AND_ASSIGN(SimpleWindowDelegate);
    };

    class SimpleBrowserViewDelegate : public CefBrowserViewDelegate
    {
    public:
        SimpleBrowserViewDelegate()
        {
            LogEvent("BrowserViewDelegate", "Nuevo delegado de vista de navegador creado");
        }

        bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view,
                                       CefRefPtr<CefBrowserView> popup_browser_view,
                                       bool is_devtools) override
        {
            // Log information about the popup creation
            std::string popup_type = is_devtools ? "DevTools" : "Normal Popup";
            LogEvent("PopupCreated", "Nueva ventana emergente creada (" + popup_type + ")");

            // Create a new top-level Window for the popup. It will show itself after
            // creation.
            CefWindow::CreateTopLevelWindow(
                new SimpleWindowDelegate(popup_browser_view));

            LogEvent("PopupWindow", "Ventana emergente enviada para creación");

            // We created the Window.
            return true;
        }

    private:
        IMPLEMENT_REFCOUNTING(SimpleBrowserViewDelegate);
        DISALLOW_COPY_AND_ASSIGN(SimpleBrowserViewDelegate);
    };

} // namespace

WebviewApp::WebviewApp(CefRefPtr<WebviewHandler> handler)
{
    m_handler = handler;
    LogEvent("AppInit", "Aplicación WebviewApp inicializada");
}

WebviewApp::ProcessType WebviewApp::GetProcessType(CefRefPtr<CefCommandLine> command_line)
{
    // The command-line flag won't be specified for the browser process.
    if (!command_line->HasSwitch("type"))
    {
        LogEvent("ProcessType", "Proceso de navegador principal detectado");
        return BrowserProcess;
    }

    const std::string &process_type = command_line->GetSwitchValue("type");
    if (process_type == "renderer")
    {
        LogEvent("ProcessType", "Proceso de renderizado detectado");
        return RendererProcess;
    }

#if defined(OS_LINUX)
    else if (process_type == "zygote")
    {
        LogEvent("ProcessType", "Proceso zygote detectado");
        return ZygoteProcess;
    }
#endif
    LogEvent("ProcessType", "Otro tipo de proceso detectado: " + process_type);
    return OtherProcess;
}

void WebviewApp::OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line)
{
    // Registrar el proceso que se está iniciando
    std::string process_type_str = process_type.ToString();
    if (process_type_str.empty())
    {
        process_type_str = "browser";
    }
    LogEvent("CommandLine", "Procesando línea de comandos para proceso: " + process_type_str);

    // Pass additional command-line flags to the browser process.
    if (process_type.empty())
    {
        if (!m_bEnableGPU)
        {
            command_line->AppendSwitch("disable-gpu");
            command_line->AppendSwitch("disable-gpu-compositing");
        }
        // El más importante para CEF 103

        command_line->AppendSwitch("allow-running-insecure-content");
        command_line->AppendSwitch("disable-popup-blocking");        // allow running insecure content in secure pages
        command_line->AppendSwitch("disable-gpu-shader-disk-cache"); // disable gpu shader disk cache
        command_line->AppendSwitch("no-sanbox");

        // Agregar parámetro para detener el audio en pestañas cerradas o inactivas
        command_line->AppendSwitch("disable-background-media-suspend");

        command_line->AppendSwitchWithValue("enable-features", "NetworkService,NetworkServiceInProcess");
        command_line->AppendSwitch("enable-chrome-runtime");
        command_line->AppendSwitch("ignore-certificate-errors"); // ignore certificate errors

        // http://www.chromium.org/developers/design-documents/process-models
        if (m_uMode == 1)
        {
            command_line->AppendSwitch("process-per-site");                     // each site in its own process
            command_line->AppendSwitchWithValue("renderer-process-limit", "8"); // limit renderer process count to decrease memory usage
        }
        else if (m_uMode == 2)
        {
            command_line->AppendSwitch("process-per-tab"); // each tab in its own process
        }
        else if (m_uMode == 3)
        {
            command_line->AppendSwitch("single-process"); // all in one process
        }
        command_line->AppendSwitch("enable-webgl");
        command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

        // Support cross domain requests
        std::string values = command_line->GetSwitchValue("disable-features");
        if (values == "")
        {
            values = "SameSiteByDefaultCookies,CookiesWithoutSameSiteMustBeSecure";
        }
        else
        {
            values += ",SameSiteByDefaultCookies,CookiesWithoutSameSiteMustBeSecure";
        }
        if (values.find("CalculateNativeWinOcclusion") == size_t(-1))
        {
            values += ",CalculateNativeWinOcclusion";
        }

        command_line->AppendSwitchWithValue("disable-features", values);
        // for unsafe domain, add domain to whitelist
        if (!m_strFilterDomain.empty())
        {
            command_line->AppendSwitch("ignore-certificate-errors"); // ignore certificate errors
            command_line->AppendSwitchWithValue("unsafely-treat-insecure-origin-as-secure",
                                                m_strFilterDomain);
        }

        // MEJORA 6: Configuraciones adicionales para Cloudflare
        command_line->AppendSwitch("disable-features=IsolateOrigins,site-per-process");
        command_line->AppendSwitchWithValue("disable-site-isolation-trials", "true");
        command_line->AppendSwitchWithValue("disable-site-isolation-for-policy", "true");
    }

#ifdef APPLE
    command_line->AppendSwitch("use-mock-keychain");
    command_line->AppendSwitch("single-process");
#endif
#ifdef linux

#endif
}

void WebviewApp::OnContextInitialized()
{
    CEF_REQUIRE_UI_THREAD();
    //    CefBrowserSettings browser_settings;
    //    browser_settings.windowless_frame_rate = 60;
    //
    //    CefWindowInfo window_info;
    //    window_info.SetAsWindowless(0);
    //
    //    // create browser
    //    CefBrowserHost::CreateBrowser(window_info, m_handler, "", browser_settings, nullptr, nullptr);
}

// CefRefPtr<CefClient> WebviewApp::GetDefaultClient() {
//     // Called when a new browser window is created via the Chrome runtime UI.
//     return WebviewHandler::GetInstance();
// }

void WebviewApp::SetUnSafelyTreatInsecureOriginAsSecure(const CefString &strFilterDomain)
{
    m_strFilterDomain = strFilterDomain;
}

void WebviewApp::OnWebKitInitialized()
{
    // inject js function for jssdk
    std::string extensionCode = R"(
var external = {};
var clientSdk = {};
(() => {
clientSdk.jsCmd = (functionName, arg1, arg2, arg3) => {
if (typeof arg1 === 'function') {
native function jsCmd(functionName, arg1);
return jsCmd(functionName, arg1);
}
else if	 (typeof arg2 === 'function') {
jsonString = arg1;
if	(typeof arg1 !== 'string'){
jsonString = JSON.stringify(arg1);
}
native function jsCmd(functionName, jsonString, arg2);
return jsCmd(functionName, jsonString, arg2);
}
else if	 (typeof arg3 === 'function') {
jsonString = arg1;
if	(typeof arg1 !== 'string'){
jsonString = JSON.stringify(arg1);
}
native function jsCmd(functionName, jsonString, arg2, arg3);
return jsCmd(functionName, jsonString, arg2, arg3);
}else {

}
			};

            external.JavaScriptChannel = (n,e,r) => {
                var a; 
                null == r ? a = '' : (a = '_' + new Date + (1e3 + Math.floor(8999 * Math.random())), window[a] = function (n, e) { 
                    return function () { 
                        try {
                            e && e.call && e.call(null, arguments[1]) 
                        } finally {
                            delete window[n]
                        } 
                    } 
                }(a, r)); 
                try {
                    external.StartRequest(external.GetNextReqID(), n, a, JSON.stringify(e || {}), '') 
                } catch (l) {
                    console.log('messeage send')
                }
            }

            external.EvaluateCallback = (nReqID, result) => {
                native function EvaluateCallback();
                EvaluateCallback(nReqID, result);
            }

			external.StartRequest  = (nReqID, strCmd, strCallBack, strArgs, strLog) => {
				native function StartRequest();
				StartRequest(nReqID, strCmd, strCallBack, strArgs, strLog);
			};
			external.GetNextReqID  = () => {
			  native function GetNextReqID();
			  return GetNextReqID();
			};
		})();
	 )";

    CefRefPtr<CefJSHandler> handler = new CefJSHandler();

    if (!m_render_js_bridge.get())
        m_render_js_bridge.reset(new CefJSBridge);
    handler->AttachJSBridge(m_render_js_bridge);

    CefRegisterExtension("v8/extern", extensionCode, handler);
}

void WebviewApp::OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra_info)
{
    LogEvent("BrowserCreated", "Navegador creado con ID: " +
                                   std::to_string(browser->GetIdentifier()));

    if (!m_render_js_bridge.get())
    {
        m_render_js_bridge.reset(new CefJSBridge);
    }
}

void WebviewApp::SetProcessMode(uint32_t uMode)
{
    m_uMode = uMode;
}

void WebviewApp::SetEnableGPU(bool bEnable)
{
    m_bEnableGPU = bEnable;
}

void WebviewApp::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line)
{
    // Registrar el lanzamiento de un proceso hijo
    const std::string &process_type = command_line->GetSwitchValue("type");
    LogEvent("ChildProcess", "Lanzando proceso hijo de tipo: " + process_type);

    // Aplicar headless a TODOS los procesos utility y GPU, sin importar cuándo se lancen
    if (process_type == "utility")
    {
        command_line->AppendSwitch("headless");
        command_line->AppendSwitch("hide-scrollbars");
        command_line->AppendSwitch("mute-audio");
        command_line->AppendSwitch("disable-gpu");
        LogEvent("ChildProcess", "Aplicando modo headless a proceso: " + process_type);
    }
}

void WebviewApp::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser)
{
}

void WebviewApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
}

void WebviewApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (m_render_js_bridge.get())
    {
        m_render_js_bridge->RemoveCallbackFuncWithFrame(frame);
    }
}

void WebviewApp::OnUncaughtException(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Exception> exception, CefRefPtr<CefV8StackTrace> stackTrace)
{
}

void WebviewApp::OnFocusedNodeChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefDOMNode> node)
{
    // Get node attribute
    bool is_editable = (node.get() && node->IsEditable());
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kFocusedNodeChangedMessage);
    message->GetArgumentList()->SetBool(0, is_editable);
    if (is_editable)
    {
        CefRect rect = node->GetElementBounds();
        message->GetArgumentList()->SetInt(1, rect.x);
        message->GetArgumentList()->SetInt(2, rect.y + rect.height);
    }
    frame->SendProcessMessage(PID_BROWSER, message);
}

bool WebviewApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    const CefString &message_name = message->GetName();
    if (message_name == kExecuteJsCallbackMessage)
    {
        int callbackId = message->GetArgumentList()->GetInt(0);
        bool error = message->GetArgumentList()->GetBool(1);
        CefString result = message->GetArgumentList()->GetString(2);
        if (m_render_js_bridge.get())
        {
            m_render_js_bridge->ExecuteJSCallbackFunc(callbackId, error, result);
        }
    }

    return false;
}