// Vita3K emulator project
// Copyright (C) 2024 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <app/functions.h>

#include <audio/state.h>
#include <config/functions.h>
#include <config/state.h>
#include <config/version.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <gui/functions.h>
#include <gui/imgui_impl_sdl.h>
#include <io/functions.h>
#include <kernel/state.h>
#include <motion/state.h>
#include <ngs/state.h>
#include <renderer/state.h>

#include <renderer/functions.h>
#include <rtc/rtc.h>
#include <util/fs.h>
#include <util/lock_and_find.h>
#include <util/log.h>
#include <util/string_utils.h>

#if USE_DISCORD
#include <app/discord.h>
#endif

#include <gdbstub/functions.h>

#include <renderer/vulkan/functions.h>

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_vulkan.h>

#ifdef ANDROID
#include <SDL.h>
#include <adrenotools/driver.h>
#include <boost/range/iterator_range.hpp>
#include <jni.h>

static bool load_custom_driver(const std::string &driver_name) {
    fs::path driver_path = fs::path(SDL_AndroidGetInternalStoragePath()) / "driver" / driver_name / "/";

    if (!fs::exists(driver_path)) {
        LOG_ERROR("Could not find driver {}", driver_name);
        return false;
    }

    std::string main_so_name;
    {
        fs::path driver_name_file = driver_path / "driver_name.txt";
        if (!fs::exists(driver_name_file)) {
            LOG_ERROR("Could not find driver driver_name.txt");
            return false;
        }

        fs::ifstream name_file(driver_name_file, std::ios_base::in);
        name_file >> main_so_name;
        name_file.close();
    }

    const char *temp_dir = nullptr;
    fs::path temp_dir_path;
    if (SDL_GetAndroidSDKVersion() < 29) {
        temp_dir_path = driver_path / "tmp/";
        fs::create_directory(temp_dir_path);
        temp_dir = temp_dir_path.c_str();
    }

    fs::path lib_dir;
    // retrieve the app lib dir using jni
    {
        // retrieve the JNI environment.
        JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
        env->PushLocalFrame(10);
        // retrieve the Java instance of the SDLActivity
        jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
        // the following calls activity.getApplicationInfo().nativeLibraryDir
        jclass actibity_class = env->GetObjectClass(activity);
        jmethodID getApplicationInfo_method = env->GetMethodID(actibity_class, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
        jobject app_info = env->CallObjectMethod(activity, getApplicationInfo_method);
        jclass app_info_class = env->GetObjectClass(app_info);
        jfieldID app_info_field = env->GetFieldID(app_info_class, "nativeLibraryDir", "Ljava/lang/String;");
        jstring lib_dir_java = reinterpret_cast<jstring>(env->GetObjectField(app_info, app_info_field));
        const char *lib_dir_ptr = env->GetStringUTFChars(lib_dir_java, nullptr);

        // copy the dir path in our local object
        lib_dir = fs::path(lib_dir_ptr) / "/";

        env->ReleaseStringUTFChars(lib_dir_java, lib_dir_ptr);
        // remove all local references
        env->PopLocalFrame(nullptr);
    }

    fs::create_directory(driver_path / "file_redirect");

    void *vulkan_handle = adrenotools_open_libvulkan(
        RTLD_NOW,
        ADRENOTOOLS_DRIVER_FILE_REDIRECT | ADRENOTOOLS_DRIVER_CUSTOM,
        temp_dir,
        lib_dir.c_str(),
        driver_path.c_str(),
        main_so_name.c_str(),
        (driver_path / "file_redirect/").c_str(),
        nullptr);

    if (!vulkan_handle) {
        LOG_ERROR("Could not open handle for custom driver {}", driver_name);
        return false;
    }

    // we use a custom sdl build, if the path starts with this magic number, it uses the following handle instead
    struct {
        uint64_t magic;
        void *handle;
    } load_library_parameter;
    load_library_parameter.magic = 0xFEEDC0DE;
    load_library_parameter.handle = vulkan_handle;

    if (SDL_Vulkan_LoadLibrary(reinterpret_cast<const char *>(&load_library_parameter)) < 0) {
        LOG_ERROR("Could not load custom diver, error {}", SDL_GetError());
        return false;
    }

    return true;
}
#endif

namespace app {
void update_viewport(EmuEnvState &state) {
    int w = 0;
    int h = 0;

    switch (state.renderer->current_backend) {
    case renderer::Backend::OpenGL:
        SDL_GL_GetDrawableSize(state.window.get(), &w, &h);
        break;

    case renderer::Backend::Vulkan:
        SDL_Vulkan_GetDrawableSize(state.window.get(), &w, &h);
        break;

    default:
        LOG_ERROR("Unimplemented backend renderer: {}.", static_cast<int>(state.renderer->current_backend));
        break;
    }

    state.drawable_size.x = w;
    state.drawable_size.y = h;

    if (h > 0) {
        const float window_aspect = static_cast<float>(w) / h;
        const float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
        if (state.cfg.stretch_the_display_area) {
            // Match the aspect ratio to the screen size.
            state.viewport_size.x = static_cast<SceFloat>(w);
            state.viewport_size.y = static_cast<SceFloat>(h);
            state.viewport_pos.x = 0;
            state.viewport_pos.y = 0;
        } else if (window_aspect > vita_aspect) {
            // Window is wide. Pin top and bottom.
            state.viewport_size.x = h * vita_aspect;
            state.viewport_size.y = static_cast<SceFloat>(h);
            state.viewport_pos.x = (w - state.viewport_size.x) / 2;
            state.viewport_pos.y = 0;
        } else {
            // Window is tall. Pin left and right.
            state.viewport_size.x = static_cast<SceFloat>(w);
            state.viewport_size.y = w / vita_aspect;
            state.viewport_pos.x = 0;
            state.viewport_pos.y = (h - state.viewport_size.y) / 2;
        }
    } else {
        state.viewport_pos.x = 0;
        state.viewport_pos.y = 0;
        state.viewport_size.x = 0;
        state.viewport_size.y = 0;
    }
}

void init_paths(Root &root_paths) {
#ifdef ANDROID
    fs::path storage_path = fs::path(SDL_AndroidGetExternalStoragePath()) / "";
    fs::path vita_storage_path = storage_path / "vita/";

    root_paths.set_base_path(storage_path);
    // note: this one is not actually used, we must use custom functions to retrieve static assets
    root_paths.set_static_assets_path(storage_path);

    root_paths.set_pref_path(vita_storage_path);
    root_paths.set_log_path(storage_path);
    root_paths.set_config_path(storage_path);
    root_paths.set_shared_path(storage_path);
    root_paths.set_cache_path(storage_path / "cache" / "");
#else
    auto sdl_base_path = SDL_GetBasePath();
    auto base_path = fs_utils::utf8_to_path(sdl_base_path);
    SDL_free(sdl_base_path);

    root_paths.set_base_path(base_path);
    root_paths.set_static_assets_path(base_path);

#if defined(__APPLE__)
    // On Apple platforms, base_path is "Contents/Resources/" inside the app bundle.
    // An extra parent_path is apparently needed because of the trailing slash.
    auto portable_path = base_path.parent_path().parent_path().parent_path().parent_path() / "portable" / "";
#else
    auto portable_path = base_path / "portable" / "";
#endif

    if (fs::is_directory(portable_path)) {
        // If a portable directory exists, use it for everything else.
        // Note that pref_path should not be the same as the other paths.
        root_paths.set_pref_path(portable_path / "fs" / "");
        root_paths.set_log_path(portable_path);
        root_paths.set_config_path(portable_path);
        root_paths.set_shared_path(portable_path);
        root_paths.set_cache_path(portable_path / "cache" / "");
    } else {
        // SDL_GetPrefPath is deferred as it creates the directory.
        // When using a portable directory, it is not needed.
        auto sdl_pref_path = SDL_GetPrefPath(org_name, app_name);
        auto pref_path = fs_utils::utf8_to_path(sdl_pref_path);
        SDL_free(sdl_pref_path);

#if defined(__APPLE__)
        // Store other data in the user-wide path. Otherwise we may end up dumping
        // files into the "/Applications/" install directory or the app bundle.
        // This will typically be "~/Library/Application Support/Vita3K/Vita3K/".
        // Check for config.yml first, though, to maintain backwards compatibility,
        // even though storing user data inside the app bundle is not a good idea.
        auto existing_config = base_path / "config.yml";
        if (!fs::exists(existing_config)) {
            base_path = pref_path;
        }

        // pref_path should not be the same as the other paths.
        // For backwards compatibility, though, check if ux0 exists first.
        auto existing_ux0 = pref_path / "ux0";
        if (!fs::is_directory(existing_ux0)) {
            pref_path = pref_path / "fs" / "";
        }
#endif

        root_paths.set_pref_path(pref_path);
        root_paths.set_log_path(base_path);
        root_paths.set_config_path(base_path);
        root_paths.set_shared_path(base_path);
        root_paths.set_cache_path(base_path / "cache" / "");

#if defined(__linux__) && !defined(__APPLE__)
        // XDG Data Dirs.
        auto env_home = getenv("HOME");
        auto XDG_DATA_DIRS = getenv("XDG_DATA_DIRS");
        auto XDG_DATA_HOME = getenv("XDG_DATA_HOME");
        auto XDG_CACHE_HOME = getenv("XDG_CACHE_HOME");
        auto XDG_CONFIG_HOME = getenv("XDG_CONFIG_HOME");
        auto APPDIR = getenv("APPDIR"); // Used in AppImage

        if (XDG_DATA_HOME != NULL)
            root_paths.set_pref_path(fs::path(XDG_DATA_HOME) / app_name / app_name / "");

        if (XDG_CONFIG_HOME != NULL)
            root_paths.set_config_path(fs::path(XDG_CONFIG_HOME) / app_name / "");
        else if (env_home != NULL)
            root_paths.set_config_path(fs::path(env_home) / ".config" / app_name / "");

        if (XDG_CACHE_HOME != NULL) {
            root_paths.set_cache_path(fs::path(XDG_CACHE_HOME) / app_name / "");
            root_paths.set_log_path(fs::path(XDG_CACHE_HOME) / app_name / "");
        } else if (env_home != NULL) {
            root_paths.set_cache_path(fs::path(env_home) / ".cache" / app_name / "");
            root_paths.set_log_path(fs::path(env_home) / ".cache" / app_name / "");
        }

        // Don't assume that base_path is portable.
        if (fs::exists(root_paths.get_base_path() / "data") && fs::exists(root_paths.get_base_path() / "lang") && fs::exists(root_paths.get_base_path() / "shaders-builtin"))
            root_paths.set_static_assets_path(root_paths.get_base_path());
        else if (env_home != NULL)
            root_paths.set_static_assets_path(fs::path(env_home) / ".local/share" / app_name / "");

        if (XDG_DATA_DIRS != NULL) {
            auto env_paths = string_utils::split_string(XDG_DATA_DIRS, ':');
            for (auto &i : env_paths) {
                if (fs::exists(fs::path(i) / app_name)) {
                    root_paths.set_static_assets_path(fs::path(i) / app_name / "");
                    break;
                }
            }
        } else if (XDG_DATA_HOME != NULL) {
            if (fs::exists(fs::path(XDG_DATA_HOME) / app_name / "data") && fs::exists(fs::path(XDG_DATA_HOME) / app_name / "lang") && fs::exists(fs::path(XDG_DATA_HOME) / app_name / "shaders-builtin"))
                root_paths.set_static_assets_path(fs::path(XDG_DATA_HOME) / app_name / "");
        }

        if (APPDIR != NULL && fs::exists(fs::path(APPDIR) / "usr/share/Vita3K")) {
            root_paths.set_static_assets_path(fs::path(APPDIR) / "usr/share/Vita3K");
        }

        // shared path
        if (env_home != NULL)
            root_paths.set_shared_path(fs::path(env_home) / ".local/share" / app_name / "");

        if (XDG_DATA_DIRS != NULL) {
            auto env_paths = string_utils::split_string(XDG_DATA_DIRS, ':');
            for (auto &i : env_paths) {
                if (fs::exists(fs::path(i) / app_name)) {
                    root_paths.set_shared_path(fs::path(i) / app_name / "");
                    break;
                }
            }
        } else if (XDG_DATA_HOME != NULL) {
            root_paths.set_shared_path(fs::path(XDG_DATA_HOME) / app_name / "");
        }
#endif
    }
#endif

    // Create default preference and cache path for safety
    fs::create_directories(root_paths.get_config_path());
    fs::create_directories(root_paths.get_cache_path());
    fs::create_directories(root_paths.get_log_path() / "shaderlog");
    fs::create_directories(root_paths.get_log_path() / "texturelog");
}

bool init(EmuEnvState &state, const Root &root_paths) {
    state.base_path = root_paths.get_base_path();
    state.default_path = root_paths.get_pref_path();
    state.log_path = root_paths.get_log_path();
    state.config_path = root_paths.get_config_path();
    state.cache_path = root_paths.get_cache_path();
    state.shared_path = root_paths.get_shared_path();
    state.static_assets_path = root_paths.get_static_assets_path();

    // If configuration does not provide a preference path, use SDL's default
    if (state.cfg.pref_path == root_paths.get_pref_path() || state.cfg.pref_path.empty())
        state.pref_path = root_paths.get_pref_path();
    else {
        auto last_char = state.cfg.pref_path.back();
        if (last_char != fs::path::preferred_separator && last_char != '/')
            state.cfg.pref_path += fs::path::preferred_separator;
        state.pref_path = state.cfg.get_pref_path();
    }

    LOG_INFO("Base path: {}", state.base_path);
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__APPLE__)
    LOG_INFO("Static assets path: {}", state.static_assets_path);
    LOG_INFO("Shared path: {}", state.shared_path);
    LOG_INFO("Log path: {}", state.log_path);
    LOG_INFO("User config path: {}", state.config_path);
    LOG_INFO("User cache path: {}", state.cache_path);
#endif
    LOG_INFO("User pref path: {}", state.pref_path);

    if (ImGui::GetCurrentContext() == NULL) {
        ImGui::CreateContext();
    }
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;

    state.backend_renderer = renderer::Backend::Vulkan;

    if (string_utils::toupper(state.cfg.current_config.backend_renderer) == "OPENGL") {
#if defined(__APPLE__)
        state.cfg.backend_renderer = "Vulkan";
        config::serialize_config(state.cfg, state.cfg.config_path);
#else
        state.backend_renderer = renderer::Backend::OpenGL;
#endif
    }

    int window_type = 0;
    switch (state.backend_renderer) {
    case renderer::Backend::OpenGL:
        window_type = SDL_WINDOW_OPENGL;
        break;

    case renderer::Backend::Vulkan:
        window_type = SDL_WINDOW_VULKAN;
        break;

    default:
        LOG_ERROR("Unimplemented backend renderer: {}.", state.cfg.backend_renderer);
        break;
    }

#ifdef ANDROID
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    state.display.fullscreen = true;
    window_type |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
    if (state.cfg.fullscreen) {
        state.display.fullscreen = true;
        window_type |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
#endif

#if defined(WIN32) || defined(__linux__)
    const auto isSteamDeck = []() {
#if defined(__linux__) && !defined(ANDROID)
        std::ifstream file("/etc/os-release");
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("VARIANT_ID=steamdeck") != std::string::npos)
                    return true;
            }
        }
#endif
        return false;
    };

#endif
    state.res_width_dpi_scale = static_cast<uint32_t>(DEFAULT_RES_WIDTH * state.dpi_scale);
    state.res_height_dpi_scale = static_cast<uint32_t>(DEFAULT_RES_HEIGHT * state.dpi_scale);

#ifdef ANDROID
    if (!state.cfg.current_config.custom_driver_name.empty()) {
        // load custom driver using libadrenotools
        if (!load_custom_driver(state.cfg.current_config.custom_driver_name))
            return false;
    }
#endif

    state.window = WindowPtr(SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, state.res_width_dpi_scale, state.res_height_dpi_scale, window_type | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);

    if (!state.window) {
        LOG_ERROR("SDL failed to create window!");
        return false;
    }

    // initialize the renderer first because we need to know if we need a page table
    if (!state.cfg.console) {
        if (renderer::init(state.window.get(), state.renderer, state.backend_renderer, state.cfg, root_paths)) {
            update_viewport(state);
        } else {
            switch (state.backend_renderer) {
            case renderer::Backend::OpenGL:
#ifdef ANDROID
                error_dialog("Could not create OpenGL ES context!\nDoes your GPU support OpenGL ES 3.2?", nullptr);
#else
                error_dialog("Could not create OpenGL context!\nDoes your GPU at least support OpenGL 4.4?", nullptr);
#endif
                break;

            case renderer::Backend::Vulkan:
                error_dialog("Could not create Vulkan context!\nDoes your device support Vulkan?");
                break;

            default:
                error_dialog(fmt::format("Unknown backend renderer: {}.", state.cfg.backend_renderer));
                break;
            }
            return false;
        }
    }

#ifdef ANDROID
    state.renderer->current_custom_driver = state.cfg.current_config.custom_driver_name;
#endif

    if (!init(state.io, state.cache_path, state.log_path, state.pref_path, state.cfg.console)) {
        LOG_ERROR("Failed to initialize file system for the emulator!");
        return false;
    }

    state.motion.init();

#if USE_DISCORD
    if (discordrpc::init() && state.cfg.discord_rich_presence) {
        discordrpc::update_presence();
    }
#endif

    return true;
}

bool late_init(EmuEnvState &state) {
    // note: mem is not initialized yet but that's not an issue
    // the renderer is not using it yet, just storing it for later uses
    state.renderer->late_init(state.cfg, state.app_path, state.mem);

    const bool need_page_table = state.renderer->mapping_method == MappingMethod::PageTable || state.renderer->mapping_method == MappingMethod::NativeBuffer;
    if (!init(state.mem, need_page_table)) {
        LOG_ERROR("Failed to initialize memory for emulator state!");
        return false;
    }

    if (state.mem.use_page_table && state.kernel.cpu_backend == CPUBackend::Unicorn)
        LOG_CRITICAL("Unicorn backend is not supported with a page table");

    const ResumeAudioThread resume_thread = [&state](SceUID thread_id) {
        const auto thread = lock_and_find(thread_id, state.kernel.threads, state.kernel.mutex);
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->status == ThreadStatus::wait) {
            thread->update_status(ThreadStatus::run);
        }
    };
    if (!state.audio.init(resume_thread, state.cfg.audio_backend)) {
        LOG_WARN("Failed to initialize audio! Audio will not work.");
    }

    if (!ngs::init(state.ngs, state.mem)) {
        LOG_ERROR("Failed to initialize ngs.");
        return false;
    }

    return true;
}

void destroy(EmuEnvState &emuenv, ImGui_State *imgui) {
    ImGui_ImplSdl_Shutdown(imgui);

#ifdef USE_DISCORD
    discordrpc::shutdown();
#endif

    if (emuenv.cfg.gdbstub)
        server_close(emuenv);

    // There may be changes that made in the GUI, so we should save, again
    if (emuenv.cfg.overwrite_config)
        config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

void switch_state(EmuEnvState &emuenv, const bool pause) {
    if (pause) {
#ifdef ANDROID
        emuenv.display.imgui_render = true;
        gui::set_controller_overlay_state(0);
#endif

        emuenv.kernel.pause_threads();
    }
    else {
#ifdef ANDROID
        emuenv.display.imgui_render = false;
        if (emuenv.cfg.enable_gamepad_overlay)
            gui::set_controller_overlay_state(gui::get_overlay_display_mask(emuenv.cfg));
#endif

        emuenv.kernel.resume_threads();
    }

    emuenv.audio.switch_state(pause);
}

} // namespace app
