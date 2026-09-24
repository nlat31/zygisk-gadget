#include <unistd.h>
#include <getopt.h>
#include <iostream>
#include <fstream>
#include <regex>
#include <climits>
#include <cerrno>
#include <csignal>
#include <iomanip>

#include "logcat.h"
#include "nlohmann/json.hpp"

using namespace std;
using json = nlohmann::json;

const char* short_options = "hcp:d:";
const struct option long_options[] = {
        {"help", no_argument, nullptr, 'h'},
        {"config", no_argument, nullptr, 'c'},
        {"package", required_argument, nullptr, 'p'},
        {"delay", required_argument, nullptr, 'd'},
        {nullptr, 0, nullptr, 0}
};

void show_usage() {
    printf("Usage: ./zygisk-gadget -p <packageName> <option(s)>\n");
    printf(" Options:\n");
    printf("  -d, --delay <microseconds>             Delay in microseconds before loading frida-gadget\n");
    printf("  -c, --config                           Activate config mode (default: false)\n");
    printf("  -h, --help                             Show help\n\n");
}

json get_json(const std::string& path) {
    std::ifstream file(path);
    if (file.is_open()) {
        json j = json::parse(file, nullptr, false);
        file.close();
        if (j.is_discarded()) {
            std::cerr << "Failed to parse JSON file: " << path << std::endl;
            return nullptr;
        }
        return j;
    } else {
        return nullptr;
    }
}

void update_json(json& j, const std::vector<std::string>& key_path, const json& value) {
    json* current = &j;
    // Navigate to the correct position in the JSON object
    for (size_t i = 0; i < key_path.size(); ++i) {
        const std::string& key = key_path[i];

        if (current->contains(key)) {
            if (i == key_path.size() - 1) {
                // Last key in the path, update the value
                (*current)[key] = value;
            } else {
                // Navigate deeper into the JSON object
                current = &((*current)[key]);
            }
        } else {
            std::cerr << "Key path element '" << key << "' not found in JSON." << std::endl;
            exit(-1);
        }
    }
}

void write_json(const json& j, const string& file_path) {
    ofstream file(file_path);
    if (!file.is_open()) {
        cerr << "Unable to write to JSON file: " << file_path << endl;
        exit(-1);
    }
    file << std::setw(4) << j << std::endl; // Pretty print with 4 spaces indentation
}

uint check_delay_optarg(char* option) {
    if (option == nullptr || option[0] == '\0') {
        std::cerr << "Empty delay value" << std::endl;
        return static_cast<uint>(-1);
    }
    if (option[0] == '-') {
        std::cerr << "Negative value is not allowed: " << option << std::endl;
        return static_cast<uint>(-1);
    }
    char* endptr = nullptr;
    errno = 0;
    unsigned long long temp_value = strtoull(option, &endptr, 10);
    if (endptr == option || *endptr != '\0') {
        std::cerr << "Invalid characters found in the input: " << option << std::endl;
        return static_cast<uint>(-1);
    }
    if (errno == ERANGE || temp_value > static_cast<unsigned long long>(UINT_MAX)) {
        std::cerr << "Value out of range for unsigned int: " << option << std::endl;
        return static_cast<uint>(-1);
    }
    return static_cast<uint>(temp_value);
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

// Function to handle signals like Ctrl + C (SIGINT)
void signalHandler(int signal) {
    json j = get_json(config_file_path);
    std::vector<std::string> key_path;
    key_path = {"package", "name"};
    update_json(j, key_path, "com.hackcatml.test");
    write_json(j, config_file_path);

    exit(signal);
}

int main(int argc, char* argv[]) {
    uint uid = getuid();
    if (uid != 0) {
        cout << "Need root to run this program" << endl;
        return -1;
    }

    int option;
    string pkg;
    uint delay = 0;
    bool isValidArg = true, config_mode = false;

    while((option = getopt_long(argc, argv, short_options, long_options, nullptr)) != -1) {
        switch (option) {
            case 'p':
                pkg = optarg;
                break;
            case 'd': {
                delay = check_delay_optarg(optarg);
                if (delay == -1)
                    return -1;
                break;
            }
            case 'c':
            {
                std::regex pattern(".*-gadget\\.config$");
                std::string module_dir = config_file_path.substr(0, config_file_path.rfind('/'));
                std::string frida_config_name = find_matching_file(module_dir, pattern);
                if (!frida_config_name.empty()) {
                    cout << "[*] Found " << frida_config_name << endl;
                    std::string frida_config_path = module_dir.append("/" + frida_config_name);
                    json j = get_json(frida_config_path);
                    cout << j.dump(4) << endl;
                    config_mode = true;
                } else {
                    cout << R"([!] Cannot find "frida-gadget.config". Create or push "frida-gadget.config" file in )" << "\"" + module_dir + "/\"" << endl;
                    return -1;
                }
                break;
            }
            case 'h':
                show_usage();
                return -1;
            default:
                isValidArg = false;
                break;
        }
    }

    if (!isValidArg || pkg.empty()) {
        printf("Wrong Arguments, Please Check!!\n");
        show_usage();
        return -1;
    }

    json j = get_json(config_file_path);
    std::vector<std::string> key_path;
    key_path = {"package", "name"};
    update_json(j, key_path, pkg);
    key_path = {"package" , "delay"};
    update_json(j, key_path, delay);
    key_path = {"package", "mode", "config"};
    update_json(j, key_path, config_mode);

    write_json(j, config_file_path);

    // Register signal handler for SIGINT (Ctrl + C)
    std::signal(SIGINT, signalHandler);

    logcat();

    return 0;
}
