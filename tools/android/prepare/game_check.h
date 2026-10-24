// scripts/game_check.py: the game files bbport runs (Bloodborne with the 1.09 update merged in).
#pragma once
#include <optional>
#include <string>

struct Problem {
    std::string kind, title, version;
};
// None for the supported game (or with BB_SKIP_GAME_CHECK=1); image_hash is the SHA-256 of eboot.bin's
// loaded image.
std::optional<Problem> game_problem(const std::string& game, const std::string& image_hash);
std::string game_explain(const Problem& p);
bool game_check_skipped();
// True for the TITLE_IDs of the Bloodborne releases bbport supports (any region; the PKG step of bb-recomp checks them before unpacking).
bool is_supported_title(const std::string& title_id);
