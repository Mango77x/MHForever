#include "host/sdl_host.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace mhf::host {

struct SdlHost::Impl {
    SDL_Window *window{};
    SDL_Renderer *renderer{};
    SDL_Texture *texture{};
    SDL_AudioStream *audio{};
    std::vector<SDL_Gamepad *> gamepads;
    HostOptions options;
    int audio_rate{44100};
    std::vector<KeyEvent> keys;
    bool text_input{};
    bool initialized{};
};

SdlHost::SdlHost() : impl_(std::make_unique<Impl>()) {}

SdlHost::~SdlHost() {
    for (auto *g : impl_->gamepads) SDL_CloseGamepad(g);
    if (impl_->audio) SDL_DestroyAudioStream(impl_->audio);
    if (impl_->texture) SDL_DestroyTexture(impl_->texture);
    if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window) SDL_DestroyWindow(impl_->window);
    if (impl_->initialized) SDL_Quit();
}

bool SdlHost::create_renderer(unsigned long long flags) {
    const auto &options = impl_->options;
    if (impl_->window == nullptr) {
        if (!SDL_CreateWindowAndRenderer(options.title, options.width * options.scale, options.height * options.scale,
                                         static_cast<SDL_WindowFlags>(flags), &impl_->window, &impl_->renderer)) {
            std::fprintf(stderr, "[host] cannot create the window: %s\n", SDL_GetError());
            return false;
        }
    } else if (impl_->renderer == nullptr) {
        impl_->renderer = SDL_CreateRenderer(impl_->window, nullptr);
        if (impl_->renderer == nullptr) {
            std::fprintf(stderr, "[host] cannot create a renderer: %s\n", SDL_GetError());
            return false;
        }
    }
    SDL_SetRenderLogicalPresentation(impl_->renderer, options.width, options.height,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(impl_->renderer, 0); // pacing comes from the emulated vblank
    impl_->texture = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                       options.width, options.height);
    SDL_SetTextureScaleMode(impl_->texture, SDL_SCALEMODE_NEAREST);
    return true;
}

bool SdlHost::open(const HostOptions &options) {
    impl_->options = options;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    impl_->initialized = true;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (options.hidden ? SDL_WINDOW_HIDDEN : 0);
    if (options.opengl) {
        flags |= SDL_WINDOW_OPENGL;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        impl_->window = SDL_CreateWindow(options.title, options.width * options.scale, options.height * options.scale, flags);
        if (impl_->window == nullptr) {
            std::fprintf(stderr, "[host] cannot create the window: %s\n", SDL_GetError());
            return false;
        }
    } else if (!create_renderer(flags)) {
        return false;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) std::fprintf(stderr, "[host] no audio: %s\n", SDL_GetError());
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, options.audio_rate};
    impl_->audio_rate = options.audio_rate;
    impl_->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (impl_->audio != nullptr) {
        SDL_ResumeAudioStreamDevice(impl_->audio);
    } else {
        std::fprintf(stderr, "[host] no audio output: %s\n", SDL_GetError());
    }
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i])) impl_->gamepads.push_back(g);
        }
        SDL_free(ids);
    }
    return true;
}

bool SdlHost::poll() {
    SDL_Event e;
    bool keep_running = true;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: keep_running = false; break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT) != 0)) {
                toggle_fullscreen();
                break;
            }
            switch (e.key.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: impl_->keys.push_back({KeyEvent::Kind::Enter, {}}); break;
            case SDLK_ESCAPE: impl_->keys.push_back({KeyEvent::Kind::Escape, {}}); break;
            case SDLK_BACKSPACE: impl_->keys.push_back({KeyEvent::Kind::Backspace, {}}); break;
            case SDLK_LEFT: impl_->keys.push_back({KeyEvent::Kind::Left, {}}); break;
            case SDLK_RIGHT: impl_->keys.push_back({KeyEvent::Kind::Right, {}}); break;
            case SDLK_UP: impl_->keys.push_back({KeyEvent::Kind::Up, {}}); break;
            case SDLK_DOWN: impl_->keys.push_back({KeyEvent::Kind::Down, {}}); break;
            default: break;
            }
            if (impl_->keys.size() > 256) impl_->keys.erase(impl_->keys.begin());
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (impl_->text_input && e.text.text != nullptr) impl_->keys.push_back({KeyEvent::Kind::Text, e.text.text});
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which)) impl_->gamepads.push_back(g);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (auto it = impl_->gamepads.begin(); it != impl_->gamepads.end(); ++it) {
                if (SDL_GetGamepadID(*it) == e.gdevice.which) {
                    SDL_CloseGamepad(*it);
                    impl_->gamepads.erase(it);
                    break;
                }
            }
            break;
        default: break;
        }
    }
    return keep_running;
}

void SdlHost::present(const std::uint32_t *pixels, int width, int height, int stride) {
    if (impl_->renderer == nullptr && (impl_->window == nullptr || !create_renderer(0))) return;
    SDL_Rect area{0, 0, std::min(width, impl_->options.width), std::min(height, impl_->options.height)};
    SDL_UpdateTexture(impl_->texture, &area, pixels, stride * 4);
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    SDL_RenderClear(impl_->renderer);
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr);
    SDL_RenderPresent(impl_->renderer);
}

std::vector<KeyEvent> SdlHost::take_key_events() {
    std::vector<KeyEvent> out;
    out.swap(impl_->keys);
    return out;
}

void SdlHost::set_text_input(bool on) {
    if (impl_->window == nullptr || on == impl_->text_input) return;
    impl_->text_input = on;
    if (on) SDL_StartTextInput(impl_->window);
    else SDL_StopTextInput(impl_->window);
}

void *SdlHost::window_handle() const { return impl_->window; }

void SdlHost::toggle_fullscreen() {
    if (impl_->window == nullptr) return;
    const bool full = (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(impl_->window, !full);
}

GamepadState SdlHost::input() const {
    GamepadState s;
    static const bool kNoKeys[SDL_SCANCODE_COUNT] = {};
    // While text is being typed, letters are text, not buttons.
    const bool *keys = impl_->text_input ? kNoKeys : SDL_GetKeyboardState(nullptr);
    // Keyboard defaults (remappable later, docs/07-launcher-ux.md).
    s.south = keys[SDL_SCANCODE_K];
    s.east = keys[SDL_SCANCODE_L];
    s.west = keys[SDL_SCANCODE_J];
    s.north = keys[SDL_SCANCODE_I];
    s.left_shoulder = keys[SDL_SCANCODE_Q];
    s.right_shoulder = keys[SDL_SCANCODE_E];
    s.start = keys[SDL_SCANCODE_RETURN];
    s.select = keys[SDL_SCANCODE_BACKSPACE];
    s.up = keys[SDL_SCANCODE_UP];
    s.down = keys[SDL_SCANCODE_DOWN];
    s.left = keys[SDL_SCANCODE_LEFT];
    s.right = keys[SDL_SCANCODE_RIGHT];
    s.left_x = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f);
    s.left_y = (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f);
    for (auto *g : impl_->gamepads) {
        s.south |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH);
        s.east |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST);
        s.west |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST);
        s.north |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_NORTH);
        s.left_shoulder |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
                           SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
        s.right_shoulder |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ||
                            SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
        s.start |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START);
        s.select |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK);
        s.guide |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE);
        s.up |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP);
        s.down |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        s.left |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        s.right |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        auto axis = [g](SDL_GamepadAxis a) { return static_cast<float>(SDL_GetGamepadAxis(g, a)) / 32767.0f; };
        const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
        if (std::fabs(lx) > std::fabs(s.left_x)) s.left_x = lx;
        if (std::fabs(ly) > std::fabs(s.left_y)) s.left_y = ly;
        const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
        if (std::fabs(rx) > std::fabs(s.right_x)) s.right_x = rx;
        if (std::fabs(ry) > std::fabs(s.right_y)) s.right_y = ry;
    }
    return s;
}

void SdlHost::queue_audio(const std::int16_t *stereo, std::size_t frames) {
    if (impl_->audio == nullptr) return;
    // Keep at most ~200 ms queued; beyond that the emulation is ahead of the device and old audio would only add lag.
    if (queued_audio_frames() > static_cast<std::size_t>(impl_->audio_rate / 5)) return;
    SDL_PutAudioStreamData(impl_->audio, stereo, static_cast<int>(frames * 4));
}

std::size_t SdlHost::queued_audio_frames() const {
    if (impl_->audio == nullptr) return 0;
    const int bytes = SDL_GetAudioStreamQueued(impl_->audio);
    return bytes > 0 ? static_cast<std::size_t>(bytes) / 4u : 0u;
}

} // namespace mhf::host
