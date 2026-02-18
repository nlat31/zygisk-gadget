#include <jni.h>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <regex>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <cstdint>

#include "zygisk.hpp"
#include "log.h"
#include "xdl.h"
#include "nlohmann/json.hpp"

#define BUFFER_SIZE (64 * 1024)

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

using json = nlohmann::json;

static bool write_full(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = TEMP_FAILURE_RETRY(write(fd, p, len));
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool read_full(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = TEMP_FAILURE_RETRY(read(fd, p, len));
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static void writeString(int fd, const std::string& str) {
    // Use fixed-width length for stable IPC, and cap to avoid abuse/corruption.
    // Include the null terminator for legacy behavior.
    const uint32_t length = static_cast<uint32_t>(str.size() + 1);
    (void)write_full(fd, &length, sizeof(length));
    (void)write_full(fd, str.c_str(), length);
}

static std::string readString(int fd) {
    uint32_t length = 0;
    if (!read_full(fd, &length, sizeof(length))) return "";
    // sanity cap: paths / package names should be small
    if (length == 0 || length > 16 * 1024) {
        LOGE("readString: invalid length=%u", length);
        return "";
    }
    std::vector<char> buffer(length);
    if (!read_full(fd, buffer.data(), length)) return "";
    // Ensure null-terminated even if sender is buggy
    buffer.back() = '\0';
    return {buffer.data()};
}

std::string getPathFromFd(int fd) {
    char buf[PATH_MAX];
    std::string fdPath = "/proc/self/fd/" + std::to_string(fd);
    ssize_t len = readlink(fdPath.c_str(), buf, sizeof(buf) - 1);
    close(fd);
    if (len != -1) {
        buf[len] = '\0';
        return {buf};
    } else {
        // Handle error
        return "";
    }
}

namespace fs = std::filesystem;
std::string find_matching_file(const fs::path& directory, const std::regex& pattern) {
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto& path = entry.path();
        const auto& filename = path.filename().string();

        if (std::regex_search(filename, pattern)) {
            return filename;
        }
    }
    return ""; // Return an empty string if no match is found
}

static std::string normalize_dir(std::string p) {
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

void injection_thread(const char* app_data_dir, const char* frida_gadget_name, uint time_to_sleep) {
    LOGD("Frida-gadget injection thread start, app_data_dir: %s, gadget name: %s, usleep: %d",
         app_data_dir, frida_gadget_name, time_to_sleep);
    usleep(time_to_sleep);

    std::string app_dir = normalize_dir(app_data_dir ? std::string(app_data_dir) : std::string());
    if (app_dir.empty()) {
        LOGE("app_data_dir is empty, skip injection");
        return;
    }
    std::string gadget_path = app_dir + "/" + std::string(frida_gadget_name);

    std::ifstream file(gadget_path);
    if (file) {
        LOGD("Gadget is ready to load from %s", gadget_path.c_str());
    } else {
        LOGD("Cannot find gadget in %s", gadget_path.c_str());
        return;
    }

    // Prefer dlopen() here. xDL's xdl_open() can return NULL even if the library is
    // actually loaded (pathname mismatch like /data/user/0 vs /data/data symlink).
    dlerror();  // clear
    void* handle = dlopen(gadget_path.c_str(), RTLD_NOW);
    if (handle) {
        LOGD("Frida-gadget loaded (dlopen)");
    } else {
        const char* err = dlerror();
        LOGE("dlopen failed: %s", err ? err : "(null)");
        // Fallback: try xDL force load for edge cases.
        void* xh = xdl_open(gadget_path.c_str(), XDL_TRY_FORCE_LOAD);
        if (xh) {
            LOGD("Frida-gadget loaded (xdl_open)");
            handle = xh;
        } else {
            LOGE("Frida-gadget failed to load (xdl_open returned NULL)");
        }
    }

    // Only cleanup files when gadget is successfully loaded.
    // If load fails, keep the file so users can inspect permissions/ownership.
    if (handle) {
        unlink(gadget_path.c_str());
        // If there's a frida-gadget config file, remove it too.
        std::regex pattern(".*-gadget.*\\.config\\.so$");
        std::string frida_config_name = find_matching_file(app_dir, pattern);
        if (!frida_config_name.empty()) {
            std::string frida_config_path = app_dir + "/" + frida_config_name;
            unlink(frida_config_path.c_str());
        }
    }
}

class MyModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->_api = api;
        _env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) {
            LOGE("Skip unknown process");
            return;
        }

        auto package_name = _env->GetStringUTFChars(args->nice_name, nullptr);

        std::string module_dir = getPathFromFd(_api->getModuleDir());
        int fd = _api->connectCompanion();

        std::string config_file_path = module_dir + "/config";
        writeString(fd, config_file_path);

        std::string target_package_name = readString(fd);

        if (strcmp(package_name, target_package_name.c_str()) == 0) {
            LOGD("Enable gadget injection %s", package_name);
            _enable_gadget_injection = true;
            write(fd, &_enable_gadget_injection, sizeof(_enable_gadget_injection));

            _target_package_name = strdup(target_package_name.c_str());

            // Use the system provided app_data_dir to support multi-user (/data/user/<id>/...)
            // and avoid hardcoding /data/data.
            if (args->app_data_dir) {
                auto app_dir = _env->GetStringUTFChars(args->app_data_dir, nullptr);
                if (app_dir) {
                    writeString(fd, app_dir);
                    _app_data_dir = strdup(app_dir);
                    _env->ReleaseStringUTFChars(args->app_data_dir, app_dir);
                } else {
                    writeString(fd, "");
                }
            } else {
                writeString(fd, "");
            }

            uint delay;
            read(fd, &delay, sizeof(delay));
            _delay = delay;

            std::string frida_gadget_name = readString(fd);
            if (frida_gadget_name.empty()) {
                LOGE("Companion did not provide gadget name, skip injection");
                _enable_gadget_injection = false;
                close(fd);
                _env->ReleaseStringUTFChars(args->nice_name, package_name);
                return;
            }
            _frida_gadget_name = strdup(frida_gadget_name.c_str());

            close(fd);
        } else {
            _api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            close(fd);
        }
        _env->ReleaseStringUTFChars(args->nice_name, package_name);
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (_enable_gadget_injection) {
            std::thread t(injection_thread, _app_data_dir, _frida_gadget_name, _delay);
            t.detach();
        }
    }

private:
    Api* _api{};
    JNIEnv* _env{};
    bool _enable_gadget_injection = false;
    char* _target_package_name{};
    char* _app_data_dir{};
    uint _delay{};
    char* _frida_gadget_name{};

};

json get_json(const std::string& path) {
    std::ifstream file(path);
    if (file.is_open()) {
        json j;
        file >> j;
        file.close();
        return j;
    } else {
        LOGD("Failed to open %s", path.c_str());
        return nullptr;
    }
}

static bool copy_file(const char *source_path, const char *dest_path) {
    FILE *source_file, *dest_file;
    char buffer[BUFFER_SIZE];
    size_t bytes_read;

    source_file = fopen(source_path, "rb");
    if (source_file == nullptr) {
        LOGE("Error opening source file %s: %s", source_path, strerror(errno));
        return false;
    }

    dest_file = fopen(dest_path, "wb");
    if (dest_file == nullptr) {
        LOGE("Error opening destination file %s: %s", dest_path, strerror(errno));
        fclose(source_file);
        return false;
    }

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, source_file)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dest_file) != bytes_read) {
            LOGE("Error writing to destination file %s: %s", dest_path, strerror(errno));
            fclose(source_file);
            fclose(dest_file);
            return false;
        }
    }

    if (ferror(source_file)) {
        LOGE("Error reading from source file %s: %s", source_path, strerror(errno));
    }

    fclose(source_file);
    fclose(dest_file);
    return true;
}

static void chown_like_dir(const char* file_path, const char* dir_path) {
    struct stat st{};
    if (stat(dir_path, &st) != 0) {
        LOGW("stat(%s) failed: %s", dir_path, strerror(errno));
        return;
    }
    if (chown(file_path, st.st_uid, st.st_gid) != 0) {
        LOGW("chown(%s, %d, %d) failed: %s",
             file_path, static_cast<int>(st.st_uid), static_cast<int>(st.st_gid), strerror(errno));
    }
}

static void companion_handler(int i) {
    std::string config_file_path = readString(i);

    json j = get_json(config_file_path);
    if (j == nullptr) {
        return;
    }
    std::string target_package_name = j["package"]["name"];
    uint delay = j["package"]["delay"];
    bool frida_config_mode = j["package"]["mode"]["config"];

    writeString(i, target_package_name);

    bool enable_gadget_injection;
    read(i, &enable_gadget_injection, sizeof(enable_gadget_injection));
    if (!enable_gadget_injection) {
        return;
    }

    // Read the actual app data dir from the app process (e.g. /data/user/0/<pkg>).
    std::string app_data_dir = normalize_dir(readString(i));
    if (app_data_dir.empty()) {
        app_data_dir = "/data/data/" + target_package_name;
        LOGW("app_data_dir not provided, fallback to %s", app_data_dir.c_str());
    }

    write(i, &delay, sizeof(delay));

#ifdef __arm__
    std::regex frida_gadget_pattern(".*-gadget.*arm\\.so$");
#elifdef __aarch64__
    std::regex frida_gadget_pattern(".*-gadget.*arm64\\.so$");
#elifdef __i386__
    std::regex frida_gadget_pattern(".*-gadget.*x86\\.so$");
#elifdef __x86_64__
    std::regex frida_gadget_pattern(".*-gadget.*x86_64\\.so$");
#endif
    std::string module_dir = config_file_path.substr(0, config_file_path.rfind('/'));;
    std::string frida_gadget_name = find_matching_file(module_dir, frida_gadget_pattern);
    if (frida_gadget_name.empty()) {
        LOGE("Cannot find gadget in module dir: %s", module_dir.c_str());
        return;
    }
    std::string frida_gadget_path = module_dir + "/" + frida_gadget_name;

    std::string copy_src;
    std::string copy_dst;
    if (frida_config_mode) {
        std::regex frida_config_pattern(".*-gadget\\.config$");
        std::string frida_config_name = find_matching_file(module_dir, frida_config_pattern);
        if (frida_config_name.empty()) {
            LOGW("Config mode enabled but cannot find frida-gadget.config in %s", module_dir.c_str());
        }
        std::string frida_config_path = module_dir + "/" + frida_config_name;

        std::string new_frida_config_name = frida_gadget_name.substr(0, frida_gadget_name.find_last_of('.')) + ".config.so";
        copy_src = frida_config_path;
        copy_dst = app_data_dir + "/" + new_frida_config_name;
        LOGD("Copy config: %s -> %s", copy_src.c_str(), copy_dst.c_str());
        if (copy_file(copy_src.c_str(), copy_dst.c_str())) {
            chown_like_dir(copy_dst.c_str(), app_data_dir.c_str());
        }
    }

    copy_src = frida_gadget_path;
    copy_dst = app_data_dir + "/" + frida_gadget_name;
    LOGD("Copy gadget: %s -> %s", copy_src.c_str(), copy_dst.c_str());
    if (copy_file(copy_src.c_str(), copy_dst.c_str())) {
        chown_like_dir(copy_dst.c_str(), app_data_dir.c_str());
    }

    // IMPORTANT: only send gadget name after copy completes.
    // Otherwise the app process may attempt to dlopen a partially copied ELF and crash.
    writeString(i, frida_gadget_name);
}

REGISTER_ZYGISK_MODULE(MyModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
