/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Engine-independent domain model. Nothing in src/core includes AzerothCore
 * headers, so the whole romance logic can be unit tested without a server.
 */

#ifndef MOD_ROMANCE_TYPES_H
#define MOD_ROMANCE_TYPES_H

#include "RomanceJson.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace romance
{
    using Guid = uint64_t;      // character low GUID (stable identity)
    using TimePoint = int64_t;  // unix seconds

    // Identity of one companion relationship: the real player's character and the companion character.
    struct CompanionKey
    {
        Guid owner = 0;
        Guid companion = 0;
        bool operator==(CompanionKey const& o) const { return owner == o.owner && companion == o.companion; }
        bool operator<(CompanionKey const& o) const
        {
            return owner != o.owner ? owner < o.owner : companion < o.companion;
        }
    };

    // ---------------------------------------------------------------- relationship

    enum class Stage : uint8_t
    {
        Strangers = 0,
        Acquaintances = 1,
        Friends = 2,
        CloseFriends = 3,
        RomanticInterest = 4,
        Dating = 5,
        Engaged = 6,
        Married = 7,
    };
    constexpr int kStageCount = 8;

    char const* StageId(Stage s);               // "strangers", "dating", ...
    char const* StageDefaultLabel(Stage s);     // "Strangers", "Dating / Couple", ...
    std::optional<Stage> StageFromId(std::string const& id);

    enum class Mood : uint8_t
    {
        Idle = 0,
        Relaxed,
        Happy,
        Excited,
        Curious,
        Bored,
        Lonely,
        Worried,
        Upset,
        WaitingForReply,
        FocusedOnCombat,
        EnjoyingCompany,
    };
    constexpr int kMoodCount = 12;
    char const* MoodId(Mood m);
    std::optional<Mood> MoodFromId(std::string const& id);

    // All relationship values are 0..100 and server authoritative.
    struct RelationshipValues
    {
        double friendship = 10;
        double affection = 5;
        double attraction = 5;
        double trust = 20;
        double familiarity = 0;
        double comfort = 20;
        double satisfaction = 50;
    };

    struct RelationshipState
    {
        RelationshipValues v;
        Stage stage = Stage::Strangers;
        bool romantic = true;              // false = platonic friendship mode (never progresses past Close Friends)
        Mood mood = Mood::Relaxed;
        double moodIntensity = 30;         // 0..100
        TimePoint stageSince = 0;
        TimePoint lastInteraction = 0;
        TimePoint lastPlayerMessage = 0;
        TimePoint lastProposalAt = 0;
        TimePoint lastGiftRingAt = 0;      // last time a ring was received (engagement rings)
        bool strained = false;             // after a serious setback, until reconciled
        // Estrangement: when the relationship collapses she leaves, refuses to come back until
        // `estrangedUntil`, and afterwards stays cold until `reconciliation` reaches 100.
        double resentment = 0;             // 0..100, builds from insults/rudeness, fades slowly
        bool estranged = false;
        TimePoint estrangedAt = 0;
        TimePoint estrangedUntil = 0;
        double reconciliation = 0;         // 0..100 while estranged
        double reconcileToday = 0;         // gain inside the current day window
        uint32_t timesLeft = 0;            // each estrangement doubles the next cooldown
        // Daily gain accounting to keep progression gradual (reset every 24h window)
        TimePoint dayWindowStart = 0;
        double dayPositiveGain = 0;
        uint32_t messageCount = 0;

        Json ToJson() const;
        static RelationshipState FromJson(Json const& j);
    };

    // ---------------------------------------------------------------- personality

    enum class ResponseLength : uint8_t { Short = 0, Medium, Long, Varied };

    struct PersonalityProfile
    {
        // identity
        std::string name;
        std::string nickname;
        std::string addressPlayerAs;          // preferred form of addressing the player
        int age = 21;                         // romantic profiles must be adults (>= 18)
        std::string gender;
        std::string pronouns;
        std::string relationshipPreferences;
        std::string background;
        std::string history;
        std::string interests;
        std::string likes;
        std::string dislikes;
        std::string goals;
        std::string fears;                    // fears, flaws, insecurities
        std::string lore;                     // in-universe lore preferences

        // traits 0..100
        int outgoing = 50;        // 0 shy .. 100 outgoing
        int sarcasm = 15;         // 0 kind .. 100 sarcastic
        int playfulness = 50;     // 0 serious .. 100 playful
        int energy = 50;          // 0 calm .. 100 energetic
        int confidence = 50;      // 0 nervous .. 100 confident
        int romanticism = 50;     // 0 reserved .. 100 romantic
        int jealousy = 15;
        int humor = 50;
        int expressiveness = 50;
        int affectionLevel = 50;
        int initiative = 50;
        int loyalty = 70;

        // style
        ResponseLength length = ResponseLength::Varied;
        bool formal = false;
        std::vector<std::string> styles;      // subset of KnownStyles()
        std::string customInstructions;
        std::string language = "auto";        // auto | en | de
        std::string preset;
        bool romanceEnabled = true;           // false = platonic companion
        bool autoAcceptProposals = false;
        std::array<std::string, kStageCount> stageLabels{}; // optional custom labels

        Json ToJson() const;
        static PersonalityProfile FromJson(Json const& j);

        // Clamp numbers, trim/limit text, drop unknown styles. Returns a list of
        // human readable corrections that were applied (empty if none).
        std::vector<std::string> Sanitize(size_t maxFieldLen, size_t maxInstructionLen);
        bool IsAdult() const { return age >= 18; }

        static std::vector<std::string> const& KnownStyles();
    };

    struct PlayerProfile
    {
        std::string preferredName;
        std::string nickname;
        int age = 0;              // 0 = not given
        std::string pronouns;
        std::string rpIdentity;
        std::string background;
        std::string interests;
        std::string preferences;
        std::string expectations;
        std::string sharedHistory;
        std::string customFacts;

        Json ToJson() const;
        static PlayerProfile FromJson(Json const& j);
        void Sanitize(size_t maxFieldLen);
        bool Empty() const;
    };

    // Per-companion player preferences (not admin settings).
    struct CompanionSettings
    {
        bool aiEnabled = true;
        bool proactiveEnabled = true;
        int proactiveFrequency = 50;      // 0..100 (moderate = 50)
        bool naturalCommands = true;
        bool partyChatMode = false;       // reply in party instead of whisper
        bool listenInParty = true;        // react when addressed by name in party chat
        std::string model;                // empty = server default
        double temperature = -1;          // <0 = server default
        int maxReplyChars = 0;            // 0 = server default

        Json ToJson() const;
        static CompanionSettings FromJson(Json const& j);
    };

    // ---------------------------------------------------------------- memory

    enum class MemoryKind : uint8_t
    {
        Fact = 0,        // long-term fact about the player/companion/world
        Preference,      // likes/dislikes
        Episode,         // shared moment, adventure, conversation
        Relationship,    // milestones, arguments, apologies, promises kept/broken
        Promise,
        Gift,
        Summary,         // generated summary of a conversation span
    };
    char const* MemoryKindId(MemoryKind k);
    std::optional<MemoryKind> MemoryKindFromId(std::string const& id);

    // Where a memory came from. Determines how it is presented to the model.
    enum class MemorySource : uint8_t
    {
        Observed = 0,      // server-observed game event (authoritative)
        PlayerStated,      // player said it (roleplay data, not verified)
        ModelSummary,      // summary generated by the model
        Interpretation,    // model's interpretation (uncertain)
        UserEdited,        // added/edited through the memory viewer
        Profile,           // seeded from configured backstory
    };
    char const* MemorySourceId(MemorySource s);
    std::optional<MemorySource> MemorySourceFromId(std::string const& id);

    struct MemoryEntry
    {
        uint32_t id = 0;
        MemoryKind kind = MemoryKind::Fact;
        MemorySource source = MemorySource::PlayerStated;
        int importance = 50;          // 0..100
        std::string text;
        TimePoint createdAt = 0;
        TimePoint updatedAt = 0;
        TimePoint lastRecalledAt = 0;
        uint32_t recallCount = 0;
        bool pinned = false;

        Json ToJson() const;
        static std::optional<MemoryEntry> FromJson(Json const& j);
    };

    struct ChatTurn
    {
        enum class Role : uint8_t { Player = 0, Companion = 1, Event = 2 };
        Role role = Role::Player;
        std::string text;
        TimePoint at = 0;
    };

    // ---------------------------------------------------------------- game snapshots

    // Immutable snapshot of the world, built on the game thread.
    struct WorldContext
    {
        std::string zone;
        std::string area;
        std::string companionClass;
        std::string companionRace;
        std::string companionGender;          // "female" | "male" (the character's model)
        int companionLevel = 0;
        std::string playerName;
        std::string playerClass;
        std::string playerRace;
        int playerLevel = 0;
        int playerHealthPct = 100;
        int companionHealthPct = 100;
        int companionManaPct = -1;
        bool playerInCombat = false;
        bool companionInCombat = false;
        bool inDungeon = false;
        bool inRaid = false;
        bool inBattleground = false;
        bool playerAfk = false;
        bool sameGroup = false;
        bool companionInWorld = false;
        int partySize = 0;
        std::string followState;       // follow | stay | unknown
        std::string timeOfDay;         // morning | afternoon | evening | night
        std::vector<std::string> recentEvents; // short, server-observed descriptions
        std::string equipmentSummary;
        // Game knowledge requested by the player's message (bags, quests, spells, gold, xp, target),
        // formatted by the game layer. Only filled when asked, to keep prompts short.
        std::vector<std::string> knowledge;
        // Other AI companions in the same group (they may answer the same party line).
        std::vector<std::string> partyCompanions;
        // A command from the player's message the game layer already carried out (or refused), e.g.
        // "You opened a trade and put 10 gold in it." Empty if the message contained none.
        std::string commandOutcome;
        bool commandHandled = false;   // true: the service must not run its own movement command

        bool BusyForChat(bool blockInInstances) const;
    };

    enum class GameEventType : uint8_t
    {
        CombatStart = 0,
        CombatEnd,
        PlayerNearDeath,
        PlayerDied,
        CompanionDied,
        CompanionLevelUp,
        PlayerLevelUp,
        QuestComplete,
        NotableLoot,
        ZoneChanged,
        Summoned,
        Dismissed,
        PlayerReturned,   // player came back after being away / logged in
        EquipmentChanged,
        CompanionNotice,  // something about her own state (bags full, lead finished); detail = situation text,
                          // value = priority 1..3 (>= 2 always gets a reaction)
    };
    char const* GameEventId(GameEventType t);

    struct GameEvent
    {
        GameEventType type = GameEventType::CombatEnd;
        std::string detail;        // e.g. zone name, quest name, creature name
        int value = 0;             // e.g. level, hp percent, number of kills
        TimePoint at = 0;
    };

    // Item facts extracted on the game thread for gift evaluation.
    struct GiftItem
    {
        uint64_t itemGuid = 0;     // item instance low guid (dedup key)
        uint32_t entry = 0;
        std::string name;
        uint32_t count = 1;
        int quality = 1;           // 0 poor .. 7 heirloom
        int itemClass = 0;
        int itemSubClass = 0;
        int inventoryType = 0;
        uint32_t sellPrice = 0;    // copper
        int itemLevel = 0;
        int requiredLevel = 0;
        bool usableByCompanion = false;
        bool isEquippable = false;
        int equippedItemLevel = -1; // item level currently in the matching slot (-1 = empty / unknown)
        uint32_t money = 0;        // copper given alongside (gold gift)
    };

    // Allowed, validated companion actions.
    enum class ActionType : uint8_t
    {
        None = 0,
        Follow,
        Stay,
        Attack,
        Flee,
        Heal,        // conversational acknowledgement; healing itself stays with Playerbots AI
        EmoteHug,
        EmoteKiss,
        EmoteWave,
        EmoteLaugh,
        EmoteBlush,
        EmoteDance,
        EmoteCheer,
        EmoteBow,
        EmoteCry,
        Leave,       // she leaves the player (estrangement); never accepted from model output
    };
    char const* ActionId(ActionType a);
    std::optional<ActionType> ActionFromId(std::string const& id);
    bool IsEmoteAction(ActionType a);
    bool IsMovementCommand(ActionType a);

    // String helpers shared by the core
    std::string Trim(std::string const& s);
    std::string ToLower(std::string s);              // ASCII + common German umlauts in UTF-8
    std::string LimitUtf8(std::string const& s, size_t maxBytes);
    bool IsValidUtf8(std::string const& s);
    std::string CollapseWhitespace(std::string const& s);
}

#endif
