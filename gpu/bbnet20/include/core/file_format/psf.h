// bbport online module: shadPS4's param.sfo reader as BBSC uses it (GetString("APP_VER")),
// answered from what the runtime passed to bbnet_init.
#pragma once
#include <optional>
#include <string>
#include <string_view>

class PSF {
public:
    std::optional<std::string_view> GetString(std::string_view key) const {
        if (key == "APP_VER" && !app_ver.empty()) return std::string_view{app_ver};
        if (key == "TITLE_ID" && !title_id.empty()) return std::string_view{title_id};
        if (key == "TITLE" && !title.empty()) return std::string_view{title};
        return std::nullopt;
    }
    std::string app_ver, title_id, title;
};
