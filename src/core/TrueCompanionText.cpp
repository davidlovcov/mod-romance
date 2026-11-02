/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionText.h"
#include "TrueCompanionMemory.h"

#include <algorithm>
#include <cctype>

namespace truecompanion
{
    namespace
    {
        // Chat commands understood by mod-playerbots (ChatCommandHandlerStrategy and the common
        // movement/combat strategies) at commit 05a1f6f. Only the first word(s) are matched.
        char const* const kPlayerbotCommands[] = {
            "follow", "stay", "guard", "free", "flee", "attack", "grind", "passive", "tank attack", "max dps",
            "co", "nc", "de", "react", "ss", "ll", "q", "u", "use", "e", "ue", "t", "nt", "s", "b", "bank", "gb",
            "c", "talk", "cast", "castnc", "spell", "spells", "talents", "glyphs", "glyph", "stats", "quests",
            "reputation", "rep", "pvp stats", "leave", "release", "repair", "emblems", "trainer", "maintenance",
            "remove", "autogear", "equip", "upgrade", "chat", "home", "destroy", "reset", "emote", "buff", "help",
            "invite", "lfg", "rti", "position", "summon", "who", "save mana", "formation", "stance", "cancel",
            "sendmail", "mail", "outfit", "go", "debug", "cdebug", "cs", "wts", "hire", "craft", "flag", "range",
            "ra", "give", "leader", "cheat", "ginvite", "rtsc", "drink", "eat", "calc", "roll", "open items",
            "unlock items", "unlock traded item", "wipe", "tame", "pet", "pet attack", "wait for attack time",
            "loot", "add all loot", "taxi", "teleport", "items", "inventory", "inv", "log", "los", "aura", "drop",
            "share", "accept", "do", "d", "rpg", "rpg status", "travel", "move from group", "naxx", "mount",
            "ready", "ready check", "whisper", "join", "focus heal", "boost", "disperse", "tell", "nc ?", "co ?",
            "keep", "sell", "buy", "bags", "vendor" };

        bool StartsWithWord(std::string const& s, std::string const& word)
        {
            if (s.compare(0, word.size(), word) != 0)
                return false;
            return s.size() == word.size() || s[word.size()] == ' ' || s[word.size()] == '+' || s[word.size()] == '-' ||
                   s[word.size()] == ',' || s[word.size()] == '=';
        }

        bool ContainsPhrase(std::string const& hay, std::string const& phrase)
        {
            size_t pos = 0;
            while ((pos = hay.find(phrase, pos)) != std::string::npos)
            {
                bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
                size_t end = pos + phrase.size();
                bool rightOk = end >= hay.size() || !std::isalnum(static_cast<unsigned char>(hay[end]));
                if (leftOk && rightOk)
                    return true;
                ++pos;
            }
            return false;
        }

        // Word stem at a word start: "propos" matches "propose" and "proposal", but not "unproposed".
        bool ContainsStem(std::string const& hay, std::string const& stem)
        {
            size_t pos = 0;
            while ((pos = hay.find(stem, pos)) != std::string::npos)
            {
                if (pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1])))
                    return true;
                ++pos;
            }
            return false;
        }

        bool NegatedBefore(std::string const& hay, std::string const& phrase)
        {
            size_t pos = hay.find(phrase);
            if (pos == std::string::npos)
                return false;
            std::string before = hay.substr(pos >= 24 ? pos - 24 : 0, pos >= 24 ? 24 : pos);
            for (char const* n : { "don't ", "dont ", "do not ", "never ", "nicht ", "nie ", "stop " })
                if (before.find(n) != std::string::npos)
                    return true;
            return false;
        }

        size_t WordCount(std::string const& s)
        {
            size_t n = 0;
            bool in = false;
            for (char c : s)
            {
                if (c == ' ')
                    in = false;
                else if (!in)
                {
                    in = true;
                    ++n;
                }
            }
            return n;
        }

        std::string StripLeadingName(std::string const& lower, std::string const& lowerName)
        {
            if (lowerName.empty())
                return lower;
            if (lower.compare(0, lowerName.size(), lowerName) == 0)
            {
                std::string rest = lower.substr(lowerName.size());
                size_t i = 0;
                while (i < rest.size() && (rest[i] == ',' || rest[i] == ' ' || rest[i] == ':' || rest[i] == '!'))
                    ++i;
                return rest.substr(i);
            }
            return lower;
        }
    }

    MessageClassifier::MessageClassifier()
    {
        for (char const* c : kPlayerbotCommands)
            _commands.insert(c);
    }

    void MessageClassifier::AddCommands(std::vector<std::string> const& extraCommands)
    {
        for (auto const& c : extraCommands)
        {
            std::string t = ToLower(Trim(c));
            if (!t.empty())
                _commands.insert(t);
        }
    }

    bool MessageClassifier::LooksLikePlayerbotCommand(std::string const& lower) const
    {
        if (lower.empty())
            return false;
        if (lower[0] == '#')
            return true;
        // ".command" goes to Playerbots; "...I missed you" is roleplay.
        if (lower[0] == '.' && lower.size() > 1 && std::isalpha(static_cast<unsigned char>(lower[1])))
            return true;
        size_t words = WordCount(lower);
        // Longer sentences and questions are conversation, even if they start with a command word
        // ("follow the river with me?" / "give me a hug?"). Short forms like "nc ?" stay commands.
        if (words > 5 || (words > 3 && lower.find('?') != std::string::npos))
            return false;
        for (auto const& cmd : _commands)
            if (StartsWithWord(lower, cmd))
            {
                if (cmd.size() == 1 && words > 2)
                    continue; // one-letter shortcuts only as bare commands
                // "stay with me forever" is roleplay; a bare command, a short argument or a strategy edit is a command.
                bool strategyEdit = lower.find_first_of("+-=,") != std::string::npos;
                if (strategyEdit || (words <= 3 && (lower.find('?') == std::string::npos || words == 2)))
                    return true;
            }
        return false;
    }

    std::optional<ActionType> MessageClassifier::DetectAction(std::string const& l)
    {
        struct Pattern { char const* phrase; ActionType action; };
        static Pattern const patterns[] = {
            { "follow me", ActionType::Follow }, { "come with me", ActionType::Follow }, { "come here", ActionType::Follow },
            { "stay close", ActionType::Follow }, { "folge mir", ActionType::Follow }, { "folg mir", ActionType::Follow },
            { "komm mit", ActionType::Follow }, { "komm her", ActionType::Follow }, { "komm zu mir", ActionType::Follow },
            { "stay here", ActionType::Stay }, { "wait here", ActionType::Stay }, { "stay put", ActionType::Stay },
            { "wait for me", ActionType::Stay }, { "stay there", ActionType::Stay }, { "bleib hier", ActionType::Stay },
            { "warte hier", ActionType::Stay }, { "warte auf mich", ActionType::Stay }, { "bleib stehen", ActionType::Stay },
            { "attack", ActionType::Attack }, { "help me fight", ActionType::Attack }, { "help me kill", ActionType::Attack },
            { "kill it", ActionType::Attack }, { "get him", ActionType::Attack }, { "get them", ActionType::Attack },
            { "greif an", ActionType::Attack }, { "angriff", ActionType::Attack }, { "hilf mir kämpfen", ActionType::Attack },
            { "run away", ActionType::Flee }, { "retreat", ActionType::Flee }, { "fall back", ActionType::Flee },
            { "lauf weg", ActionType::Flee }, { "rückzug", ActionType::Flee }, { "flieh", ActionType::Flee },
            { "heal me", ActionType::Heal }, { "can you heal", ActionType::Heal }, { "need healing", ActionType::Heal },
            { "heil mich", ActionType::Heal }, { "heile mich", ActionType::Heal },
        };
        for (auto const& p : patterns)
            if (ContainsPhrase(l, p.phrase) && !NegatedBefore(l, p.phrase))
                return p.action;
        return std::nullopt;
    }

    ProposalIntent MessageClassifier::DetectProposal(std::string const& l)
    {
        static char const* const breakup[] = { "break up", "breaking up", "it's over between us", "we are over", "we're over",
                                               "schluss machen", "mache schluss", "mach schluss", "trennen uns", "es ist aus" };
        static char const* const engage[] = { "get engaged", "be my fiancee", "be my fiancée", "be my fiance",
                                              "verlobe dich", "verloben", "meine verlobte" };
        static char const* const marry[] = { "marry me", "get married", "be my wife", "be my husband", "heirate mich",
                                             "heiraten", "meine frau werden", "mein mann werden" };
        static char const* const dating[] = { "be my girlfriend", "be my boyfriend", "be my partner", "go out with me",
                                              "be my lover", "be together", "be a couple", "date me", "dating me",
                                              "meine freundin sein", "mein freund sein", "meine partnerin", "zusammen sein",
                                              "mit mir gehen", "ein paar sein" };
        // Whole words only: "update me" is not "date me", "be together forever" still is "be together".
        for (char const* p : breakup)
            if (ContainsPhrase(l, p) && !NegatedBefore(l, p))
                return ProposalIntent::Breakup;
        for (char const* p : engage)
            if (ContainsPhrase(l, p) && !NegatedBefore(l, p))
                return ProposalIntent::Engage;
        for (char const* p : marry)
            if (ContainsPhrase(l, p) && !NegatedBefore(l, p))
                return ProposalIntent::Marry;
        for (char const* p : dating)
            if (ContainsPhrase(l, p) && !NegatedBefore(l, p))
                return ProposalIntent::Dating;
        return ProposalIntent::None;
    }

    bool MessageClassifier::MentionsRelationshipStatus(std::string const& l)
    {
        static char const* const words[] = {
            "girlfriend", "boyfriend", "partner", "couple", "together", "date", "dating", "go out", "lover", "relationship",
            "marry", "married", "marriage", "wedding", "engaged", "engagement", "wife", "husband",
            "break up", "breaking up", "it's over", "we're over", "we are over",
            "freundin", "freund sein", "zusammen", "paar", "beziehung", "hochzeit", "ehe",
            "frau werden", "mann werden", "schluss", "trennen", "es ist aus" };
        // Stems: any word starting with them (propose/proposal, fiance/fiancée, girlfriends, heiraten, verloben/verlobt).
        static char const* const stems[] = { "propos", "fianc", "girlfriend", "boyfriend", "partner", "marri", "marry",
                                             "heirat", "verlob" };
        for (char const* w : words)
            if (ContainsPhrase(l, w))
                return true;
        for (char const* w : stems)
            if (ContainsStem(l, w))
                return true;
        return false;
    }

    ClassifiedMessage MessageClassifier::Classify(std::string const& rawText, std::string const& companionName, bool naturalCommands) const
    {
        ClassifiedMessage c;
        std::string text = Trim(rawText);
        if (text.empty())
            return c;

        // Explicit escape: "!follow" always goes to Playerbots.
        if (text[0] == '!')
        {
            c.route = MessageRoute::PlayerbotCommand;
            c.text = Trim(text.substr(1));
            return c;
        }

        std::string lower = ToLower(text);
        std::string withoutName = StripLeadingName(lower, ToLower(companionName));
        std::optional<ActionType> nlAction = DetectAction(withoutName);
        // "follow me" is a natural sentence; a bare "follow" stays a Playerbots command.
        bool naturalSentence = nlAction && WordCount(lower) >= 2;
        if (!(naturalCommands && naturalSentence) && LooksLikePlayerbotCommand(lower))
        {
            c.route = MessageRoute::PlayerbotCommand;
            c.text = text;
            return c;
        }

        c.route = MessageRoute::Roleplay;
        c.text = text;
        if (naturalCommands)
            c.action = nlAction;
        c.proposal = DetectProposal(withoutName);
        return c;
    }

    bool IsAddonAnnouncement(std::string const& text)
    {
        std::string l = ToLower(Trim(text));
        // raid target markers in front ("{rt1}", "{star}") are what quest addons put there
        bool marker = !l.empty() && l[0] == '{' && l.find('}') != std::string::npos && l.find('}') < 12;
        static char const* const addons[] = { "questie:", "questie :", "[questie]", "<questie>", "questhelper:", "[quest helper]",
                                              "<dbm>", "[dbm]", "<bigwigs>", "[bigwigs]", "<recount>", "[recount]", "<details!>",
                                              "<skada>", "[skada]" };
        for (char const* a : addons)
            if (l.find(a) != std::string::npos)
                return true;
        // "{rt1} 8/8 Lynx Collar for [Unfortunate Measures]" from other quest announcers: marker + count + quest link
        if (marker && l.find('/') != std::string::npos && l.find('[') != std::string::npos)
            return true;
        return false;
    }

    bool IsBareAcknowledgment(std::string const& text)
    {
        std::string l;
        for (char ch : ToLower(Trim(text)))
        {
            unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalpha(c) || c == ' ' || c >= 0x80)
                l.push_back(ch);
        }
        l = Trim(l);
        static char const* const words[] = { "ok", "okay", "k", "kk", "okey", "alright", "allright", "cool", "nice", "indeed", "lol", "haha",
                                             "hahaha", "hehe", "xd", "gg", "np", "mhm", "hm", "hmm", "mh", "ah", "oh", "ah ok", "oh ok",
                                             "ok cool", "ok nice", "nice one", "lmao", "rofl", "jo", "joa", "aha", "achso", "alles klar" };
        for (char const* w : words)
            if (l == w)
                return true;
        return false;
    }

    std::string DetectLanguage(std::string const& text)
    {
        std::string l = ToLower(text);
        int de = 0, en = 0;
        for (char const* u : { "\xc3\xa4", "\xc3\xb6", "\xc3\xbc", "\xc3\x9f" })
            if (l.find(u) != std::string::npos)
                de += 2;
        static char const* const deWords[] = { "ich", "du", "nicht", "und", "ist", "das", "wie", "bist", "mir", "dich",
                                               "ja", "nein", "was", "schön", "danke", "hallo", "gut", "wir", "auch", "mit", "sehr", "heute" };
        static char const* const enWords[] = { "the", "you", "and", "is", "are", "how", "what", "i'm", "it's", "my", "your",
                                               "thanks", "hello", "good", "we", "with", "very", "today", "do", "don't", "have" };
        std::string word;
        auto score = [&](std::string const& w)
        {
            for (char const* d : deWords) if (w == d) ++de;
            for (char const* e : enWords) if (w == e) ++en;
        };
        for (char ch : l)
        {
            unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalpha(c) || c == '\'' || c >= 0x80)
                word.push_back(ch);
            else
            {
                score(word);
                word.clear();
            }
        }
        score(word);
        if (de == en)
            return "";
        return de > en ? "de" : "en";
    }

    std::string SanitizeReply(std::string const& textIn, std::string const& companionName, size_t maxChars)
    {
        std::string t = textIn;
        // remove reasoning blocks some models emit even with think=false
        for (;;)
        {
            size_t a = t.find("<think>");
            if (a == std::string::npos)
                break;
            size_t b = t.find("</think>", a);
            t.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
        }
        // control chars and WoW escape sequences
        std::string clean;
        clean.reserve(t.size());
        for (char ch : t)
        {
            unsigned char c = static_cast<unsigned char>(ch);
            if (c == '|')
                continue;
            if (c < 0x20)
            {
                clean.push_back(' ');
                continue;
            }
            clean.push_back(ch);
        }
        if (!IsValidUtf8(clean))
        {
            std::string ascii;
            for (char ch : clean)
                if (static_cast<unsigned char>(ch) < 0x80)
                    ascii.push_back(ch);
            clean = ascii;
        }
        t = Trim(CollapseWhitespace(clean));

        // strip "Elara:" self prefix and wrapping quotes
        if (!companionName.empty())
        {
            std::string lower = ToLower(t), ln = ToLower(companionName);
            for (std::string pre : { ln + ":", ln + " :", "**" + ln + "**:", "[" + ln + "]:" })
                if (lower.compare(0, pre.size(), pre) == 0)
                {
                    t = Trim(t.substr(pre.size()));
                    break;
                }
        }
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"' && std::count(t.begin(), t.end(), '"') == 2)
            t = Trim(t.substr(1, t.size() - 2));

        if (t.size() > maxChars)
        {
            std::string cut = LimitUtf8(t, maxChars);
            size_t lastStop = cut.find_last_of(".!?~");
            if (lastStop != std::string::npos && lastStop > maxChars / 2)
                cut = cut.substr(0, lastStop + 1);
            else
            {
                size_t sp = cut.find_last_of(' ');
                if (sp != std::string::npos && sp > maxChars / 2)
                    cut = cut.substr(0, sp) + "...";
            }
            t = Trim(cut);
        }
        return t;
    }

    std::string SanitizePlayerText(std::string const& text, size_t maxChars)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x20)
            {
                out.push_back(' ');
                continue;
            }
            // Item links arrive as |cff...|Hitem:...|h[Name]|h|r; keep only the visible name.
            if (c == '|' && i + 1 == text.size())
                break;   // a lone trailing '|' would start a UI escape sequence
            if (c == '|')
            {
                char n = text[i + 1];
                if (n == 'c') { i += std::min<size_t>(9, text.size() - i - 1); continue; }   // |cAARRGGBB
                if (n == 'H')
                {
                    size_t end = text.find("|h", i + 2);
                    i = end == std::string::npos ? text.size() : end + 1;
                    continue;
                }
                ++i; // |h |r |T etc.
                continue;
            }
            if (c == '<' && i + 1 < text.size() && text[i + 1] == '<')
            {
                ++i; // "<<" marks server events in prompts; players may not forge them
                continue;
            }
            out.push_back(char(c));
        }
        if (!IsValidUtf8(out))
        {
            std::string ascii;
            for (char ch : out)
                if (static_cast<unsigned char>(ch) < 0x80)
                    ascii.push_back(ch);
            out = ascii;
        }
        return LimitUtf8(Trim(CollapseWhitespace(out)), maxChars);
    }

    std::vector<std::string> SplitForChat(std::string const& text, size_t maxBytes)
    {
        std::vector<std::string> parts;
        std::string rest = Trim(text);
        while (!rest.empty())
        {
            if (rest.size() <= maxBytes)
            {
                parts.push_back(rest);
                break;
            }
            std::string chunk = LimitUtf8(rest, maxBytes);
            size_t cut = chunk.size();
            size_t sentence = chunk.find_last_of(".!?");
            size_t space = chunk.find_last_of(' ');
            if (sentence != std::string::npos && sentence > maxBytes / 2)
                cut = sentence + 1;
            else if (space != std::string::npos && space > maxBytes / 3)
                cut = space;
            if (cut == 0)
            {
                // maxBytes smaller than one character: emit that character instead of looping forever
                cut = 1;
                while (cut < rest.size() && (static_cast<unsigned char>(rest[cut]) & 0xC0) == 0x80)
                    ++cut;
            }
            parts.push_back(Trim(rest.substr(0, cut)));
            rest = Trim(rest.substr(cut));
        }
        return parts;
    }

    bool IsRepetitive(std::string const& reply, std::vector<std::string> const& recentLines, double threshold)
    {
        auto t = Tokenize(reply);
        if (t.size() < 3)
        {
            std::string lr = ToLower(Trim(reply));
            for (auto const& line : recentLines)
                if (ToLower(Trim(line)) == lr)
                    return true;
            return false;
        }
        for (auto const& line : recentLines)
            if (TokenJaccard(t, Tokenize(line)) >= threshold)
                return true;
        return false;
    }

    size_t CountName(std::string const& text, std::string const& name)
    {
        if (name.empty())
            return 0;
        std::string l = ToLower(text), n = ToLower(name);
        size_t count = 0, pos = 0;
        while ((pos = l.find(n, pos)) != std::string::npos)
        {
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(l[pos - 1]));
            size_t end = pos + n.size();
            bool rightOk = end >= l.size() || !std::isalnum(static_cast<unsigned char>(l[end]));
            if (leftOk && rightOk)
                ++count;
            pos = end;
        }
        return count;
    }

    namespace
    {
        bool IsDirective(std::string const& lower)
        {
            static char const* const starts[] = { "tell them", "say it", "say so", "say that", "say a few", "answer them",
                                                  "ask them", "suggest", "let them know" };
            for (char const* st : starts)
                if (lower.compare(0, std::char_traits<char>::length(st), st) == 0)
                    return true;
            return false;
        }

        // the note split at ". ", "; " and ", " boundaries (outside quotes), keeping the separators
        std::vector<std::string> SplitClauses(std::string const& s)
        {
            std::vector<std::string> out;
            std::string cur;
            bool quoted = false;
            for (size_t i = 0; i < s.size(); ++i)
            {
                char c = s[i];
                cur += c;
                if (c == '"')
                    quoted = !quoted;
                if (!quoted && (c == '.' || c == ';' || c == ',' || c == '!' || c == '?') && (i + 1 == s.size() || s[i + 1] == ' '))
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty())
                out.push_back(cur);
            return out;
        }

        std::string SwapPersons(std::string const& text)
        {
            static std::set<std::string> const subjectAfter = {
                "and", "but", "so", "because", "that", "when", "if", "as", "while", "until", "then", "or", "where", "what",
                "which", "who", "how", "why", "once", "before", "after", "since", "unless", "than", "though", "although" };
            std::string out;
            bool quoted = false;
            bool clauseStart = true;      // the next word may be a subject
            bool subjectI = false;        // the clause is about her ("I ... and am ...")
            std::string prevLower;
            size_t i = 0;
            while (i < text.size())
            {
                char c = text[i];
                if (c == '"')
                    quoted = !quoted;
                if (quoted || !std::isalpha(static_cast<unsigned char>(c)))
                {
                    out += c;
                    if (!quoted && (c == ';' || c == ',' || c == ':' || c == '(' || c == '.'))
                        clauseStart = true;
                    if (!quoted && (c == '.' || c == ';'))
                        subjectI = false;
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < text.size() && (std::isalpha(static_cast<unsigned char>(text[j])) || text[j] == '\'' ))
                    ++j;
                std::string w = text.substr(i, j - i);
                std::string lw = ToLower(w);
                bool cap = std::isupper(static_cast<unsigned char>(w[0])) != 0;
                std::string next;
                for (size_t k = j; k < text.size() && text[k] == ' '; ++k)
                    if (k + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[k + 1])))
                    {
                        size_t e = k + 1;
                        while (e < text.size() && std::isalpha(static_cast<unsigned char>(text[e])))
                            ++e;
                        next = ToLower(text.substr(k + 1, e - k - 1));
                        break;
                    }
                bool subject = clauseStart || subjectAfter.count(prevLower) > 0;
                std::string r = w;
                if (lw == "you")
                {
                    if (next == "all")
                        r = subject ? "we" : "us";
                    else
                        r = subject ? "I" : "me";
                    if (subject && next != "all")
                        subjectI = true;
                }
                else if (lw == "you're") { r = "I'm"; subjectI = true; }
                else if (lw == "you've") { r = "I've"; subjectI = true; }
                else if (lw == "you'll") { r = "I'll"; subjectI = true; }
                else if (lw == "you'd") { r = "I'd"; subjectI = true; }
                else if (lw == "your") r = "my";
                else if (lw == "yours") r = "mine";
                else if (lw == "yourself") r = "myself";
                else if (lw == "they" || lw == "them") { r = "you"; if (subject) subjectI = false; }
                else if (lw == "their") r = "your";
                else if (lw == "theirs") r = "yours";
                else if (lw == "themselves") r = "yourself";
                else if (lw == "they're") { r = "you're"; subjectI = false; }
                else if (lw == "they've") { r = "you've"; subjectI = false; }
                else if (lw == "are" && (prevLower == "you" && out.size() >= 2 && out.compare(out.size() - 2, 2, "I ") == 0))
                    r = "am";
                else if (lw == "are" && subjectI && (prevLower == "and" || prevLower == "but" || prevLower == "still"))
                    r = "am";
                else if (lw == "were" && out.size() >= 2 && out.compare(out.size() - 2, 2, "I ") == 0)
                    r = "was";
                else if (lw == "aren't" && out.size() >= 2 && out.compare(out.size() - 2, 2, "I ") == 0)
                    r = "am not";
                bool pronounI = r == "I" || r.compare(0, 2, "I'") == 0;
                if (r != w && cap)
                    r[0] = char(std::toupper(static_cast<unsigned char>(r[0])));   // "They" -> "You"
                else if (r != w && !pronounI)
                    r[0] = char(std::tolower(static_cast<unsigned char>(r[0])));
                out += r;
                prevLower = lw;
                clauseStart = false;
                i = j;
            }
            return out;
        }
    }

    std::string PlainCompanionLine(std::string const& note)
    {
        std::string s = Trim(note);
        for (char const* lead : { "Nothing yet. ", "Nothing. " })
            if (s.compare(0, std::char_traits<char>::length(lead), lead) == 0)
                s = s.substr(std::char_traits<char>::length(lead));
        // drop the instructions to the AI ("Tell them.", "; tell them and ask which one.")
        std::string kept;
        for (std::string const& clause : SplitClauses(s))
        {
            std::string body = Trim(clause);
            if (IsDirective(ToLower(body)))
            {
                // "...; tell them." -> the clause before ends the sentence
                while (!kept.empty() && (kept.back() == ' ' || kept.back() == ';' || kept.back() == ','))
                    kept.pop_back();
                if (!kept.empty() && kept.back() != '.' && kept.back() != '!' && kept.back() != '?')
                    kept += '.';
                continue;
            }
            if (!kept.empty() && kept.back() != ' ')
                kept += ' ';
            kept += body;
        }
        kept = Trim(kept);
        while (!kept.empty() && (kept.back() == ';' || kept.back() == ','))
            kept.pop_back();
        if (kept.empty())
            return "";
        if (kept.back() != '.' && kept.back() != '!' && kept.back() != '?' && kept.back() != '"' && kept.back() != ')')
            kept += '.';
        std::string r = SwapPersons(kept);
        r[0] = char(std::toupper(static_cast<unsigned char>(r[0])));
        return r;
    }
}
