/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace truecompanion
{
    namespace
    {
        template <typename E, size_t N>
        std::optional<E> FromIdTable(std::string const& id, char const* const (&ids)[N])
        {
            for (size_t i = 0; i < N; ++i)
                if (id == ids[i])
                    return E(i);
            return std::nullopt;
        }

        char const* const kStageIds[kStageCount] = {
            "strangers", "acquaintances", "friends", "close_friends",
            "romantic_interest", "dating", "engaged", "married" };
        char const* const kStageLabels[kStageCount] = {
            "Strangers", "Acquaintances", "Friends", "Close Friends",
            "Romantic Interest", "Dating / Couple", "Engaged", "Married" };
        char const* const kMoodIds[kMoodCount] = {
            "idle", "relaxed", "happy", "excited", "curious", "bored", "lonely",
            "worried", "upset", "waiting_for_reply", "focused_on_combat", "enjoying_company" };
        char const* const kMemoryKindIds[] = {
            "fact", "preference", "episode", "relationship", "promise", "gift", "summary" };
        char const* const kMemorySourceIds[] = {
            "observed", "player_stated", "model_summary", "interpretation", "user_edited", "profile" };
        char const* const kGameEventIds[] = {
            "combat_start", "combat_end", "player_near_death", "player_died", "companion_died",
            "companion_level_up", "player_level_up", "quest_complete", "notable_loot", "zone_changed",
            "summoned", "dismissed", "player_returned", "equipment_changed", "companion_notice" };
        char const* const kActionIds[] = {
            "none", "follow", "stay", "attack", "flee", "heal",
            "emote_hug", "emote_kiss", "emote_wave", "emote_laugh", "emote_blush",
            "emote_dance", "emote_cheer", "emote_bow", "emote_cry", "leave" };

        int ClampInt(int64_t v, int lo, int hi) { return int(std::max<int64_t>(lo, std::min<int64_t>(hi, v))); }
        double Clamp100(double v) { return std::isfinite(v) ? std::max(0.0, std::min(100.0, v)) : 0.0; }
    }

    char const* StageId(Stage s) { return kStageIds[std::min<int>(int(s), kStageCount - 1)]; }
    char const* StageDefaultLabel(Stage s) { return kStageLabels[std::min<int>(int(s), kStageCount - 1)]; }
    std::optional<Stage> StageFromId(std::string const& id) { return FromIdTable<Stage>(id, kStageIds); }

    char const* MoodId(Mood m) { return kMoodIds[std::min<int>(int(m), kMoodCount - 1)]; }
    std::optional<Mood> MoodFromId(std::string const& id) { return FromIdTable<Mood>(id, kMoodIds); }

    char const* MemoryKindId(MemoryKind k) { return kMemoryKindIds[int(k)]; }
    std::optional<MemoryKind> MemoryKindFromId(std::string const& id) { return FromIdTable<MemoryKind>(id, kMemoryKindIds); }
    char const* MemorySourceId(MemorySource s) { return kMemorySourceIds[int(s)]; }
    std::optional<MemorySource> MemorySourceFromId(std::string const& id) { return FromIdTable<MemorySource>(id, kMemorySourceIds); }
    char const* GameEventId(GameEventType t) { return kGameEventIds[int(t)]; }

    char const* ActionId(ActionType a) { return kActionIds[int(a)]; }
    std::optional<ActionType> ActionFromId(std::string const& id) { return FromIdTable<ActionType>(id, kActionIds); }
    bool IsEmoteAction(ActionType a) { return a >= ActionType::EmoteHug && a <= ActionType::EmoteCry; }
    bool IsMovementCommand(ActionType a)
    {
        return a == ActionType::Follow || a == ActionType::Stay || a == ActionType::Attack || a == ActionType::Flee;
    }

    // ------------------------------------------------------------ RelationshipState

    Json RelationshipState::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("friendship", v.friendship);
        j.Set("affection", v.affection);
        j.Set("attraction", v.attraction);
        j.Set("trust", v.trust);
        j.Set("familiarity", v.familiarity);
        j.Set("comfort", v.comfort);
        j.Set("satisfaction", v.satisfaction);
        j.Set("stage", StageId(stage));
        j.Set("romantic", romantic);
        j.Set("mood", MoodId(mood));
        j.Set("moodIntensity", moodIntensity);
        j.Set("stageSince", int64_t(stageSince));
        j.Set("lastInteraction", int64_t(lastInteraction));
        j.Set("lastPlayerMessage", int64_t(lastPlayerMessage));
        j.Set("lastProposalAt", int64_t(lastProposalAt));
        j.Set("lastGiftRingAt", int64_t(lastGiftRingAt));
        j.Set("strained", strained);
        j.Set("resentment", resentment);
        j.Set("estranged", estranged);
        j.Set("estrangedAt", int64_t(estrangedAt));
        j.Set("estrangedUntil", int64_t(estrangedUntil));
        j.Set("reconciliation", reconciliation);
        j.Set("reconcileToday", reconcileToday);
        j.Set("timesLeft", int64_t(timesLeft));
        j.Set("dayWindowStart", int64_t(dayWindowStart));
        j.Set("dayPositiveGain", dayPositiveGain);
        j.Set("messageCount", int64_t(messageCount));
        return j;
    }

    RelationshipState RelationshipState::FromJson(Json const& j)
    {
        RelationshipState r;
        r.v.friendship = Clamp100(j.GetNumber("friendship", r.v.friendship));
        r.v.affection = Clamp100(j.GetNumber("affection", r.v.affection));
        r.v.attraction = Clamp100(j.GetNumber("attraction", r.v.attraction));
        r.v.trust = Clamp100(j.GetNumber("trust", r.v.trust));
        r.v.familiarity = Clamp100(j.GetNumber("familiarity", r.v.familiarity));
        r.v.comfort = Clamp100(j.GetNumber("comfort", r.v.comfort));
        r.v.satisfaction = Clamp100(j.GetNumber("satisfaction", r.v.satisfaction));
        r.stage = StageFromId(j.GetString("stage")).value_or(Stage::Strangers);
        r.romantic = j.GetBool("romantic", true);
        r.mood = MoodFromId(j.GetString("mood")).value_or(Mood::Relaxed);
        r.moodIntensity = Clamp100(j.GetNumber("moodIntensity", 30));
        r.stageSince = j.GetInt("stageSince", 0);
        r.lastInteraction = j.GetInt("lastInteraction", 0);
        r.lastPlayerMessage = j.GetInt("lastPlayerMessage", 0);
        r.lastProposalAt = j.GetInt("lastProposalAt", 0);
        r.lastGiftRingAt = j.GetInt("lastGiftRingAt", 0);
        r.strained = j.GetBool("strained", false);
        r.resentment = Clamp100(j.GetNumber("resentment", 0));
        r.estranged = j.GetBool("estranged", false);
        r.estrangedAt = j.GetInt("estrangedAt", 0);
        r.estrangedUntil = j.GetInt("estrangedUntil", 0);
        r.reconciliation = Clamp100(j.GetNumber("reconciliation", 0));
        r.reconcileToday = Clamp100(j.GetNumber("reconcileToday", 0));
        r.timesLeft = uint32_t(std::max<int64_t>(0, std::min<int64_t>(20, j.GetInt("timesLeft", 0))));
        r.dayWindowStart = j.GetInt("dayWindowStart", 0);
        r.dayPositiveGain = std::max(0.0, j.GetNumber("dayPositiveGain", 0));
        r.messageCount = uint32_t(std::max<int64_t>(0, j.GetInt("messageCount", 0)));
        return r;
    }

    // ------------------------------------------------------------ Personality

    std::vector<std::string> const& PersonalityProfile::KnownStyles()
    {
        static std::vector<std::string> const styles = {
            "anime", "mature", "teasing", "tsundere", "dandere", "sweet", "fantasy_roleplay", "casual", "poetic" };
        return styles;
    }

    namespace
    {
        char const* LengthId(ResponseLength l)
        {
            switch (l)
            {
                case ResponseLength::Short: return "short";
                case ResponseLength::Medium: return "medium";
                case ResponseLength::Long: return "long";
                default: return "varied";
            }
        }
        ResponseLength LengthFromId(std::string const& s)
        {
            if (s == "short") return ResponseLength::Short;
            if (s == "medium") return ResponseLength::Medium;
            if (s == "long") return ResponseLength::Long;
            return ResponseLength::Varied;
        }
    }

    Json PersonalityProfile::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("name", name);
        j.Set("nickname", nickname);
        j.Set("addressPlayerAs", addressPlayerAs);
        j.Set("age", age);
        j.Set("gender", gender);
        j.Set("pronouns", pronouns);
        j.Set("relationshipPreferences", relationshipPreferences);
        j.Set("background", background);
        j.Set("history", history);
        j.Set("interests", interests);
        j.Set("likes", likes);
        j.Set("dislikes", dislikes);
        j.Set("goals", goals);
        j.Set("fears", fears);
        j.Set("lore", lore);
        Json t = Json::MakeObject();
        t.Set("outgoing", outgoing);
        t.Set("sarcasm", sarcasm);
        t.Set("playfulness", playfulness);
        t.Set("energy", energy);
        t.Set("confidence", confidence);
        t.Set("romanticism", romanticism);
        t.Set("jealousy", jealousy);
        t.Set("humor", humor);
        t.Set("expressiveness", expressiveness);
        t.Set("affectionLevel", affectionLevel);
        t.Set("initiative", initiative);
        t.Set("loyalty", loyalty);
        j.Set("traits", t);
        j.Set("length", LengthId(length));
        j.Set("formal", formal);
        Json st = Json::MakeArray();
        for (auto const& s : styles)
            st.Push(s);
        j.Set("styles", st);
        j.Set("customInstructions", customInstructions);
        j.Set("language", language);
        j.Set("preset", preset);
        j.Set("romanceEnabled", romanceEnabled);
        j.Set("autoAcceptProposals", autoAcceptProposals);
        Json labels = Json::MakeArray();
        for (auto const& l : stageLabels)
            labels.Push(l);
        j.Set("stageLabels", labels);
        return j;
    }

    PersonalityProfile PersonalityProfile::FromJson(Json const& j)
    {
        PersonalityProfile p;
        p.name = j.GetString("name");
        p.nickname = j.GetString("nickname");
        p.addressPlayerAs = j.GetString("addressPlayerAs");
        p.age = int(j.GetInt("age", 21));
        p.gender = j.GetString("gender");
        p.pronouns = j.GetString("pronouns");
        p.relationshipPreferences = j.GetString("relationshipPreferences");
        p.background = j.GetString("background");
        p.history = j.GetString("history");
        p.interests = j.GetString("interests");
        p.likes = j.GetString("likes");
        p.dislikes = j.GetString("dislikes");
        p.goals = j.GetString("goals");
        p.fears = j.GetString("fears");
        p.lore = j.GetString("lore");
        Json const& t = j["traits"];
        p.outgoing = int(t.GetInt("outgoing", p.outgoing));
        p.sarcasm = int(t.GetInt("sarcasm", p.sarcasm));
        p.playfulness = int(t.GetInt("playfulness", p.playfulness));
        p.energy = int(t.GetInt("energy", p.energy));
        p.confidence = int(t.GetInt("confidence", p.confidence));
        p.romanticism = int(t.GetInt("romanticism", p.romanticism));
        p.jealousy = int(t.GetInt("jealousy", p.jealousy));
        p.humor = int(t.GetInt("humor", p.humor));
        p.expressiveness = int(t.GetInt("expressiveness", p.expressiveness));
        p.affectionLevel = int(t.GetInt("affectionLevel", p.affectionLevel));
        p.initiative = int(t.GetInt("initiative", p.initiative));
        p.loyalty = int(t.GetInt("loyalty", p.loyalty));
        p.length = LengthFromId(j.GetString("length", "varied"));
        p.formal = j.GetBool("formal", false);
        for (Json const& s : j["styles"].AsArray())
            if (s.IsString())
                p.styles.push_back(s.AsString());
        p.customInstructions = j.GetString("customInstructions");
        p.language = j.GetString("language", "auto");
        p.preset = j.GetString("preset");
        p.romanceEnabled = j.GetBool("romanceEnabled", true);
        p.autoAcceptProposals = j.GetBool("autoAcceptProposals", false);
        auto const& labels = j["stageLabels"].AsArray();
        for (size_t i = 0; i < labels.size() && i < size_t(kStageCount); ++i)
            p.stageLabels[i] = labels[i].AsString("");
        return p;
    }

    std::vector<std::string> PersonalityProfile::Sanitize(size_t maxFieldLen, size_t maxInstructionLen)
    {
        std::vector<std::string> notes;
        auto clean = [&](std::string& s, size_t lim)
        {
            std::string before = s;
            s = LimitUtf8(Trim(s), lim);
            // '|' starts WoW UI escape sequences; never allow it in stored text.
            s.erase(std::remove(s.begin(), s.end(), '|'), s.end());
            if (s.size() < Trim(before).size())
                notes.push_back("text trimmed");
        };
        for (std::string* f : { &name, &nickname, &addressPlayerAs, &gender, &pronouns, &preset })
            clean(*f, 64);
        for (std::string* f : { &relationshipPreferences, &background, &history, &interests, &likes,
                                &dislikes, &goals, &fears, &lore })
            clean(*f, maxFieldLen);
        clean(customInstructions, maxInstructionLen);
        for (auto& l : stageLabels)
            clean(l, 32);

        if (age < 18)
        {
            notes.push_back("age raised to 18: companions must be adults");
            age = 18;
        }
        if (age > 1000)
            age = 1000; // elves and dragons may be old, but keep it sane

        for (int* t : { &outgoing, &sarcasm, &playfulness, &energy, &confidence, &romanticism, &jealousy,
                        &humor, &expressiveness, &affectionLevel, &initiative, &loyalty })
            *t = ClampInt(*t, 0, 100);

        auto const& known = KnownStyles();
        std::vector<std::string> kept;
        for (auto const& s : styles)
            if (std::find(known.begin(), known.end(), s) != known.end() &&
                std::find(kept.begin(), kept.end(), s) == kept.end())
                kept.push_back(s);
        if (kept.size() != styles.size())
            notes.push_back("unknown styles removed");
        styles = kept;

        if (language != "auto" && language != "en" && language != "de")
        {
            language = "auto";
            notes.push_back("language reset to auto");
        }
        return notes;
    }

    // ------------------------------------------------------------ PlayerProfile

    Json PlayerProfile::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("preferredName", preferredName);
        j.Set("nickname", nickname);
        j.Set("age", age);
        j.Set("pronouns", pronouns);
        j.Set("rpIdentity", rpIdentity);
        j.Set("background", background);
        j.Set("interests", interests);
        j.Set("preferences", preferences);
        j.Set("expectations", expectations);
        j.Set("sharedHistory", sharedHistory);
        j.Set("customFacts", customFacts);
        return j;
    }

    PlayerProfile PlayerProfile::FromJson(Json const& j)
    {
        PlayerProfile p;
        p.preferredName = j.GetString("preferredName");
        p.nickname = j.GetString("nickname");
        p.age = int(j.GetInt("age", 0));
        p.pronouns = j.GetString("pronouns");
        p.rpIdentity = j.GetString("rpIdentity");
        p.background = j.GetString("background");
        p.interests = j.GetString("interests");
        p.preferences = j.GetString("preferences");
        p.expectations = j.GetString("expectations");
        p.sharedHistory = j.GetString("sharedHistory");
        p.customFacts = j.GetString("customFacts");
        return p;
    }

    void PlayerProfile::Sanitize(size_t maxFieldLen)
    {
        auto clean = [&](std::string& s, size_t lim)
        {
            s = LimitUtf8(Trim(s), lim);
            s.erase(std::remove(s.begin(), s.end(), '|'), s.end());
        };
        clean(preferredName, 64);
        clean(nickname, 64);
        clean(pronouns, 32);
        for (std::string* f : { &rpIdentity, &background, &interests, &preferences, &expectations, &sharedHistory, &customFacts })
            clean(*f, maxFieldLen);
        age = ClampInt(age, 0, 1000);
    }

    bool PlayerProfile::Empty() const
    {
        return preferredName.empty() && nickname.empty() && rpIdentity.empty() && background.empty() &&
               interests.empty() && preferences.empty() && expectations.empty() && sharedHistory.empty() &&
               customFacts.empty();
    }

    // ------------------------------------------------------------ CompanionSettings

    Json CompanionSettings::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("aiEnabled", aiEnabled);
        j.Set("proactiveEnabled", proactiveEnabled);
        j.Set("proactiveFrequency", proactiveFrequency);
        j.Set("naturalCommands", naturalCommands);
        j.Set("partyChatMode", partyChatMode);
        j.Set("listenInParty", listenInParty);
        j.Set("model", model);
        j.Set("temperature", temperature);
        j.Set("maxReplyChars", maxReplyChars);
        j.Set("waitForPlayerLoot", waitForPlayerLoot);
        return j;
    }

    CompanionSettings CompanionSettings::FromJson(Json const& j)
    {
        CompanionSettings s;
        s.aiEnabled = j.GetBool("aiEnabled", true);
        s.proactiveEnabled = j.GetBool("proactiveEnabled", true);
        s.proactiveFrequency = ClampInt(j.GetInt("proactiveFrequency", 50), 0, 100);
        s.naturalCommands = j.GetBool("naturalCommands", true);
        s.partyChatMode = j.GetBool("partyChatMode", false);
        s.listenInParty = j.GetBool("listenInParty", true);
        s.model = LimitUtf8(j.GetString("model"), 96);
        s.temperature = j.GetNumber("temperature", -1);
        if (!std::isfinite(s.temperature) || s.temperature > 2.0)
            s.temperature = -1;
        s.maxReplyChars = ClampInt(j.GetInt("maxReplyChars", 0), 0, 2000);
        s.waitForPlayerLoot = j.GetBool("waitForPlayerLoot", false);
        return s;
    }

    // ------------------------------------------------------------ Memory

    Json MemoryEntry::ToJson() const
    {
        Json j = Json::MakeObject();
        j.Set("id", int64_t(id));
        j.Set("kind", MemoryKindId(kind));
        j.Set("source", MemorySourceId(source));
        j.Set("importance", importance);
        j.Set("text", text);
        j.Set("createdAt", int64_t(createdAt));
        j.Set("updatedAt", int64_t(updatedAt));
        j.Set("lastRecalledAt", int64_t(lastRecalledAt));
        j.Set("recallCount", int64_t(recallCount));
        j.Set("pinned", pinned);
        return j;
    }

    std::optional<MemoryEntry> MemoryEntry::FromJson(Json const& j)
    {
        if (!j.IsObject())
            return std::nullopt;
        MemoryEntry m;
        m.id = uint32_t(std::max<int64_t>(0, j.GetInt("id", 0)));
        auto kind = MemoryKindFromId(j.GetString("kind"));
        auto source = MemorySourceFromId(j.GetString("source"));
        if (!kind || !source)
            return std::nullopt;
        m.kind = *kind;
        m.source = *source;
        m.importance = ClampInt(j.GetInt("importance", 50), 0, 100);
        m.text = LimitUtf8(Trim(j.GetString("text")), 400);
        m.text.erase(std::remove(m.text.begin(), m.text.end(), '|'), m.text.end());
        if (m.text.empty())
            return std::nullopt;
        m.createdAt = j.GetInt("createdAt", 0);
        m.updatedAt = j.GetInt("updatedAt", m.createdAt);
        m.lastRecalledAt = j.GetInt("lastRecalledAt", 0);
        m.recallCount = uint32_t(std::max<int64_t>(0, j.GetInt("recallCount", 0)));
        m.pinned = j.GetBool("pinned", false);
        return m;
    }

    // ------------------------------------------------------------ WorldContext

    bool WorldContext::BusyForChat(bool blockInInstances) const
    {
        if (playerInCombat || companionInCombat || playerAfk || !companionInWorld)
            return true;
        if (inBattleground)
            return true;
        if (blockInInstances && (inDungeon || inRaid))
            return true;
        return false;
    }

    // ------------------------------------------------------------ string helpers

    std::string Trim(std::string const& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return s.substr(b, e - b);
    }

    std::string ToLower(std::string s)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
                s[i] = char(std::tolower(c));
            else if (c == 0xC3 && i + 1 < s.size())
            {
                // Ä Ö Ü (C3 84 / C3 96 / C3 9C) -> ä ö ü (C3 A4 / C3 B6 / C3 BC)
                unsigned char d = static_cast<unsigned char>(s[i + 1]);
                if (d == 0x84 || d == 0x96 || d == 0x9C)
                    s[i + 1] = char(d + 0x20);
                ++i;
            }
        }
        return s;
    }

    bool IsValidUtf8(std::string const& s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (n == 0 || i + n > s.size())
                return false;
            for (size_t k = 1; k < n; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                    return false;
            i += n;
        }
        return true;
    }

    std::string LimitUtf8(std::string const& s, size_t maxBytes)
    {
        if (s.size() <= maxBytes)
            return s;
        size_t cut = maxBytes;
        // step back over continuation bytes so we never split a code point
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        return s.substr(0, cut);
    }

    std::string CollapseWhitespace(std::string const& s)
    {
        std::string out;
        out.reserve(s.size());
        bool space = false;
        for (char c : s)
        {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = true;
                continue;
            }
            if (space && !out.empty())
                out.push_back(' ');
            space = false;
            out.push_back(c);
        }
        return out;
    }
}
