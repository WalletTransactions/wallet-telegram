#include "card.hpp"

#include <algorithm>

#include "callback.hpp"
#include "expense/text.hpp"

namespace card {

const char* const kConfirm = "Записать?";

namespace {

using drafts::Draft;
using telegram::Button;
using telegram::Keyboard;

// "2026-09-28 12:00:00" → «сегодня», «вчера», «24.09», «15.03.2025».
std::string expense_date(const std::string& datetime, const std::string& today, const std::string& yesterday) {
    std::string date = datetime.substr(0, 10);
    if (date == today) return "сегодня";
    if (date == yesterday) return "вчера";
    std::string day_month = text::format_day_month(date);
    if (date.compare(0, 4, today, 0, 4) == 0) return day_month;
    return day_month + "." + date.substr(0, 4);
}

// Символ валюты по ISO из кошельков пользователя, иначе сам ISO-код.
std::string currency_label(const std::string& iso, const std::vector<resolver::Wallet>& wallets) {
    for (const auto& w : wallets) {
        if (w.iso_code == iso && !w.symbol.empty()) return w.symbol;
    }
    return iso;
}

std::string wallet_button_text(const resolver::Wallet& w) {
    std::string cur = w.symbol.empty() ? w.iso_code : w.symbol;
    return cur.empty() ? w.name : w.name + " · " + cur;
}

// Кнопки по две в ряд.
void add_pairs(Keyboard& kb, const std::vector<Button>& buttons) {
    for (std::size_t i = 0; i < buttons.size(); i += 2) {
        std::vector<Button> row{buttons[i]};
        if (i + 1 < buttons.size()) row.push_back(buttons[i + 1]);
        kb.push_back(std::move(row));
    }
}

Button cancel_button(const Draft& d) { return {"✕ Не записывать", callback::make(d.id, callback::Kind::Cancel)}; }

Button category_button(const Draft& d, std::size_t i) {
    return {d.categories[i].name, callback::make(d.id, callback::Kind::PickCategory, static_cast<int>(i))};
}

void render_category(const Draft& d, Message& m) {
    using Why = resolver::CategoryPick::Why;
    const std::string& asked = d.expense.category;
    if (d.categories.empty()) {
        m.text += "Категорий пока нет — заведите их в приложении.";
        m.keyboard.push_back({cancel_button(d)});
        return;
    }

    std::vector<Button> buttons;
    if (d.category_page < 0) {
        if (d.category_why == Why::Ambiguous) m.text += "Категория «" + asked + "» — какая именно?";
        else if (d.category_why == Why::NotFound) m.text += "Нет категории «" + asked + "». Может быть:";
        else m.text += "Какая категория?";
        for (std::size_t i : d.category_candidates) buttons.push_back(category_button(d, i));
        add_pairs(m.keyboard, buttons);
        if (d.categories.size() > d.category_candidates.size()) {
            m.keyboard.push_back({{"Другая…", callback::make(d.id, callback::Kind::CategoryPage, 0)}});
        }
        m.keyboard.push_back({cancel_button(d)});
        return;
    }

    int pages = page_count(d);
    int page = std::clamp(d.category_page, 0, pages - 1);
    m.text += d.category_why == Why::NotFound ? "Нет категории «" + asked + "». Выберите:" : "Какая категория?";
    if (pages > 1) m.text += " Стр. " + std::to_string(page + 1) + " из " + std::to_string(pages) + ".";
    std::size_t from = static_cast<std::size_t>(page) * kPageSize;
    std::size_t to = std::min(from + kPageSize, d.categories.size());
    for (std::size_t i = from; i < to; ++i) buttons.push_back(category_button(d, i));
    add_pairs(m.keyboard, buttons);

    std::vector<Button> nav;
    if (page > 0) nav.push_back({"‹ Назад", callback::make(d.id, callback::Kind::CategoryPage, page - 1)});
    if (page + 1 < pages) nav.push_back({"Ещё ›", callback::make(d.id, callback::Kind::CategoryPage, page + 1)});
    if (!nav.empty()) m.keyboard.push_back(std::move(nav));
    m.keyboard.push_back({cancel_button(d)});
}

void render_wallet(const Draft& d, Message& m) {
    using Why = resolver::WalletPick::Why;
    const std::string& hint = d.expense.currency_hint;
    switch (d.wallet_why) {
    case Why::NotFound:
        m.text += "Нет кошелька «" + d.expense.wallet + "». Какой?";
        break;
    case Why::Ambiguous:
        m.text += "Кошелёк «" + d.expense.wallet + "» — какой именно?";
        break;
    case Why::CurrencyMismatch: {
        const resolver::Wallet& named = d.wallets[*d.wallet_force];
        m.text += "Сумма в " + hint + ", а кошелёк «" + named.name + "» — в " + named.iso_code + ".";
        m.text += d.wallet_candidates.empty() ? " Кошельков в " + hint + " нет, конвертации тоже."
                                              : " Записать в кошелёк в " + hint + "?";
        break;
    }
    default:
        m.text += "Какой кошелёк?";
        break;
    }

    std::vector<Button> buttons;
    for (std::size_t i : d.wallet_candidates) {
        buttons.push_back(
            {wallet_button_text(d.wallets[i]), callback::make(d.id, callback::Kind::PickWallet, static_cast<int>(i))});
    }
    add_pairs(m.keyboard, buttons);
    if (d.wallet_force) {
        m.keyboard.push_back({{"Всё равно в «" + d.wallets[*d.wallet_force].name + "»",
                               callback::make(d.id, callback::Kind::PickWallet, static_cast<int>(*d.wallet_force))}});
    }
    m.keyboard.push_back({cancel_button(d)});
}

void render_card(const Draft& d, const std::string& default_wallet_uuid, Message& m) {
    if (d.attempted) {
        m.text += "Не удалось связаться с сервером — возможно, трата уже записана. Нажмите, чтобы проверить.";
        m.keyboard.push_back({{"✓ Проверить ещё раз", callback::make(d.id, callback::Kind::Save)}});
        return;
    }
    m.text += kConfirm;
    m.keyboard.push_back({{"✓ Записать", callback::make(d.id, callback::Kind::Save)}});
    m.keyboard.push_back({{"Категория…", callback::make(d.id, callback::Kind::EditCategory)},
                          {"Кошелёк…", callback::make(d.id, callback::Kind::EditWallet)}});
    if (d.offer_remember && d.wallet && d.wallets[*d.wallet].uuid != default_wallet_uuid) {
        m.keyboard.push_back({{"★ Сделать кошелёк основным", callback::make(d.id, callback::Kind::MakeDefault)}});
    }
    m.keyboard.push_back({cancel_button(d)});
}

} // namespace

int page_count(const Draft& d) {
    return std::max<int>(1, static_cast<int>((d.categories.size() + kPageSize - 1) / kPageSize));
}

std::string summary(const Draft& d, const std::string& today, const std::string& yesterday) {
    const parser::ParsedExpense& e = d.expense;
    std::string amount;
    if (d.wallet) {
        const resolver::Wallet& w = d.wallets[*d.wallet];
        amount = text::format_amount(e.amount, std::to_string(w.precision), w.symbol);
    } else {
        amount = text::format_amount(e.amount, std::to_string(e.fraction_digits),
                                     e.currency_hint.empty() ? std::string{} : currency_label(e.currency_hint, d.wallets));
    }
    std::string category = d.category ? d.categories[*d.category].name : "категория?";
    std::string wallet = d.wallet ? d.wallets[*d.wallet].name + (d.wallet_by_currency ? " (по валюте)" : "") : "кошелёк?";

    std::string out = amount + " · " + category + " · " + wallet + " · " + expense_date(e.datetime, today, yesterday);
    if (!e.comment.empty()) out += " — " + text::truncate_utf8(e.comment, 80);
    return out;
}

Message render(const Draft& d, const std::string& default_wallet_uuid, const std::string& today,
               const std::string& yesterday) {
    Message m;
    if (d.step == Draft::Step::Done) {
        m.text = "✓ Записано: " + summary(d, today, yesterday);
        return m;
    }
    // Из свободного текста модель могла понять не то — попросить проверить.
    m.text = std::string(d.from_text ? "Понял из текста — проверьте перед записью: " : "Понял так: ") +
             summary(d, today, yesterday) + "\n\n";
    switch (d.step) {
    case Draft::Step::Category:
        render_category(d, m);
        break;
    case Draft::Step::Wallet:
        render_wallet(d, m);
        break;
    case Draft::Step::Card:
        render_card(d, default_wallet_uuid, m);
        break;
    case Draft::Step::Done:
        break;
    }
    return m;
}

} // namespace card
