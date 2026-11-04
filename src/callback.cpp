#include "callback.hpp"

#include <cctype>

namespace callback {

namespace {

struct Spec {
    Kind kind;
    const char* code;
    bool has_number;
};
constexpr Spec kSpecs[] = {
    {Kind::PickCategory, "c", true}, {Kind::CategoryPage, "cp", true}, {Kind::PickWallet, "w", true},
    {Kind::EditCategory, "ce", false}, {Kind::EditWallet, "we", false}, {Kind::MakeDefault, "wd", false},
    {Kind::Cancel, "x", false}, {Kind::Save, "ok", false},
};

const Spec& spec_of(Kind kind) {
    for (const auto& s : kSpecs) {
        if (s.kind == kind) return s;
    }
    return kSpecs[0];
}

bool digits(const std::string& s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

} // namespace

std::string make(const std::string& draft_id, Kind kind, int number) {
    const Spec& s = spec_of(kind);
    std::string out = draft_id + ":" + s.code;
    if (s.has_number) out += ":" + std::to_string(number);
    return out;
}

std::optional<Action> parse(const std::string& data) {
    std::size_t first = data.find(':');
    if (first == std::string::npos) return std::nullopt;
    std::string id = data.substr(0, first);
    if (!digits(id, 20)) return std::nullopt;

    std::size_t second = data.find(':', first + 1);
    std::string code = data.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    std::string number = second == std::string::npos ? std::string{} : data.substr(second + 1);

    for (const auto& s : kSpecs) {
        if (code != s.code) continue;
        Action a;
        a.draft_id = id;
        a.kind = s.kind;
        if (s.has_number) {
            if (!digits(number, 3)) return std::nullopt;
            a.number = std::stoi(number);
        } else if (second != std::string::npos) {
            return std::nullopt;
        }
        return a;
    }
    return std::nullopt;
}

const char* kind_name(Kind kind) { return spec_of(kind).code; }

namespace {

constexpr const char* kAiPrefix = "ai:";

bool provider_id(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) {
        if (c < 'a' || c > 'z') return false;
    }
    return true;
}

} // namespace

bool is_ai(const std::string& data) { return data.compare(0, 3, kAiPrefix) == 0; }

const char* ai_kind_name(AiKind kind) {
    switch (kind) {
    case AiKind::Select: return "sel";
    case AiKind::Consent: return "consent";
    case AiKind::Revoke: return "revoke";
    case AiKind::Back: return "back";
    }
    return "back";
}

std::string make_ai(AiKind kind, const std::string& provider) {
    if (kind == AiKind::Back) return "ai:back";
    return std::string(kAiPrefix) + ai_kind_name(kind) + ":" + provider;
}

std::optional<AiAction> parse_ai(const std::string& data) {
    if (!is_ai(data)) return std::nullopt;
    std::string rest = data.substr(3);
    if (rest == "back") return AiAction{AiKind::Back, {}};
    std::size_t colon = rest.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string verb = rest.substr(0, colon);
    std::string provider = rest.substr(colon + 1);
    // Лишние поля («ai:sel:claude:x»), «..», «/», заглавные — не провайдер.
    if (!provider_id(provider)) return std::nullopt;
    for (AiKind k : {AiKind::Select, AiKind::Consent, AiKind::Revoke}) {
        if (verb == ai_kind_name(k)) return AiAction{k, provider};
    }
    return std::nullopt;
}

} // namespace callback
