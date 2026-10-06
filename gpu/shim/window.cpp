// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    const char* input_mode=std::getenv("BB_INPUT_MODE");
    keyboard_mouse=!input_mode || (std::strcmp(input_mode,"legacy") && std::strcmp(input_mode,"gamepad"));
    force_keyboard_mouse=input_mode && !std::strcmp(input_mode,"kbm");
    const char* capture=std::getenv("BB_MOUSE_CAPTURE");
    mouse_requested=!capture || std::strcmp(capture,"0");
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef __APPLE__
    if (driver && !std::strcmp(driver, "cocoa")) {
        window_info.type = WindowSystemType::Metal;
        metal_view = SDL_Metal_CreateView(window);
        ASSERT_MSG(metal_view, "Failed to create Metal view: {}", SDL_GetError());
        window_info.render_surface = SDL_Metal_GetLayer(metal_view);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
#ifdef __APPLE__
    if (metal_view) SDL_Metal_DestroyView(metal_view);
#endif
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
}

bool WindowSDL::PollEvents() {
    // Opt-in automation uses the same shutdown path as closing this window.
    static const char* quit_file=std::getenv("BB_QUIT_FILE");
    if (quit_file && *quit_file && std::remove(quit_file)==0) return false;
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat && event.key.scancode>SDL_SCANCODE_UNKNOWN && event.key.scancode<512) {
                std::scoped_lock lock{input_mutex};
                host_input.pressed_keys[event.key.scancode]=1;
            }
            if (!event.key.repeat && event.key.key==SDLK_F8 && keyboard_mouse) {
                mouse_requested=!mouse_captured;
                force_keyboard_mouse=true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (mouse_captured && event.button.button>=1 && event.button.button<=32) {
                std::scoped_lock lock{input_mutex};
                host_input.pressed_mouse_buttons|=SDL_BUTTON_MASK(event.button.button);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse_captured) {
                std::scoped_lock lock{input_mutex};
                host_input.mouse_x+=event.motion.xrel;
                host_input.mouse_y+=event.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mouse_captured) {
                std::scoped_lock lock{input_mutex};
                const float y=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                host_input.wheel+=y>0 ? 1 : y<0 ? -1 : 0;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    const bool focused=SDL_GetKeyboardFocus()==window;
    const bool gameplay=focused && !text_active && !BbOverlay::CapturesInput();
    const bool want_mouse=gameplay && keyboard_mouse && mouse_requested &&
                          (force_keyboard_mouse || !SDL_HasGamepad());
    if (want_mouse!=mouse_captured) {
        if (SDL_SetWindowRelativeMouseMode(window,want_mouse)) mouse_captured=want_mouse;
        else LOG_WARNING(Frontend,"Mouse capture unavailable: {}",SDL_GetError());
        std::scoped_lock lock{input_mutex};
        host_input.mouse_x=host_input.mouse_y=0;
        host_input.wheel=0;
        host_input.pressed_mouse_buttons=0;
    }
    {
        std::scoped_lock lock{input_mutex};
        host_input.focused=gameplay;
        host_input.mouse_captured=mouse_captured && gameplay;
        int count=0;
        const bool* keys=SDL_GetKeyboardState(&count);
        static_assert(SDL_SCANCODE_COUNT<=sizeof(host_input.keys));
        for (int i=0;i<512;++i) host_input.keys[i]=gameplay && i<count && keys[i];
        host_input.mouse_buttons=host_input.mouse_captured ? SDL_GetMouseState(nullptr,nullptr) : 0;
        if (!gameplay) {
            host_input.mouse_x=host_input.mouse_y=0; host_input.wheel=0;
            std::memset(host_input.pressed_keys,0,sizeof(host_input.pressed_keys));
            host_input.pressed_mouse_buttons=0;
        }
    }
    return is_open;
}

bool WindowSDL::ReadHostInput(BbHostInput& input) {
    std::scoped_lock lock{input_mutex};
    input=host_input;
    host_input.mouse_x=host_input.mouse_y=0;
    host_input.wheel=0;
    std::memset(host_input.pressed_keys,0,sizeof(host_input.pressed_keys));
    host_input.pressed_mouse_buttons=0;
    return true;
}

} // namespace Frontend
