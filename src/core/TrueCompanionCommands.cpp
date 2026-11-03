/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionCommands.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace truecompanion
{
    namespace
    {
        std::string Lower(std::string s)
        {
            for (auto& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string Trim(std::string s)
        {
            size_t b = s.find_first_not_of(' ');
            if (b == std::string::npos)
                return "";
            size_t e = s.find_last_not_of(' ');
            return s.substr(b, e - b + 1);
        }

        bool StartsWith(std::string const& s, std::string const& p) { return s.compare(0, p.size(), p) == 0; }

        bool EndsWith(std::string const& s, std::string const& p)
        {
            return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
        }

        // Removes the first matching prefix (whole words). Returns true if one was removed.
        bool StripPrefix(std::string& s, std::initializer_list<char const*> prefixes)
        {
            for (char const* p : prefixes)
            {
                std::string pre = p;
                if (s == pre)
                {
                    s.clear();
                    return true;
                }
                if (StartsWith(s, pre + " "))
                {
                    s = Trim(s.substr(pre.size() + 1));
                    return true;
                }
            }
            return false;
        }

        bool StripSuffix(std::string& s, std::initializer_list<char const*> suffixes)
        {
            for (char const* p : suffixes)
            {
                std::string suf = std::string(" ") + p;
                if (EndsWith(s, suf))
                {
                    s = Trim(s.substr(0, s.size() - suf.size()));
                    return true;
                }
            }
            return false;
        }

        // Lowercase, punctuation to spaces (apostrophes kept), collapsed spaces.
        std::string Normalize(std::string const& text)
        {
            std::string out;
            for (char ch : Lower(text))
            {
                unsigned char c = static_cast<unsigned char>(ch);
                bool keep = std::isalnum(c) || c == '\'' || c >= 0x80;
                char put = keep ? ch : ' ';
                if (put == ' ' && (out.empty() || out.back() == ' '))
                    continue;
                out += put;
            }
            return Trim(out);
        }

        void StripAddress(std::string& s, std::string const& name)
        {
            if (name.empty())
                return;
            std::string n = Lower(name);
            StripPrefix(s, { "hey", "hi", "oh", "ok", "okay" });
            if (s == n)
                s.clear();
            else if (StartsWith(s, n + " "))
                s = Trim(s.substr(n.size() + 1));
            if (EndsWith(s, " " + n))
                s = Trim(s.substr(0, s.size() - n.size() - 1));
        }

        bool StripGroupAddress(std::string& s)
        {
            return StripPrefix(s, { "hey everyone", "hey everybody", "hey guys", "hey girls", "hey all", "everyone", "everybody",
                                    "all of you", "you all", "y'all", "you guys", "guys", "girls", "ladies", "team", "both of you",
                                    "you two", "you both", "both", "alle", "ihr alle", "ihr beiden", "leute" });
        }

        void StripPoliteness(std::string& s)
        {
            bool again = true;
            while (again && !s.empty())
            {
                again = StripPrefix(s, { "hey", "please", "pls", "plz", "can you", "could you", "would you", "will you",
                                         "can u", "could u", "would you mind", "i want you to", "i need you to",
                                         "i'd like you to", "go and", "quickly", "now", "just", "kannst du", "bitte", "könntest du" });
                again = StripSuffix(s, { "please", "pls", "plz", "for me", "now", "right now", "bitte", "quickly" }) || again;
            }
        }

        void StripDeterminers(std::string& s)
        {
            StripPrefix(s, { "all of your", "all your", "all of the", "all the", "all", "your", "the", "some", "a", "an",
                             "that", "this", "those", "these", "one of your", "one", "deine", "dein", "das", "den", "die", "der" });
        }

        bool IsPronounTarget(std::string const& s)
        {
            static char const* const words[] = { "", "it", "him", "her", "them", "that", "this", "my target", "the target",
                                                 "target", "what i'm targeting", "what i am targeting", "what i target",
                                                 "that one", "this one", "whatever i'm attacking", "with me" };
            for (char const* w : words)
                if (s == w)
                    return true;
            return false;
        }

        // Things you "give" a person that are not items.
        bool IsAbstractGift(std::string const& s)
        {
            static char const* const words[] = { "hug", "kiss", "hand", "break", "chance", "minute", "second", "moment", "smile",
                                                 "advice", "attention", "time", "love", "hint", "reason", "name", "answer",
                                                 "a hug", "a kiss", "a hand", "a break", "a chance", "a minute", "a second",
                                                 "a moment", "a smile", "a hint", "some time", "some space", "space",
                                                 "an answer", "a reason", "your opinion", "opinion", "idea", "an idea",
                                                 "strength", "courage", "hope", "a sign", "sign", "peace", "company", "it", "that",
                                                 "something", "anything", "everything", "me", "yourself", "you" };
            for (char const* w : words)
                if (s == w)
                    return true;
            return false;
        }

        // A count at the start of `s` ("5 linen cloth", "3 x copper ore"), removed from `s`. nullopt: no count there.
        // An explicit 0 or a count with too many digits is returned as 0 (the caller rejects it).
        std::optional<uint64_t> LeadingCount(std::string& s)
        {
            size_t i = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == 0 || i >= s.size() || s[i] != ' ')
                return std::nullopt;
            uint64_t n = i > 6 ? 0 : std::stoull(s.substr(0, i));
            s = Trim(s.substr(i + 1));
            if (StartsWith(s, "x "))
                s = Trim(s.substr(2));
            return n;
        }

        // "one linen cloth", "two stacks of silk": a spelled-out count is a count, not a determiner.
        // Consumes the word (and a following "of"), so the item name is what is left.
        std::optional<uint64_t> LeadingNumberWord(std::string& s)
        {
            static char const* const words[] = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten" };
            for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
            {
                std::string w = words[i];
                if (s != w && !StartsWith(s, w + " "))
                    continue;
                s = Trim(s.substr(w.size()));
                if (StartsWith(s, "of "))
                    s = Trim(s.substr(3));
                return uint64_t(i + 1);
            }
            return std::nullopt;
        }

        // A number the player typed that is not a whole positive count: "-5", "1.5", "1,000", "2'000".
        // Works on the raw text (Normalize turns "1.5" into "1 5").
        bool HasMalformedNumber(std::string const& raw)
        {
            for (size_t i = 0; i < raw.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(raw[i])))
                    continue;
                size_t j = i;
                while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j])))
                    ++j;
                bool negative = i > 0 && raw[i - 1] == '-' && (i == 1 || !std::isalnum(static_cast<unsigned char>(raw[i - 2])));
                bool separated = j + 1 < raw.size() && (raw[j] == '.' || raw[j] == ',' || raw[j] == '\'') &&
                                 std::isdigit(static_cast<unsigned char>(raw[j + 1]));
                if (negative || separated)
                    return true;
                i = j;
            }
            return false;
        }

        struct MoneyScan
        {
            uint64_t copper = 0;
            bool unit = false;   // at least one "<number> <gold|silver|copper>"
            bool bad = false;    // a number with a money unit that can't be used (too many digits, or 0 in total)
        };

        // "10g", "5 gold 20 silver", "1g50s" in normalized text.
        MoneyScan ScanMoney(std::string const& s)
        {
            MoneyScan m;
            size_t i = 0;
            while (i < s.size())
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                    ++j;
                size_t k = j;
                if (k < s.size() && s[k] == ' ')
                    ++k;
                size_t w = k;
                while (w < s.size() && std::isalpha(static_cast<unsigned char>(s[w])))
                    ++w;
                std::string unit = s.substr(k, w - k);
                uint64_t mult = (unit == "g" || unit == "gold" || unit == "gc" || unit == "gp") ? 10000
                                : (unit == "s" || unit == "silver")                             ? 100
                                : (unit == "c" || unit == "copper")                             ? 1
                                                                                                : 0;
                // "5 copper ore" / "1 gold bar" name an item: a money unit is only money when the next word
                // is a connector or ends the phrase, never another noun.
                if (mult)
                {
                    size_t nx = w;
                    if (nx < s.size() && s[nx] == ' ')
                        ++nx;
                    size_t ne = nx;
                    while (ne < s.size() && std::isalpha(static_cast<unsigned char>(s[ne])))
                        ++ne;
                    std::string next = s.substr(nx, ne - nx);
                    static char const* const connectors[] = { "and", "plus", "then", "also", "too", "more", "please",
                                                              "bitte", "for", "to", "now", "ok", "okay", "in", "total",
                                                              "altogether", "worth" };
                    bool namesItem = !next.empty() &&
                        std::find(std::begin(connectors), std::end(connectors), next) == std::end(connectors);
                    if (!namesItem)
                    {
                        m.unit = true;
                        if (j - i > 7)
                            m.bad = true;
                        else
                            m.copper += std::stoull(s.substr(i, j - i)) * mult;
                    }
                }
                i = w > j ? w : j;
            }
            if (m.unit && m.copper == 0)
                m.bad = true;
            return m;
        }

        bool IsJunk(std::string const& s)
        {
            static char const* const words[] = { "junk", "gray", "grey", "grays", "greys", "trash", "gray items", "grey items",
                                                 "junk items", "trash items", "vendor trash", "garbage", "useless stuff",
                                                 "useless items", "worthless stuff", "worthless items", "loot", "müll", "schrott" };
            for (char const* w : words)
                if (s == w)
                    return true;
            return false;
        }
    }

    char const* CompanionCommandId(CompanionCommandKind k)
    {
        switch (k)
        {
            case CompanionCommandKind::Cast: return "cast";
            case CompanionCommandKind::GiveItem: return "give_item";
            case CompanionCommandKind::GiveGold: return "give_gold";
            case CompanionCommandKind::AttackNamed: return "attack_named";
            case CompanionCommandKind::AttackTarget: return "attack_target";
            case CompanionCommandKind::AttackAll: return "attack_all";
            case CompanionCommandKind::StopAttack: return "stop_attack";
            case CompanionCommandKind::AcceptQuests: return "accept_quests";
            case CompanionCommandKind::TurnInQuests: return "turn_in_quests";
            case CompanionCommandKind::SellJunk: return "sell_junk";
            case CompanionCommandKind::SellItem: return "sell_item";
            case CompanionCommandKind::ShareQuests: return "share_quests";
            case CompanionCommandKind::ShareQuest: return "share_quest";
            case CompanionCommandKind::Lead: return "lead";
            case CompanionCommandKind::StopLead: return "stop_lead";
            case CompanionCommandKind::PauseLead: return "pause_lead";
            case CompanionCommandKind::ResumeLead: return "resume_lead";
            case CompanionCommandKind::LearnSpells: return "learn_spells";
            default: return "none";
        }
    }

    uint64_t ParseMoney(std::string const& lowerText)
    {
        if (HasMalformedNumber(lowerText))
            return 0;
        MoneyScan m = ScanMoney(Normalize(lowerText));
        return m.bad ? 0 : m.copper;
    }

    std::optional<CompanionCommand> ParseCompanionCommand(std::string const& text, std::string const& companionName,
                                                          std::string const& otherName)
    {
        std::string s = Normalize(text);
        bool malformed = HasMalformedNumber(Lower(text));   // "-5", "1.5", "1,000": Normalize hides them
        StripGroupAddress(s);
        StripAddress(s, companionName);
        StripAddress(s, otherName);
        StripPoliteness(s);
        StripAddress(s, companionName);
        StripAddress(s, otherName);
        StripGroupAddress(s);
        StripPoliteness(s);
        if (s.empty())
            return std::nullopt;
        // Questions about her state ("what spells do you have") are knowledge, not commands.
        if (StartsWith(s, "what ") || StartsWith(s, "which ") || StartsWith(s, "how ") || StartsWith(s, "do you ") ||
            StartsWith(s, "does ") || StartsWith(s, "why "))
            return std::nullopt;

        CompanionCommand c;

        // ---- lead / follow
        // pause / resume while leading (checked before "lead ..." so "lead again" resumes)
        static char const* const pause[] = { "wait", "wait here", "wait a moment", "wait a second", "wait a sec", "hold on", "hold up",
                                             "stay", "stay here", "stop here", "halt", "wait for me", "warte", "warte hier", "warte kurz" };
        for (char const* p : pause)
            if (s == p)
            {
                c.kind = CompanionCommandKind::PauseLead;
                return c;
            }
        static char const* const resume[] = { "keep leading", "continue leading", "lead on", "go on", "keep going", "carry on",
                                              "continue", "move on", "let's go", "lets go", "let's continue", "weiter", "geh weiter",
                                              "führ weiter", "los geht's" };
        for (char const* p : resume)
            if (s == p || s == std::string(p) + " again")
            {
                c.kind = CompanionCommandKind::ResumeLead;
                return c;
            }
        if (s == "lead" || s == "lead again" || s == "lead the way again" || s == "lead us again" || s == "you lead again")
        {
            c.kind = CompanionCommandKind::Lead;   // resumes a paused lead, otherwise starts one
            return c;
        }

        static char const* const lead[] = { "lead the way", "you lead", "take the lead", "you go first", "lead us", "lead me",
                                            "take me", "bring me", "walk me",
                                            "show me the way", "show us the way", "go ahead", "führ uns", "geh vor", "du führst" };
        for (char const* p : lead)
        {
            std::string pre = p;
            if (s == pre || StartsWith(s, pre + " "))
            {
                c.kind = CompanionCommandKind::Lead;
                std::string rest = Trim(s.substr(pre.size()));
                if (StripPrefix(rest, { "to the quest", "to quest", "to", "for the quest", "for quest", "for" }))
                {
                    StripDeterminers(rest);
                    StripSuffix(rest, { "quest" });
                    c.arg = rest;
                }
                return c;
            }
        }
        if (StartsWith(s, "let's do ") || StartsWith(s, "lets do ") || StartsWith(s, "let us do "))
        {
            std::string rest = s.substr(s.find(" do ") + 4);
            StripPrefix(rest, { "the quest", "quest" });
            StripDeterminers(rest);
            StripSuffix(rest, { "quest" });
            if (!rest.empty() && rest != "it" && rest != "this" && rest != "that")
            {
                c.kind = CompanionCommandKind::Lead;
                c.arg = rest;
                return c;
            }
        }
        static char const* const stopLead[] = { "stop leading", "i'll lead", "i will lead", "let me lead", "i lead",
                                                "i lead now", "follow me again", "stop questing" };
        for (char const* p : stopLead)
            if (s == p || StartsWith(s, std::string(p) + " "))
            {
                c.kind = CompanionCommandKind::StopLead;
                return c;
            }

        static char const* const stopAttack[] = { "stop attacking", "stop fighting", "stop killing", "stop hunting", "stop the attack",
                                                  "hold your fire", "cease fire", "hör auf anzugreifen", "hört auf" };
        for (char const* p : stopAttack)
            if (s == p || StartsWith(s, std::string(p) + " "))
            {
                c.kind = CompanionCommandKind::StopAttack;
                return c;
            }

        // Negated or stopping sentences never trigger an action ("don't attack", "stop selling").
        if (StartsWith(s, "don't ") || StartsWith(s, "dont ") || StartsWith(s, "do not ") || StartsWith(s, "never ") ||
            StartsWith(s, "stop ") || StartsWith(s, "nicht ") || StartsWith(s, "no "))
            return std::nullopt;

        // ---- class trainer: "learn your spells", "go to your trainer", "train"
        {
            std::string t = s;
            StripPrefix(t, { "go and", "go", "please" });
            static char const* const exact[] = { "train", "get trained", "go train", "train up", "visit your trainer", "visit the trainer",
                                                 "to your trainer", "to the trainer", "to your class trainer", "see your trainer" };
            bool learn = false;
            for (char const* p : exact)
                if (t == p)
                    learn = true;
            if (!learn && (StartsWith(t, "learn ") || StartsWith(t, "train ") || StartsWith(t, "lern ")) &&
                (t.find("spell") != std::string::npos || t.find("abilit") != std::string::npos || t.find("skills") != std::string::npos ||
                 t.find("zauber") != std::string::npos || t.find("fähigkeit") != std::string::npos))
                learn = true;
            if (learn)
            {
                c.kind = CompanionCommandKind::LearnSpells;
                return c;
            }
        }

        // ---- quests at quest givers
        // (worked on a copy: "take down the wolf" must still reach the attack rules)
        {
            std::string t = s;
            bool accept = StripPrefix(t, { "go and pick up", "go and get", "go pick up", "go get", "go grab", "fetch", "accept", "take",
                                           "get", "pick up", "grab", "hol dir", "nimm", "nehmt", "annehmen" });
            bool turnIn = !accept && StripPrefix(t, { "turn in", "hand in", "complete", "finish", "deliver", "abgeben", "gib ab" });
            if (accept || turnIn)
            {
                // "pick up all your quests" / "pick up your quests" = everything around; "a quest" / "the quest" = the nearest one
                bool everything = StartsWith(t, "all ") || StartsWith(t, "every") || t == "everything";
                bool plural = everything || t.find("quests") != std::string::npos;
                StripDeterminers(t);
                bool quest = StripPrefix(t, { "quests", "quest" }) || StripSuffix(t, { "quests", "quest" });
                if (quest || (turnIn && t.empty()))
                {
                    StripPrefix(t, { "too", "as well", "from here", "here", "available", "new", "nearby", "around here", "in the area" });
                    StripSuffix(t, { "too", "as well", "from here", "here", "from him", "from her", "from them", "quests", "quest",
                                     "around here", "nearby", "in the area" });
                    StripDeterminers(t);
                    StripPrefix(t, { "available", "new" });
                    c.kind = accept ? CompanionCommandKind::AcceptQuests : CompanionCommandKind::TurnInQuests;
                    c.arg = t;
                    c.all = accept && plural && t.empty();
                    return c;
                }
            }
        }

        // ---- sell
        // ("let's sell trash", "we should sell our junk", "go sell your stuff": only in front of "sell", since
        // "let's do ..." and "let's go" mean something else)
        if (StripPrefix(s, { "let's go sell", "lets go sell", "let's sell", "lets sell", "let us sell", "we should sell", "we need to sell",
                             "we have to sell", "you should sell", "time to sell", "go and sell", "go sell", "lass uns verkaufen",
                             "sell", "vendor", "get rid of", "verkauf", "verkaufe" }))
        {
            // "vendor, sell trash" / "vendor sell your junk"
            StripPrefix(s, { "sell", "verkauf", "verkaufe" });
            StripDeterminers(s);
            StripPrefix(s, { "our", "my" });
            StripSuffix(s, { "now", "here", "at the vendor", "to the vendor", "to a vendor", "at a vendor", "at the merchant",
                             "to the merchant", "at a merchant", "to a merchant", "beim händler" });
            if (s.empty() || IsJunk(s))
                c.kind = CompanionCommandKind::SellJunk;
            else
            {
                c.kind = CompanionCommandKind::SellItem;
                if (auto n = LeadingCount(s))
                {
                    c.amountGiven = true;
                    c.amount = *n;
                }
                c.badAmount = malformed || (c.amountGiven && c.amount == 0);
                StripPrefix(s, { "of your", "of my", "of the", "of" });
                StripDeterminers(s);
                c.arg = s;
            }
            return c;
        }

        // ---- share quests
        if (StripPrefix(s, { "share", "teile" }))
        {
            StripDeterminers(s);
            StripSuffix(s, { "with me", "with us" });
            if (s == "quests" || s == "quest" || s == "your quests" || s == "quests with me" || s == "quest log" || s == "quests mit mir")
            {
                c.kind = CompanionCommandKind::ShareQuests;
                return c;
            }
            if (StripPrefix(s, { "quest" }) || StripSuffix(s, { "quest" }))
            {
                StripDeterminers(s);
                c.kind = s.empty() ? CompanionCommandKind::ShareQuests : CompanionCommandKind::ShareQuest;
                c.arg = s;
                return c;
            }
            return std::nullopt;
        }

        // ---- give gold / items
        // "i need ..." / "i want ..." only count for money ("I need 10 gold"), never for items ("I need healing").
        bool moneyOnly = false;
        bool give = StripPrefix(s, { "give me", "hand me", "pass me", "send me", "lend me", "trade me", "toss me", "throw me",
                                     "can i have", "could i have", "may i have", "can i get", "could i get", "gib mir", "leih mir" });
        if (!give && StripPrefix(s, { "i need", "i want", "ich brauche", "ich will" }))
            give = moneyOnly = true;
        if (!give && StartsWith(s, "trade "))
        {
            // "trade your linen cloth to me"
            std::string rest = s.substr(6);
            if (StripSuffix(rest, { "to me", "with me" }))
            {
                s = rest;
                give = true;
            }
        }
        if (give)
        {
            StripSuffix(s, { "please", "if you have it", "if you have some", "if you have any", "if you can" });
            // "give me all your money / gold / copper"
            {
                std::string t = s;
                bool everything = StripPrefix(t, { "all of your", "all your", "all of the", "all the", "all", "every" });
                StripPrefix(t, { "single", "last" });
                if (everything && (t == "money" || t == "gold" || t == "copper" || t == "silver" || t == "coins" ||
                                   t == "geld" || t == "your money" || t == "of your money" || t == "coin" || t == "piece of gold"))
                {
                    c.kind = CompanionCommandKind::GiveGold;
                    c.arg = "all";
                    return c;
                }
            }
            // money: "10 gold", "2g 50s"; a broken amount ("1.5 gold", "-5 gold", "1,000 gold") is refused, never read
            // as a different amount or as an item called "000 gold"
            MoneyScan money = ScanMoney(s);
            if (money.unit)
            {
                c.kind = CompanionCommandKind::GiveGold;
                c.badAmount = malformed || money.bad;
                c.amount = c.badAmount ? 0 : money.copper;
                c.amountGiven = true;
                return c;
            }
            if (moneyOnly || s == "gold" || s == "money" || s == "some gold" || s == "some money" || s == "coins" || s == "geld")
                return std::nullopt; // no amount (or not about money): let her talk about it
            // "give me one linen cloth" is one, not the whole stack: a spelled-out count is a count, not a determiner
            if (auto w = LeadingNumberWord(s))
            {
                c.amountGiven = true;
                c.amount = *w;
            }
            StripDeterminers(s);
            if (auto n = LeadingCount(s))
            {
                c.amountGiven = true;
                c.amount = *n;
            }
            StripDeterminers(s);
            if (s.empty() || IsAbstractGift(s))
                return std::nullopt;
            c.kind = CompanionCommandKind::GiveItem;
            c.badAmount = malformed || (c.amountGiven && c.amount == 0);
            c.arg = s;
            return c;
        }

        // ---- cast
        if (StripPrefix(s, { "cast", "use your", "use", "zaubere", "wirke" }))
        {
            if (StripSuffix(s, { "on me", "at me", "auf mich" }))
                c.onMe = true;
            else
                StripSuffix(s, { "on yourself", "on you" });
            StripDeterminers(s);
            StripSuffix(s, { "spell" });
            if (s.empty() || s == "it" || s == "something" || s == "magic")
                return std::nullopt;
            c.kind = CompanionCommandKind::Cast;
            c.arg = s;
            return c;
        }

        // ---- attack
        if (StripPrefix(s, { "attack", "kill", "fight", "pull", "go for", "take down", "take out", "go after", "clear", "clean up",
                             "greif", "töte", "angreifen" }))
        {
            StripSuffix(s, { "an", "with me", "for me", "first" });
            // "attack all here", "kill everything", "kill them all", "kill all wolves"
            bool all = StripPrefix(s, { "everything", "everyone", "everybody", "all of them", "them all", "all enemies", "all mobs",
                                        "every enemy", "every mob", "all monsters", "all", "alles", "alle" });
            if (!all)
                all = StripSuffix(s, { "all", "all of them" });
            StripDeterminers(s);
            StripSuffix(s, { "over there", "there", "here", "nearby", "near us", "in front of us", "around us", "around here", "in the area" });
            StripPrefix(s, { "here", "around here", "nearby", "in the area" });
            if (all)
            {
                c.kind = CompanionCommandKind::AttackAll;
                if (s != "enemies" && s != "mobs" && s != "monsters" && s != "of them" && s != "them")
                    c.arg = s;
                return c;
            }
            if (s == "area" || s == "the area")
            {
                c.kind = CompanionCommandKind::AttackAll;
                return c;
            }
            if (IsPronounTarget(s))
            {
                c.kind = CompanionCommandKind::AttackTarget;
                return c;
            }
            c.kind = CompanionCommandKind::AttackNamed;
            c.arg = s;
            return c;
        }
        return std::nullopt;
    }

    uint32_t DetectKnowledgeTopics(std::string const& text)
    {
        std::string s = " " + Normalize(text) + " ";
        auto has = [&](std::initializer_list<char const*> words)
        {
            for (char const* w : words)
                if (s.find(std::string(" ") + w + " ") != std::string::npos)
                    return true;
            return false;
        };
        uint32_t t = 0;
        if (has({ "inventory", "bag", "bags", "backpack", "carrying", "carry", "items", "item", "loot", "stuff", "what do you have",
                  "what have you got", "do you have", "have you got", "tasche", "taschen", "inventar", "rucksack" }))
            t |= knowledge::Inventory;
        if (has({ "quest", "quests", "questlog", "quest log", "mission", "missions", "task", "tasks", "objective", "objectives",
                  "aufgabe", "aufgaben" }))
            t |= knowledge::Quests;
        if (has({ "spell", "spells", "abilities", "ability", "skills", "magic", "cast", "talents", "zauber", "fähigkeiten" }))
            t |= knowledge::Spells;
        if (has({ "gold", "money", "coin", "coins", "rich", "broke", "silver", "copper", "geld" }))
            t |= knowledge::Gold;
        if (has({ "xp", "exp", "experience", "level", "levels", "lvl", "level up", "erfahrung", "stufe" }))
            t |= knowledge::Xp;
        if (has({ "looking at", "my target", "targeting", "target", "who is that", "who's that", "what is that", "what's that",
                  "who am i looking", "that guy", "that thing", "this guy", "this thing", "ziel" }))
            t |= knowledge::Target;
        if (has({ "gear", "equipment", "armor", "armour", "weapon", "wearing", "equipped", "ausrüstung", "rüstung", "waffe" }))
            t |= knowledge::Gear;
        return t;
    }

    bool AddressesEveryone(std::string const& text)
    {
        std::string s = Normalize(text);
        StripPrefix(s, { "hey", "hi", "ok", "okay", "so" });
        if (StripGroupAddress(s))
            return true;
        s = " " + s + " ";
        for (char const* w : { " everyone ", " everybody ", " all of you ", " you all ", " y'all ", " you guys ", " both of you ",
                               " you two ", " ihr alle ", " ihr beiden " })
            if (s.find(w) != std::string::npos)
                return true;
        return false;
    }

    uint32_t CreatureTypeFromWord(std::string const& wordIn)
    {
        std::string w = Normalize(wordIn);
        struct T { char const* w; uint32_t id; };
        static T const types[] = {
            { "beast", 1 }, { "beasts", 1 }, { "animal", 1 }, { "animals", 1 },
            { "dragonkin", 2 }, { "dragon", 2 }, { "dragons", 2 }, { "whelp", 2 }, { "whelps", 2 },
            { "demon", 3 }, { "demons", 3 },
            { "elemental", 4 }, { "elementals", 4 },
            { "giant", 5 }, { "giants", 5 },
            { "undead", 6 }, { "zombie", 6 }, { "zombies", 6 }, { "skeleton", 6 }, { "skeletons", 6 }, { "ghoul", 6 }, { "ghouls", 6 },
            { "humanoid", 7 }, { "humanoids", 7 }, { "people", 7 },
            { "critter", 8 }, { "critters", 8 },
            { "mechanical", 9 }, { "mechanicals", 9 }, { "robot", 9 }, { "robots", 9 },
        };
        for (auto const& t : types)
            if (w == t.w)
                return t.id;
        return 0;
    }
}
