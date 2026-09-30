#include "util/tz.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace tz {

bool is_valid_name(const std::string& name, const std::string& zoneinfo_dir) {
    if (name.empty() || name.size() > 64 || name[0] == '/' || name.find("..") != std::string::npos) return false;
    for (unsigned char c : name) {
        if (std::isalnum(c) == 0 && c != '_' && c != '+' && c != '-' && c != '/') return false;
    }
    std::ifstream f(zoneinfo_dir + "/" + name, std::ios::binary);
    char magic[4] = {};
    // Файл пояса начинается с "TZif" — отсекает каталоги ("Europe") и
    // посторонние файлы вроде zone.tab.
    return f.read(magic, 4) && magic[0] == 'T' && magic[1] == 'Z' && magic[2] == 'i' && magic[3] == 'f';
}

namespace {

// localtime_r в поясе tz с восстановлением прежнего TZ процесса.
std::tm local_tm(const std::string& tz, std::time_t t) {
    const char* prev = std::getenv("TZ");
    std::string saved = prev != nullptr ? prev : "";
    bool had_prev = prev != nullptr;

    setenv("TZ", tz.c_str(), 1);
    tzset();
    std::tm local{};
    localtime_r(&t, &local);

    if (had_prev) setenv("TZ", saved.c_str(), 1);
    else unsetenv("TZ");
    tzset();
    return local;
}

} // namespace

std::string local_time(const std::string& tz, std::time_t now) {
    std::tm local = local_tm(tz, now);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

std::string local_time_seconds(const std::string& tz, std::time_t now) {
    std::tm local = local_tm(tz, now);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

std::string local_date(const std::string& tz, std::time_t now, int days_offset) {
    std::tm local = local_tm(tz, now + static_cast<std::time_t>(days_offset) * 86400);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buf;
}

} // namespace tz
