#include "game/quest_handler.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/packet_parsers.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace wowee {
namespace game {

QuestGiverStatus QuestHandler::getQuestGiverStatus(uint64_t guid) const {
    auto it = npcQuestStatus_.find(guid);
    return (it != npcQuestStatus_.end()) ? it->second : QuestGiverStatus::NONE;
}


static std::string formatCopperAmount(uint32_t amount) {
    uint32_t gold = amount / 10000;
    uint32_t silver = (amount / 100) % 100;
    uint32_t copper = amount % 100;

    std::ostringstream oss;
    bool wrote = false;
    if (gold > 0) {
        oss << gold << "g";
        wrote = true;
    }
    if (silver > 0) {
        if (wrote) oss << " ";
        oss << silver << "s";
        wrote = true;
    }
    if (copper > 0 || !wrote) {
        if (wrote) oss << " ";
        oss << copper << "c";
    }
    return oss.str();
}

static bool isReadableQuestText(const std::string& s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    bool hasAlpha = false;
    for (unsigned char c : s) {
        // Reject control characters but allow UTF-8 multi-byte sequences (0x80+)
        // so localized servers (French, German, Russian, etc.) work correctly.
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
        if (c >= 0x20 && c <= 0x7E && std::isalpha(c)) hasAlpha = true;
        // UTF-8 continuation/lead bytes (0x80+) are allowed but don't count as alpha
        // since we only need at least one ASCII letter to distinguish from binary garbage.
    }
    return hasAlpha;
}

static bool isPlaceholderQuestTitle(const std::string& s) {
    return s.rfind("Quest #", 0) == 0;
}

static bool looksLikeQuestDescriptionText(const std::string& s) {
    int spaces = 0;
    int commas = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if (c == ',') commas++;
    }
    const int words = spaces + 1;
    if (words > 8) return true;
    if (commas > 0 && words > 5) return true;
    if (s.find(". ") != std::string::npos) return true;
    if (s.find(':') != std::string::npos && words > 5) return true;
    return false;
}

static bool isStrongQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 6, 72)) return false;
    if (looksLikeQuestDescriptionText(s)) return false;
    unsigned char first = static_cast<unsigned char>(s.front());
    return std::isupper(first) != 0;
}

static int scoreQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 4, 72)) return -1000;
    if (looksLikeQuestDescriptionText(s)) return -1000;
    int score = 0;
    score += static_cast<int>(std::min<size_t>(s.size(), 32));
    unsigned char first = static_cast<unsigned char>(s.front());
    if (std::isupper(first)) score += 20;
    if (std::islower(first)) score -= 20;
    if (s.find(' ') != std::string::npos) score += 8;
    if (s.find('.') != std::string::npos) score -= 18;
    if (s.find('!') != std::string::npos || s.find('?') != std::string::npos) score -= 6;
    return score;
}

static bool readCStringAt(const std::vector<uint8_t>& data, size_t start, std::string& out, size_t& nextPos) {
    out.clear();
    if (start >= data.size()) return false;
    size_t i = start;
    while (i < data.size()) {
        uint8_t b = data[i++];
        if (b == 0) {
            nextPos = i;
            return true;
        }
        out.push_back(static_cast<char>(b));
    }
    return false;
}

struct QuestQueryTextCandidate {
    std::string title;
    std::string objectives;
    int score = -1000;
};

static QuestQueryTextCandidate pickBestQuestQueryTexts(const std::vector<uint8_t>& data, bool classicHint) {
    QuestQueryTextCandidate best;
    if (data.size() <= 9) return best;

    std::vector<size_t> seedOffsets;
    const size_t base = 8;
    const size_t classicOffset = base + 40u * 4u;
    const size_t wotlkOffset = base + 55u * 4u;
    if (classicHint) {
        seedOffsets.push_back(classicOffset);
        seedOffsets.push_back(wotlkOffset);
    } else {
        seedOffsets.push_back(wotlkOffset);
        seedOffsets.push_back(classicOffset);
    }
    for (size_t off : seedOffsets) {
        if (off < data.size()) {
            std::string title;
            size_t next = off;
            if (readCStringAt(data, off, title, next)) {
                QuestQueryTextCandidate c;
                c.title = title;
                c.score = scoreQuestTitle(title) + 20; // Prefer expected struct offsets

                std::string s2;
                size_t n2 = next;
                if (readCStringAt(data, next, s2, n2) && isReadableQuestText(s2, 8, 600)) {
                    c.objectives = s2;
                }
                if (c.score > best.score) best = c;
            }
        }
    }

    // Fallback: scan packet for best printable C-string title candidate.
    for (size_t start = 8; start < data.size(); ++start) {
        std::string title;
        size_t next = start;
        if (!readCStringAt(data, start, title, next)) continue;

        QuestQueryTextCandidate c;
        c.title = title;
        c.score = scoreQuestTitle(title);
        if (c.score < 0) continue;

        std::string s2, s3;
        size_t n2 = next, n3 = next;
        if (readCStringAt(data, next, s2, n2)) {
            if (isReadableQuestText(s2, 8, 600)) c.objectives = s2;
            else if (readCStringAt(data, n2, s3, n3) && isReadableQuestText(s3, 8, 600)) c.objectives = s3;
        }
        if (c.score > best.score) best = c;
    }

    return best;
}

struct QuestQueryObjectives {
    struct Kill { int32_t npcOrGoId; uint32_t required; };
    struct Item { uint32_t itemId; uint32_t required; };
    std::array<Kill, 4> kills{};
    std::array<Item, 6> items{};
    bool valid = false;
};

static uint32_t readU32At(const std::vector<uint8_t>& d, size_t pos) {
    return static_cast<uint32_t>(d[pos])
         | (static_cast<uint32_t>(d[pos + 1]) << 8)
         | (static_cast<uint32_t>(d[pos + 2]) << 16)
         | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

static QuestQueryObjectives tryParseQuestObjectivesAt(const std::vector<uint8_t>& data,
                                                       size_t startPos, int nStrings) {
    QuestQueryObjectives out;
    size_t pos = startPos;

    for (int si = 0; si < nStrings; ++si) {
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return out;
        ++pos;
    }

    for (int i = 0; i < 4; ++i) {
        if (pos + 8 > data.size()) return out;
        out.kills[i].npcOrGoId = static_cast<int32_t>(readU32At(data, pos));  pos += 4;
        out.kills[i].required  = readU32At(data, pos);                         pos += 4;
    }

    for (int i = 0; i < 6; ++i) {
        if (pos + 8 > data.size()) break;
        out.items[i].itemId   = readU32At(data, pos);  pos += 4;
        out.items[i].required = readU32At(data, pos);  pos += 4;
    }

    out.valid = true;
    return out;
}

static QuestQueryObjectives extractQuestQueryObjectives(const std::vector<uint8_t>& data, bool classicHint) {
    if (data.size() < 16) return {};

    const size_t base = 8;
    const size_t classicStart = base + 40u * 4u;
    const size_t wotlkStart   = base + 55u * 4u;

    if (classicHint) {
        auto r = tryParseQuestObjectivesAt(data, classicStart, 4);
        if (r.valid) return r;
        return tryParseQuestObjectivesAt(data, wotlkStart, 5);
    } else {
        auto r = tryParseQuestObjectivesAt(data, wotlkStart, 5);
        if (r.valid) return r;
        return tryParseQuestObjectivesAt(data, classicStart, 4);
    }
}

struct QuestQueryRewards {
    int32_t  rewardMoney = 0;
    std::array<uint32_t, 4> itemId{};
    std::array<uint32_t, 4> itemCount{};
    std::array<uint32_t, 6> choiceItemId{};
    std::array<uint32_t, 6> choiceItemCount{};
    bool valid = false;
};

static QuestQueryRewards tryParseQuestRewards(const std::vector<uint8_t>& data,
                                               bool classicLayout) {
    const size_t base = 8;
    const size_t fieldCount = classicLayout ? 40u : 55u;
    const size_t headerEnd = base + fieldCount * 4u;
    if (data.size() < headerEnd) return {};

    const size_t moneyField     = classicLayout ? 14u : 17u;
    const size_t itemIdField    = classicLayout ? 20u : 30u;
    const size_t itemCountField = classicLayout ? 24u : 34u;
    const size_t choiceIdField  = classicLayout ? 28u : 38u;
    const size_t choiceCntField = classicLayout ? 34u : 44u;

    QuestQueryRewards out;
    out.rewardMoney = static_cast<int32_t>(readU32At(data, base + moneyField * 4u));
    for (size_t i = 0; i < 4; ++i) {
        out.itemId[i]    = readU32At(data, base + (itemIdField    + i) * 4u);
        out.itemCount[i] = readU32At(data, base + (itemCountField + i) * 4u);
    }
    for (size_t i = 0; i < 6; ++i) {
        out.choiceItemId[i]    = readU32At(data, base + (choiceIdField  + i) * 4u);
        out.choiceItemCount[i] = readU32At(data, base + (choiceCntField + i) * 4u);
    }
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

QuestHandler::QuestHandler(GameHandler& owner)
    : owner_(owner) {}

// ---------------------------------------------------------------------------
// Opcode registrations
// ---------------------------------------------------------------------------

void QuestHandler::registerOpcodes(DispatchTable& table) {

    // ---- SMSG_GOSSIP_MESSAGE ----
    table[Opcode::SMSG_GOSSIP_MESSAGE] = [this](network::Packet& packet) { handleGossipMessage(packet); };

    // ---- SMSG_QUESTGIVER_QUEST_LIST ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_LIST] = [this](network::Packet& packet) { handleQuestgiverQuestList(packet); };

    // ---- SMSG_GOSSIP_COMPLETE ----
    table[Opcode::SMSG_GOSSIP_COMPLETE] = [this](network::Packet& packet) { handleGossipComplete(packet); };

    // ---- SMSG_GOSSIP_POI ----
    table[Opcode::SMSG_GOSSIP_POI] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(20)) return;
        /*uint32_t flags =*/ packet.readUInt32();
        float poiX = packet.readFloat();
        float poiY = packet.readFloat();
        uint32_t icon = packet.readUInt32();
        uint32_t data = packet.readUInt32();
        std::string name = packet.readString();
        GossipPoi poi; poi.x = poiX; poi.y = poiY; poi.icon = icon; poi.data = data; poi.name = std::move(name);
        if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
        gossipPois_.push_back(std::move(poi));
        LOG_DEBUG("SMSG_GOSSIP_POI: x=", poiX, " y=", poiY, " icon=", icon);
    };

    // ---- SMSG_QUESTGIVER_QUEST_DETAILS ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_DETAILS] = [this](network::Packet& packet) { handleQuestDetails(packet); };

    // ---- SMSG_QUESTLOG_FULL ----
    table[Opcode::SMSG_QUESTLOG_FULL] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("Your quest log is full.");
        owner_.addSystemChatMessage("Your quest log is full.");
    };

    // ---- SMSG_QUESTGIVER_REQUEST_ITEMS ----
    table[Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS] = [this](network::Packet& packet) { handleQuestRequestItems(packet); };

    // ---- SMSG_QUESTGIVER_OFFER_REWARD ----
    table[Opcode::SMSG_QUESTGIVER_OFFER_REWARD] = [this](network::Packet& packet) { handleQuestOfferReward(packet); };

    // ---- SMSG_QUEST_CONFIRM_ACCEPT ----
    table[Opcode::SMSG_QUEST_CONFIRM_ACCEPT] = [this](network::Packet& packet) { handleQuestConfirmAccept(packet); };

    // ---- SMSG_QUEST_POI_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_POI_QUERY_RESPONSE] = [this](network::Packet& packet) { handleQuestPoiQueryResponse(packet); };

    // ---- SMSG_QUESTGIVER_STATUS ----
    table[Opcode::SMSG_QUESTGIVER_STATUS] = [this](network::Packet& packet) {
        if (packet.hasRemaining(9)) {
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.packetParsers_->readQuestGiverStatus(packet);
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTGIVER_STATUS_MULTIPLE ----
    table[Opcode::SMSG_QUESTGIVER_STATUS_MULTIPLE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count; ++i) {
            if (!packet.hasRemaining(9)) break;
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.packetParsers_->readQuestGiverStatus(packet);
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILED ----
    table[Opcode::SMSG_QUESTUPDATE_FAILED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            owner_.addSystemChatMessage(questTitle.empty() ? std::string("Quest failed!")
                                                           : ('"' + questTitle + "\" failed!"));
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILEDTIMER ----
    table[Opcode::SMSG_QUESTUPDATE_FAILEDTIMER] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            owner_.addSystemChatMessage(questTitle.empty() ? std::string("Quest timed out!")
                                                           : ('"' + questTitle + "\" has timed out."));
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_FAILED ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_FAILED] = [this](network::Packet& packet) {
        // uint32 questId + uint32 reason
        if (packet.hasRemaining(8)) {
            uint32_t questId = packet.readUInt32();
            uint32_t reason = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            const char* reasonStr = nullptr;
            switch (reason) {
                case 1: reasonStr = "failed conditions"; break;
                case 2: reasonStr = "inventory full"; break;
                case 3: reasonStr = "too far away"; break;
                case 4: reasonStr = "another quest is blocking"; break;
                case 5: reasonStr = "wrong time of day"; break;
                case 6: reasonStr = "wrong race"; break;
                case 7: reasonStr = "wrong class"; break;
            }
            std::string msg = questTitle.empty() ? "Quest" : ('"' + questTitle + '"');
            msg += " failed";
            if (reasonStr) msg += std::string(": ") + reasonStr;
            msg += '.';
            owner_.addSystemChatMessage(msg);
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_INVALID ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_INVALID] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t failReason = packet.readUInt32();
            pendingTurnInRewardRequest_ = false;
            const char* reasonStr = "Unknown";
            switch (failReason) {
                case 0: reasonStr = "Don't have quest"; break;
                case 1: reasonStr = "Quest level too low"; break;
                case 4: reasonStr = "Insufficient money"; break;
                case 5: reasonStr = "Inventory full"; break;
                case 13: reasonStr = "Already on that quest"; break;
                case 18: reasonStr = "Already completed quest"; break;
                case 19: reasonStr = "Can't take any more quests"; break;
            }
            LOG_WARNING("Quest invalid: reason=", failReason, " (", reasonStr, ")");
            if (!pendingQuestAcceptTimeouts_.empty()) {
                std::vector<uint32_t> pendingQuestIds;
                pendingQuestIds.reserve(pendingQuestAcceptTimeouts_.size());
                for (const auto& pending : pendingQuestAcceptTimeouts_) {
                    pendingQuestIds.push_back(pending.first);
                }
                for (uint32_t questId : pendingQuestIds) {
                    const uint64_t npcGuid = pendingQuestAcceptNpcGuids_.count(questId) != 0
                        ? pendingQuestAcceptNpcGuids_[questId] : 0;
                    if (failReason == 13) {
                        std::string fallbackTitle = "Quest #" + std::to_string(questId);
                        std::string fallbackObjectives;
                        if (currentQuestDetails_.questId == questId) {
                            if (!currentQuestDetails_.title.empty()) fallbackTitle = currentQuestDetails_.title;
                            fallbackObjectives = currentQuestDetails_.objectives;
                        }
                        addQuestToLocalLogIfMissing(questId, fallbackTitle, fallbackObjectives);
                        triggerQuestAcceptResync(questId, npcGuid, "already-on-quest");
                    } else if (failReason == 18) {
                        triggerQuestAcceptResync(questId, npcGuid, "already-completed");
                    }
                    clearPendingQuestAccept(questId);
                }
            }
            // Only show error to user for real errors (not informational messages)
            if (failReason != 13 && failReason != 18) {  // Don't spam "already on/completed"
                owner_.addSystemChatMessage(std::string("Quest unavailable: ") + reasonStr);
            }
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_COMPLETE ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            LOG_INFO("Quest completed: questId=", questId);
            if (pendingTurnInQuestId_ == questId) {
                pendingTurnInQuestId_ = 0;
                pendingTurnInNpcGuid_ = 0;
                pendingTurnInRewardRequest_ = false;
            }
            for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
                if (it->questId == questId) {
                    // Fire toast callback before erasing
                    if (owner_.questCompleteCallback_) {
                        owner_.questCompleteCallback_(questId, it->title);
                    }
                    // Play quest-complete sound
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager())
                            sfx->playQuestComplete();
                    }
                    questLog_.erase(it);
                    LOG_INFO("  Removed quest ", questId, " from quest log");
                    if (owner_.addonEventCallback_)
                        owner_.addonEventCallback_("QUEST_TURNED_IN", {std::to_string(questId)});
                    break;
                }
            }
        }
        if (owner_.addonEventCallback_) {
            owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
        }
        // Re-query all nearby quest giver NPCs so markers refresh
        if (owner_.socket) {
            for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
                if (entity->getType() != ObjectType::UNIT) continue;
                auto unit = std::static_pointer_cast<Unit>(entity);
                if (unit->getNpcFlags() & 0x02) {
                    network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                    qsPkt.writeUInt64(guid);
                    owner_.socket->send(qsPkt);
                }
            }
        }
    };

    // ---- SMSG_QUESTUPDATE_ADD_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_KILL] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            uint32_t entry = packet.readUInt32();
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) {
                reqCount = packet.readUInt32();
            }

            LOG_INFO("Quest kill update: questId=", questId, " entry=", entry,
                     " count=", count, "/", reqCount);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    if (reqCount == 0) {
                        auto it = quest.killCounts.find(entry);
                        if (it != quest.killCounts.end()) reqCount = it->second.second;
                    }
                    // Fall back to killObjectives (parsed from SMSG_QUEST_QUERY_RESPONSE).
                    if (reqCount == 0) {
                        for (const auto& obj : quest.killObjectives) {
                            if (obj.npcOrGoId == 0 || obj.required == 0) continue;
                            uint32_t objEntry = static_cast<uint32_t>(
                                obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
                            if (objEntry == entry) {
                                reqCount = obj.required;
                                break;
                            }
                        }
                    }
                    // Some quests (e.g. escort/event quests) report kill credit updates without
                    // a corresponding objective count in SMSG_QUEST_QUERY_RESPONSE. Fall back to
                    // current count so the progress display shows "N/N" instead of "N/0".
                    if (reqCount == 0) reqCount = count;
                    quest.killCounts[entry] = {count, reqCount};

                    std::string creatureName = owner_.getCachedCreatureName(entry);
                    std::string progressMsg = quest.title + ": ";
                    if (!creatureName.empty()) {
                        progressMsg += creatureName + " ";
                    }
                    progressMsg += std::to_string(count) + "/" + std::to_string(reqCount);
                    owner_.addSystemChatMessage(progressMsg);

                    if (owner_.questProgressCallback_) {
                        owner_.questProgressCallback_(quest.title, creatureName, count, reqCount);
                    }
                    if (owner_.addonEventCallback_) {
                        owner_.addonEventCallback_("QUEST_WATCH_UPDATE", {std::to_string(questId)});
                        owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
                        owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
                    }

                    LOG_INFO("Updated kill count for quest ", questId, ": ",
                             count, "/", reqCount);
                    break;
                }
            }
        } else if (rem >= 4) {
            // Swapped mapping fallback: treat as QUESTUPDATE_COMPLETE packet.
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            LOG_INFO("Quest objectives completed (compat via ADD_KILL): questId=", questId);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    owner_.addSystemChatMessage("Quest Complete: " + quest.title);
                    break;
                }
            }
        }
    };

    // ---- SMSG_QUESTUPDATE_ADD_ITEM ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_ITEM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t itemId = packet.readUInt32();
            uint32_t count = packet.readUInt32();
            owner_.queryItemInfo(itemId, 0);

            std::string itemLabel = "item #" + std::to_string(itemId);
            uint32_t questItemQuality = 1;
            if (const ItemQueryResponseData* info = owner_.getItemInfo(itemId)) {
                if (!info->name.empty()) itemLabel = info->name;
                questItemQuality = info->quality;
            }

            bool updatedAny = false;
            for (auto& quest : questLog_) {
                if (quest.complete) continue;
                bool tracksItem =
                    quest.requiredItemCounts.count(itemId) > 0 ||
                    quest.itemCounts.count(itemId) > 0;
                // Also check itemObjectives parsed from SMSG_QUEST_QUERY_RESPONSE in case
                // requiredItemCounts hasn't been populated yet (race during quest accept).
                if (!tracksItem) {
                    for (const auto& obj : quest.itemObjectives) {
                        if (obj.itemId == itemId && obj.required > 0) {
                            quest.requiredItemCounts.emplace(itemId, obj.required);
                            tracksItem = true;
                            break;
                        }
                    }
                }
                if (!tracksItem) continue;
                quest.itemCounts[itemId] = count;
                updatedAny = true;
            }
            owner_.addSystemChatMessage("Quest item: " + buildItemLink(itemId, questItemQuality, itemLabel) + " (" + std::to_string(count) + ")");

            if (owner_.questProgressCallback_ && updatedAny) {
                for (const auto& quest : questLog_) {
                    if (quest.complete) continue;
                    if (quest.itemCounts.count(itemId) == 0) continue;
                    uint32_t required = 0;
                    auto rIt = quest.requiredItemCounts.find(itemId);
                    if (rIt != quest.requiredItemCounts.end()) required = rIt->second;
                    if (required == 0) {
                        for (const auto& obj : quest.itemObjectives) {
                            if (obj.itemId == itemId) { required = obj.required; break; }
                        }
                    }
                    if (required == 0) required = count;
                    owner_.questProgressCallback_(quest.title, itemLabel, count, required);
                    break;
                }
            }

            if (owner_.addonEventCallback_ && updatedAny) {
                owner_.addonEventCallback_("QUEST_WATCH_UPDATE", {});
                owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
                owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
            }
            LOG_INFO("Quest item update: itemId=", itemId, " count=", count,
                     " trackedQuestsUpdated=", updatedAny);
        }
    };

    // ---- SMSG_QUESTUPDATE_COMPLETE ----
    table[Opcode::SMSG_QUESTUPDATE_COMPLETE] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            uint32_t entry = packet.readUInt32();
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) reqCount = packet.readUInt32();
            if (reqCount == 0) reqCount = count;
            LOG_INFO("Quest kill update (compat via COMPLETE): questId=", questId,
                     " entry=", entry, " count=", count, "/", reqCount);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.killCounts[entry] = {count, reqCount};
                    owner_.addSystemChatMessage(quest.title + ": " + std::to_string(count) +
                                                 "/" + std::to_string(reqCount));
                    break;
                }
            }
        } else if (rem >= 4) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            LOG_INFO("Quest objectives completed: questId=", questId);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    owner_.addSystemChatMessage("Quest Complete: " + quest.title);
                    LOG_INFO("Marked quest ", questId, " as complete");
                    break;
                }
            }
        }
    };

    // ---- SMSG_QUEST_FORCE_REMOVE ----
    table[Opcode::SMSG_QUEST_FORCE_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("SMSG_QUEST_FORCE_REMOVE/SET_REST_START too short");
            return;
        }
        uint32_t value = packet.readUInt32();

        // WotLK uses this opcode as SMSG_SET_REST_START
        if (!isClassicLikeExpansion() && !isActiveExpansion("tbc")) {
            bool nowResting = (value != 0);
            if (nowResting != owner_.isResting_) {
                owner_.isResting_ = nowResting;
                owner_.addSystemChatMessage(owner_.isResting_ ? "You are now resting."
                                                              : "You are no longer resting.");
                if (owner_.addonEventCallback_)
                    owner_.addonEventCallback_("PLAYER_UPDATE_RESTING", {});
            }
            return;
        }

        // Classic/TBC: treat as QUEST_FORCE_REMOVE (uint32 questId)
        uint32_t questId = value;
        clearPendingQuestAccept(questId);
        pendingQuestQueryIds_.erase(questId);
        if (questId == 0) {
            return;
        }

        bool removed = false;
        std::string removedTitle;
        for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
            if (it->questId == questId) {
                removedTitle = it->title;
                questLog_.erase(it);
                removed = true;
                break;
            }
        }
        if (currentQuestDetails_.questId == questId) {
            questDetailsOpen_ = false;
            questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
            currentQuestDetails_ = QuestDetailsData{};
            removed = true;
        }
        if (currentQuestRequestItems_.questId == questId) {
            questRequestItemsOpen_ = false;
            currentQuestRequestItems_ = QuestRequestItemsData{};
            removed = true;
        }
        if (currentQuestOfferReward_.questId == questId) {
            questOfferRewardOpen_ = false;
            currentQuestOfferReward_ = QuestOfferRewardData{};
            removed = true;
        }
        if (removed) {
            if (!removedTitle.empty()) {
                owner_.addSystemChatMessage("Quest removed: " + removedTitle);
            } else {
                owner_.addSystemChatMessage("Quest removed (ID " + std::to_string(questId) + ").");
            }
            if (owner_.addonEventCallback_) {
                owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
                owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
                owner_.addonEventCallback_("QUEST_REMOVED", {std::to_string(questId)});
            }
        }
    };

    // ---- SMSG_QUEST_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (packet.getSize() < 8) {
            LOG_WARNING("SMSG_QUEST_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
            return;
        }

        uint32_t questId = packet.readUInt32();
        packet.readUInt32(); // questMethod

        const bool isClassicLayout = owner_.packetParsers_ && owner_.packetParsers_->questLogStride() <= 4;
        const QuestQueryTextCandidate parsed = pickBestQuestQueryTexts(packet.getData(), isClassicLayout);
        const QuestQueryObjectives objs = extractQuestQueryObjectives(packet.getData(), isClassicLayout);
        const QuestQueryRewards rwds = tryParseQuestRewards(packet.getData(), isClassicLayout);

        for (auto& q : questLog_) {
            if (q.questId != questId) continue;

            const int existingScore = scoreQuestTitle(q.title);
            const bool parsedStrong = isStrongQuestTitle(parsed.title);
            const bool parsedLongEnough = parsed.title.size() >= 6;
            const bool notShorterThanExisting =
                isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.title.size() + 2 >= q.title.size();
            const bool shouldReplaceTitle =
                parsed.score > -1000 &&
                parsedStrong &&
                parsedLongEnough &&
                notShorterThanExisting &&
                (isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.score >= existingScore + 12);

            if (shouldReplaceTitle && !parsed.title.empty()) {
                q.title = parsed.title;
            }
            if (!parsed.objectives.empty() &&
                (q.objectives.empty() || q.objectives.size() < 16)) {
                q.objectives = parsed.objectives;
            }

            // Store structured kill/item objectives for later kill-count restoration.
            if (objs.valid) {
                for (int i = 0; i < 4; ++i) {
                    q.killObjectives[i].npcOrGoId = objs.kills[i].npcOrGoId;
                    q.killObjectives[i].required  = objs.kills[i].required;
                }
                for (int i = 0; i < 6; ++i) {
                    q.itemObjectives[i].itemId   = objs.items[i].itemId;
                    q.itemObjectives[i].required = objs.items[i].required;
                }
                applyPackedKillCountsFromFields(q);
                for (int i = 0; i < 4; ++i) {
                    int32_t id = objs.kills[i].npcOrGoId;
                    if (id == 0 || objs.kills[i].required == 0) continue;
                    if (id > 0) owner_.queryCreatureInfo(static_cast<uint32_t>(id), 0);
                    else        owner_.queryGameObjectInfo(static_cast<uint32_t>(-id), 0);
                }
                for (int i = 0; i < 6; ++i) {
                    if (objs.items[i].itemId != 0 && objs.items[i].required != 0)
                        owner_.queryItemInfo(objs.items[i].itemId, 0);
                }
                LOG_DEBUG("Quest ", questId, " objectives parsed: kills=[",
                          objs.kills[0].npcOrGoId, "/", objs.kills[0].required, ", ",
                          objs.kills[1].npcOrGoId, "/", objs.kills[1].required, ", ",
                          objs.kills[2].npcOrGoId, "/", objs.kills[2].required, ", ",
                          objs.kills[3].npcOrGoId, "/", objs.kills[3].required, "]");
            }

            // Store reward data and pre-fetch item info for icons.
            if (rwds.valid) {
                q.rewardMoney = rwds.rewardMoney;
                for (int i = 0; i < 4; ++i) {
                    q.rewardItems[i].itemId = rwds.itemId[i];
                    q.rewardItems[i].count  = (rwds.itemId[i] != 0) ? rwds.itemCount[i] : 0;
                    if (rwds.itemId[i] != 0) owner_.queryItemInfo(rwds.itemId[i], 0);
                }
                for (int i = 0; i < 6; ++i) {
                    q.rewardChoiceItems[i].itemId = rwds.choiceItemId[i];
                    q.rewardChoiceItems[i].count  = (rwds.choiceItemId[i] != 0) ? rwds.choiceItemCount[i] : 0;
                    if (rwds.choiceItemId[i] != 0) owner_.queryItemInfo(rwds.choiceItemId[i], 0);
                }
            }
            break;
        }

        pendingQuestQueryIds_.erase(questId);
    };

    // ---- SMSG_QUESTUPDATE_ADD_PVP_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_PVP_KILL] = [this](network::Packet& packet) {
        if (packet.hasRemaining(16)) {
            /*uint64_t guid =*/ packet.readUInt64();
            uint32_t questId = packet.readUInt32();
            uint32_t count   = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) {
                reqCount = packet.readUInt32();
            }

            constexpr uint32_t PVP_KILL_ENTRY = 0u;
            for (auto& quest : questLog_) {
                if (quest.questId != questId) continue;

                if (reqCount == 0) {
                    auto it = quest.killCounts.find(PVP_KILL_ENTRY);
                    if (it != quest.killCounts.end()) reqCount = it->second.second;
                }
                if (reqCount == 0) {
                    for (const auto& obj : quest.killObjectives) {
                        if (obj.npcOrGoId == 0 && obj.required > 0) {
                            reqCount = obj.required;
                            break;
                        }
                    }
                }
                if (reqCount == 0) reqCount = count;
                quest.killCounts[PVP_KILL_ENTRY] = {count, reqCount};

                std::string progressMsg = quest.title + ": PvP kills " +
                    std::to_string(count) + "/" + std::to_string(reqCount);
                owner_.addSystemChatMessage(progressMsg);
                break;
            }
        }
    };

    // ---- Completed quests response (moved from GameHandler) ----
    table[Opcode::SMSG_QUERY_QUESTS_COMPLETED_RESPONSE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t count = packet.readUInt32();
            if (count <= 4096) {
                for (uint32_t i = 0; i < count; ++i) {
                    if (!packet.hasRemaining(4)) break;
                    uint32_t questId = packet.readUInt32();
                    owner_.completedQuests_.insert(questId);
                }
                LOG_DEBUG("SMSG_QUERY_QUESTS_COMPLETED_RESPONSE: ", count, " completed quests");
            }
        }
        packet.skipAll();
    };
}

// ---------------------------------------------------------------------------
// Public API methods
// ---------------------------------------------------------------------------

void QuestHandler::selectGossipOption(uint32_t optionId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || !gossipWindowOpen_) return;
    LOG_INFO("selectGossipOption: optionId=", optionId,
             " npcGuid=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " menuId=", currentGossip_.menuId,
             " numOptions=", currentGossip_.options.size());
    auto packet = GossipSelectOptionPacket::build(currentGossip_.npcGuid, currentGossip_.menuId, optionId);
    owner_.socket->send(packet);

    for (const auto& opt : currentGossip_.options) {
        if (opt.id != optionId) continue;
        LOG_INFO("  matched option: id=", opt.id, " icon=", (int)opt.icon, " text='", opt.text, "'");

        std::string text = opt.text;
        std::string textLower = text;
        std::transform(textLower.begin(), textLower.end(), textLower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        // Icon- and text-based NPC interaction fallbacks.
        // Use flags to avoid sending the same activation packet twice when
        // both the icon and text match (e.g., banker icon 6 + "deposit box").
        bool sentBanker = false;
        bool sentAuction = false;

        if (opt.icon == 6) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.socket->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (icon) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentAuction && (text == "GOSSIP_OPTION_AUCTIONEER" || textLower.find("auction") != std::string::npos)) {
            auto pkt = AuctionHelloPacket::build(currentGossip_.npcGuid);
            owner_.socket->send(pkt);
            sentAuction = true;
            LOG_INFO("Sent MSG_AUCTION_HELLO for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentBanker && (text == "GOSSIP_OPTION_BANKER" || textLower.find("deposit box") != std::string::npos)) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.socket->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (text) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        const bool isVendor = (text == "GOSSIP_OPTION_VENDOR" ||
                               (textLower.find("browse") != std::string::npos &&
                                (textLower.find("goods") != std::string::npos || textLower.find("wares") != std::string::npos)));
        const bool isArmorer = (text == "GOSSIP_OPTION_ARMORER" || textLower.find("repair") != std::string::npos);
        if (isVendor || isArmorer) {
            if (isArmorer) {
                owner_.setVendorCanRepair(true);
            }
            auto pkt = ListInventoryPacket::build(currentGossip_.npcGuid);
            owner_.socket->send(pkt);
            LOG_DEBUG("Sent CMSG_LIST_INVENTORY (gossip) to npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (textLower.find("make this inn your home") != std::string::npos ||
            textLower.find("set your home") != std::string::npos) {
            auto bindPkt = BinderActivatePacket::build(currentGossip_.npcGuid);
            owner_.socket->send(bindPkt);
            LOG_INFO("Sent CMSG_BINDER_ACTIVATE for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        // Stable master detection
        if (text == "GOSSIP_OPTION_STABLE" ||
            textLower.find("stable") != std::string::npos ||
            textLower.find("my pet") != std::string::npos) {
            owner_.stableMasterGuid_ = currentGossip_.npcGuid;
            owner_.stableWindowOpen_ = false;
            auto listPkt = ListStabledPetsPacket::build(currentGossip_.npcGuid);
            owner_.socket->send(listPkt);
            LOG_INFO("Sent MSG_LIST_STABLED_PETS (gossip) to npc=0x",
                     std::hex, currentGossip_.npcGuid, std::dec);
        }
        break;
    }
}

void QuestHandler::selectGossipQuest(uint32_t questId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || !gossipWindowOpen_) return;

    const QuestLogEntry* activeQuest = nullptr;
    for (const auto& q : questLog_) {
        if (q.questId == questId) {
            activeQuest = &q;
            break;
        }
    }

    // Validate against server-auth quest slot fields
    auto questInServerLogSlots = [&](uint32_t qid) -> bool {
        if (qid == 0 || owner_.lastPlayerFields_.empty()) return false;
        const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
        const uint8_t qStride = owner_.packetParsers_ ? owner_.packetParsers_->questLogStride() : 5;
        const uint16_t ufQuestEnd = ufQuestStart + 25 * qStride;
        for (const auto& [key, val] : owner_.lastPlayerFields_) {
            if (key < ufQuestStart || key >= ufQuestEnd) continue;
            if ((key - ufQuestStart) % qStride != 0) continue;
            if (val == qid) return true;
        }
        return false;
    };
    const bool questInServerLog = questInServerLogSlots(questId);
    if (questInServerLog && !activeQuest) {
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        requestQuestQuery(questId, false);
        for (const auto& q : questLog_) {
            if (q.questId == questId) {
                activeQuest = &q;
                break;
            }
        }
    }
    const bool activeQuestConfirmedByServer = questInServerLog;
    const bool shouldStartProgressFlow = activeQuestConfirmedByServer;
    if (shouldStartProgressFlow) {
        pendingTurnInQuestId_ = questId;
        pendingTurnInNpcGuid_ = currentGossip_.npcGuid;
        pendingTurnInRewardRequest_ = activeQuest ? activeQuest->complete : false;
        auto packet = QuestgiverCompleteQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.socket->send(packet);
    } else {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
        auto packet = owner_.packetParsers_
            ? owner_.packetParsers_->buildQueryQuestPacket(currentGossip_.npcGuid, questId)
            : QuestgiverQueryQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.socket->send(packet);
    }

    gossipWindowOpen_ = false;
}

bool QuestHandler::requestQuestQuery(uint32_t questId, bool force) {
    if (questId == 0 || owner_.state != WorldState::IN_WORLD || !owner_.socket) return false;
    if (!force && pendingQuestQueryIds_.count(questId)) return false;

    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_QUERY));
    pkt.writeUInt32(questId);
    owner_.socket->send(pkt);
    pendingQuestQueryIds_.insert(questId);

    // WotLK supports CMSG_QUEST_POI_QUERY to get objective map locations.
    if (owner_.packetParsers_ && owner_.packetParsers_->questLogStride() == 5) {
        const uint32_t wirePoiQuery = wireOpcode(Opcode::CMSG_QUEST_POI_QUERY);
        if (wirePoiQuery != 0xFFFF) {
            network::Packet poiPkt(static_cast<uint16_t>(wirePoiQuery));
            poiPkt.writeUInt32(1);          // count = 1
            poiPkt.writeUInt32(questId);
            owner_.socket->send(poiPkt);
        }
    }
    return true;
}

void QuestHandler::setQuestTracked(uint32_t questId, bool tracked) {
    if (tracked) {
        trackedQuestIds_.insert(questId);
    } else {
        trackedQuestIds_.erase(questId);
    }
}

void QuestHandler::acceptQuest() {
    if (!questDetailsOpen_ || owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    const uint32_t questId = currentQuestDetails_.questId;
    if (questId == 0) return;
    uint64_t npcGuid = currentQuestDetails_.npcGuid;
    if (pendingQuestAcceptTimeouts_.count(questId) != 0) {
        LOG_DEBUG("Ignoring duplicate quest accept while pending: questId=", questId);
        triggerQuestAcceptResync(questId, npcGuid, "duplicate-accept");
        questDetailsOpen_ = false;
        questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
        currentQuestDetails_ = QuestDetailsData{};
        return;
    }
    const bool inLocalLog = hasQuestInLog(questId);
    const int serverSlot = findQuestLogSlotIndexFromServer(questId);
    if (serverSlot >= 0) {
        LOG_INFO("Ignoring duplicate quest accept already in server quest log: questId=", questId,
                 " slot=", serverSlot);
        questDetailsOpen_ = false;
        questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
        currentQuestDetails_ = QuestDetailsData{};
        return;
    }
    if (inLocalLog) {
        LOG_WARNING("Quest accept local/server mismatch, allowing re-accept: questId=", questId);
        std::erase_if(questLog_, [&](const QuestLogEntry& q) { return q.questId == questId; });
    }

    network::Packet packet = owner_.packetParsers_
        ? owner_.packetParsers_->buildAcceptQuestPacket(npcGuid, questId)
        : QuestgiverAcceptQuestPacket::build(npcGuid, questId);
    owner_.socket->send(packet);
    pendingQuestAcceptTimeouts_[questId] = 5.0f;
    pendingQuestAcceptNpcGuids_[questId] = npcGuid;

    // Play quest-accept sound
    if (auto* ac = owner_.services().audioCoordinator) {
        if (auto* sfx = ac->getUiSoundManager())
            sfx->playQuestActivate();
    }

    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    currentQuestDetails_ = QuestDetailsData{};

    // Re-query quest giver status so marker updates (! → ?)
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.socket->send(qsPkt);
    }
}

void QuestHandler::declineQuest() {
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    currentQuestDetails_ = QuestDetailsData{};
}

void QuestHandler::closeGossip() {
    gossipWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
}

void QuestHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    if (itemGuid == 0 || questId == 0) {
        owner_.addSystemChatMessage("Cannot start quest right now.");
        return;
    }
    // Send CMSG_QUESTGIVER_QUERY_QUEST with the item GUID as the "questgiver."
    // The server responds with SMSG_QUESTGIVER_QUEST_DETAILS which handleQuestDetails()
    // picks up and opens the Accept/Decline dialog.
    auto queryPkt = owner_.packetParsers_
        ? owner_.packetParsers_->buildQueryQuestPacket(itemGuid, questId)
        : QuestgiverQueryQuestPacket::build(itemGuid, questId);
    owner_.socket->send(queryPkt);
    LOG_INFO("offerQuestFromItem: itemGuid=0x", std::hex, itemGuid, std::dec,
             " questId=", questId);
}

void QuestHandler::completeQuest() {
    if (!questRequestItemsOpen_ || owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    pendingTurnInQuestId_ = currentQuestRequestItems_.questId;
    pendingTurnInNpcGuid_ = currentQuestRequestItems_.npcGuid;
    pendingTurnInRewardRequest_ = currentQuestRequestItems_.isCompletable();

    auto packet = QuestgiverCompleteQuestPacket::build(
        currentQuestRequestItems_.npcGuid, currentQuestRequestItems_.questId);
    owner_.socket->send(packet);
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
}

void QuestHandler::closeQuestRequestItems() {
    pendingTurnInRewardRequest_ = false;
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
}

void QuestHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (!questOfferRewardOpen_ || owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    uint64_t npcGuid = currentQuestOfferReward_.npcGuid;
    LOG_INFO("Completing quest: questId=", currentQuestOfferReward_.questId,
             " npcGuid=", npcGuid, " rewardIndex=", rewardIndex);
    auto packet = QuestgiverChooseRewardPacket::build(
        npcGuid, currentQuestOfferReward_.questId, rewardIndex);
    owner_.socket->send(packet);
    pendingTurnInQuestId_ = 0;
    pendingTurnInNpcGuid_ = 0;
    pendingTurnInRewardRequest_ = false;
    questOfferRewardOpen_ = false;
    currentQuestOfferReward_ = QuestOfferRewardData{};

    // Re-query quest giver status so markers update
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.socket->send(qsPkt);
    }
}

void QuestHandler::closeQuestOfferReward() {
    pendingTurnInRewardRequest_ = false;
    questOfferRewardOpen_ = false;
    currentQuestOfferReward_ = QuestOfferRewardData{};
}

void QuestHandler::abandonQuest(uint32_t questId) {
    clearPendingQuestAccept(questId);
    int localIndex = -1;
    for (size_t i = 0; i < questLog_.size(); ++i) {
        if (questLog_[i].questId == questId) {
            localIndex = static_cast<int>(i);
            break;
        }
    }

    int slotIndex = findQuestLogSlotIndexFromServer(questId);
    if (slotIndex < 0 && localIndex >= 0) {
        slotIndex = localIndex;
        LOG_WARNING("Abandon quest using local slot fallback: questId=", questId, " slot=", slotIndex);
    }

    if (slotIndex >= 0 && slotIndex < 25) {
        if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
            network::Packet pkt(wireOpcode(Opcode::CMSG_QUESTLOG_REMOVE_QUEST));
            pkt.writeUInt8(static_cast<uint8_t>(slotIndex));
            owner_.socket->send(pkt);
        }
    } else {
        LOG_WARNING("Abandon quest failed: no quest-log slot found for questId=", questId);
    }

    if (localIndex >= 0) {
        questLog_.erase(questLog_.begin() + static_cast<ptrdiff_t>(localIndex));
        if (owner_.addonEventCallback_) {
            owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
            owner_.addonEventCallback_("QUEST_REMOVED", {std::to_string(questId)});
        }
    }

    // Remove any quest POI minimap markers for this quest.
    gossipPois_.erase(
        std::remove_if(gossipPois_.begin(), gossipPois_.end(),
            [questId](const GossipPoi& p) { return p.data == questId; }),
        gossipPois_.end());
}

void QuestHandler::shareQuestWithParty(uint32_t questId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) {
        owner_.addSystemChatMessage("Cannot share quest: not in world.");
        return;
    }
    if (!owner_.isInGroup()) {
        owner_.addSystemChatMessage("You must be in a group to share a quest.");
        return;
    }
    network::Packet pkt(wireOpcode(Opcode::CMSG_PUSHQUESTTOPARTY));
    pkt.writeUInt32(questId);
    owner_.socket->send(pkt);
    // Local feedback: find quest title
    for (const auto& q : questLog_) {
        if (q.questId == questId && !q.title.empty()) {
            owner_.addSystemChatMessage("Sharing quest: " + q.title);
            return;
        }
    }
    owner_.addSystemChatMessage("Quest shared.");
}

void QuestHandler::acceptSharedQuest() {
    if (!pendingSharedQuest_ || !owner_.socket) return;
    pendingSharedQuest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_CONFIRM_ACCEPT));
    pkt.writeUInt32(sharedQuestId_);
    owner_.socket->send(pkt);
    owner_.addSystemChatMessage("Accepted: " + sharedQuestTitle_);
}

void QuestHandler::declineSharedQuest() {
    pendingSharedQuest_ = false;
    // No response packet needed — just dismiss the UI
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool QuestHandler::hasQuestInLog(uint32_t questId) const {
    for (const auto& q : questLog_) {
        if (q.questId == questId) return true;
    }
    return false;
}

int QuestHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questId == 0 || owner_.lastPlayerFields_.empty()) return -1;
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.packetParsers_ ? owner_.packetParsers_->questLogStride() : 5;
    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField = ufQuestStart + slot * qStride;
        auto it = owner_.lastPlayerFields_.find(idField);
        if (it != owner_.lastPlayerFields_.end() && it->second == questId) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void QuestHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questId == 0 || hasQuestInLog(questId)) return;
    QuestLogEntry entry;
    entry.questId = questId;
    entry.title = title.empty() ? ("Quest #" + std::to_string(questId)) : title;
    entry.objectives = objectives;
    questLog_.push_back(std::move(entry));
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("QUEST_ACCEPTED", {std::to_string(questId)});
        owner_.addonEventCallback_("QUEST_LOG_UPDATE", {});
        owner_.addonEventCallback_("UNIT_QUEST_LOG_CHANGED", {"player"});
    }
}

bool QuestHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    if (owner_.lastPlayerFields_.empty()) return false;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.packetParsers_ ? owner_.packetParsers_->questLogStride() : 5;

    static constexpr uint32_t kQuestStatusComplete = 1;

    std::unordered_map<uint32_t, bool> serverQuestComplete;
    serverQuestComplete.reserve(25);
    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = ufQuestStart + slot * qStride + 1;
        auto it = owner_.lastPlayerFields_.find(idField);
        if (it == owner_.lastPlayerFields_.end()) continue;
        uint32_t questId = it->second;
        if (questId == 0) continue;

        bool complete = false;
        if (qStride >= 2) {
            auto stateIt = owner_.lastPlayerFields_.find(stateField);
            if (stateIt != owner_.lastPlayerFields_.end()) {
                uint32_t state = stateIt->second & 0xFF;
                complete = (state == kQuestStatusComplete);
            }
        }
        serverQuestComplete[questId] = complete;
    }

    std::unordered_set<uint32_t> serverQuestIds;
    serverQuestIds.reserve(serverQuestComplete.size());
    for (const auto& [qid, _] : serverQuestComplete) serverQuestIds.insert(qid);

    const size_t localBefore = questLog_.size();
    std::erase_if(questLog_, [&](const QuestLogEntry& q) {
        return q.questId == 0 || serverQuestIds.count(q.questId) == 0;
    });
    const size_t removed = localBefore - questLog_.size();

    size_t added = 0;
    for (uint32_t questId : serverQuestIds) {
        if (hasQuestInLog(questId)) continue;
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        ++added;
    }

    size_t marked = 0;
    for (auto& quest : questLog_) {
        auto it = serverQuestComplete.find(quest.questId);
        if (it == serverQuestComplete.end()) continue;
        if (it->second && !quest.complete) {
            quest.complete = true;
            ++marked;
            LOG_DEBUG("Quest ", quest.questId, " marked complete from update fields");
        }
    }

    if (forceQueryMetadata) {
        for (uint32_t questId : serverQuestIds) {
            requestQuestQuery(questId, false);
        }
    }

    LOG_INFO("Quest log resync from server slots: server=", serverQuestIds.size(),
             " localBefore=", localBefore, " removed=", removed, " added=", added,
             " markedComplete=", marked);
    return true;
}

void QuestHandler::applyQuestStateFromFields(const std::map<uint16_t, uint32_t>& fields) {
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF || questLog_.empty()) return;

    const uint8_t qStride = owner_.packetParsers_ ? owner_.packetParsers_->questLogStride() : 5;
    if (qStride < 2) return;

    static constexpr uint32_t kQuestStatusComplete = 1;

    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = idField + 1;
        auto idIt = fields.find(idField);
        if (idIt == fields.end()) continue;
        uint32_t questId = idIt->second;
        if (questId == 0) continue;

        auto stateIt = fields.find(stateField);
        if (stateIt == fields.end()) continue;
        bool serverComplete = ((stateIt->second & 0xFF) == kQuestStatusComplete);
        if (!serverComplete) continue;

        for (auto& quest : questLog_) {
            if (quest.questId == questId && !quest.complete) {
                quest.complete = true;
                LOG_INFO("Quest ", questId, " marked complete from VALUES update field state");
                break;
            }
        }
    }
}

void QuestHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (owner_.lastPlayerFields_.empty()) return;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF) return;

    const uint8_t qStride = owner_.packetParsers_ ? owner_.packetParsers_->questLogStride() : 5;
    if (qStride < 3) return;

    int slot = findQuestLogSlotIndexFromServer(quest.questId);
    if (slot < 0) return;

    const uint16_t countField1 = ufQuestStart + static_cast<uint16_t>(slot) * qStride + 2;
    const uint16_t countField2 = (qStride >= 5)
                                     ? static_cast<uint16_t>(countField1 + 1)
                                     : static_cast<uint16_t>(0xFFFF);

    auto f1It = owner_.lastPlayerFields_.find(countField1);
    if (f1It == owner_.lastPlayerFields_.end()) return;
    const uint32_t packed1 = f1It->second;

    uint32_t packed2 = 0;
    if (countField2 != 0xFFFF) {
        auto f2It = owner_.lastPlayerFields_.find(countField2);
        if (f2It != owner_.lastPlayerFields_.end()) packed2 = f2It->second;
    }

    auto unpack6 = [](uint32_t word, int idx) -> uint8_t {
        return static_cast<uint8_t>((word >> (idx * 6)) & 0x3F);
    };
    const uint8_t counts[6] = {
        unpack6(packed1, 0), unpack6(packed1, 1),
        unpack6(packed1, 2), unpack6(packed1, 3),
        unpack6(packed2, 0), unpack6(packed2, 1),
    };

    // Apply kill objective counts (indices 0-3).
    for (int i = 0; i < 4; ++i) {
        const auto& obj = quest.killObjectives[i];
        if (obj.npcOrGoId == 0 || obj.required == 0) continue;
        const uint32_t entryKey = static_cast<uint32_t>(
            obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
        if (counts[i] == 0 && quest.killCounts.count(entryKey)) continue;
        quest.killCounts[entryKey] = {counts[i], obj.required};
        LOG_DEBUG("Quest ", quest.questId, " objective[", i, "]: npcOrGo=",
                  obj.npcOrGoId, " count=", (int)counts[i], "/", obj.required);
    }

    // Apply item objective counts (WotLK only).
    for (int i = 0; i < 6; ++i) {
        const auto& obj = quest.itemObjectives[i];
        if (obj.itemId == 0 || obj.required == 0) continue;
        if (i < 2 && qStride >= 5) {
            uint8_t cnt = counts[4 + i];
            if (cnt > 0) {
                quest.itemCounts[obj.itemId] = std::max(quest.itemCounts[obj.itemId], static_cast<uint32_t>(cnt));
            }
        }
        quest.requiredItemCounts.emplace(obj.itemId, obj.required);
    }
}

void QuestHandler::clearPendingQuestAccept(uint32_t questId) {
    pendingQuestAcceptTimeouts_.erase(questId);
    pendingQuestAcceptNpcGuids_.erase(questId);
}

void QuestHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questId == 0 || !owner_.socket || owner_.state != WorldState::IN_WORLD) return;

    LOG_INFO("Quest accept resync: questId=", questId, " reason=", reason ? reason : "unknown");
    requestQuestQuery(questId, true);

    if (npcGuid != 0) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.socket->send(qsPkt);

        auto queryPkt = owner_.packetParsers_
            ? owner_.packetParsers_->buildQueryQuestPacket(npcGuid, questId)
            : QuestgiverQueryQuestPacket::build(npcGuid, questId);
        owner_.socket->send(queryPkt);
    }
}

// ---------------------------------------------------------------------------
// Packet handlers
// ---------------------------------------------------------------------------

void QuestHandler::handleGossipMessage(network::Packet& packet) {
    bool ok = owner_.packetParsers_ ? owner_.packetParsers_->parseGossipMessage(packet, currentGossip_)
                                    : GossipMessageParser::parse(packet, currentGossip_);
    if (!ok) return;
    if (questDetailsOpen_) return; // Don't reopen gossip while viewing quest
    gossipWindowOpen_ = true;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GOSSIP_SHOW", {});
    owner_.closeVendor(); // Close vendor if gossip opens

    // Classify gossip quests and update quest log + overhead NPC markers.
    classifyGossipQuests(true);

    // Play NPC greeting voice
    if (owner_.npcGreetingCallback_ && currentGossip_.npcGuid != 0) {
        auto entity = owner_.getEntityManager().getEntity(currentGossip_.npcGuid);
        if (entity) {
            glm::vec3 npcPos(entity->getX(), entity->getY(), entity->getZ());
            owner_.npcGreetingCallback_(currentGossip_.npcGuid, npcPos);
        }
    }
}

void QuestHandler::handleQuestgiverQuestList(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;

    GossipMessageData data;
    data.npcGuid = packet.readUInt64();
    data.menuId = 0;
    data.titleTextId = 0;

    std::string header = packet.readString();
    if (packet.hasRemaining(8)) {
        (void)packet.readUInt32(); // emoteDelay / unk
        (void)packet.readUInt32(); // emote / unk
    }
    (void)header;

    // questCount is uint8 in all WoW versions for SMSG_QUESTGIVER_QUEST_LIST.
    uint32_t questCount = 0;
    if (packet.hasRemaining(1)) {
        questCount = packet.readUInt8();
    }

    const bool hasQuestFlagsField = !isClassicLikeExpansion() && !isActiveExpansion("tbc");

    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        GossipQuestItem q;
        q.questId = packet.readUInt32();
        q.questIcon = packet.readUInt32();
        q.questLevel = static_cast<int32_t>(packet.readUInt32());

        if (hasQuestFlagsField && packet.hasRemaining(5)) {
            q.questFlags = packet.readUInt32();
            q.isRepeatable = packet.readUInt8();
        } else {
            q.questFlags = 0;
            q.isRepeatable = 0;
        }
        q.title = normalizeWowTextTokens(packet.readString());
        if (q.questId != 0) {
            data.quests.push_back(std::move(q));
        }
    }

    currentGossip_ = std::move(data);
    gossipWindowOpen_ = true;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GOSSIP_SHOW", {});
    owner_.closeVendor();

    classifyGossipQuests(false);

    LOG_INFO("Questgiver quest list: npc=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " quests=", currentGossip_.quests.size());
}

// Shared quest-icon classification for gossip windows. Derives NPC quest status
// from icon values so overhead markers stay aligned with what the NPC offers.
// updateQuestLog: if true, also patches quest log completion state (gossip handler
// does this because it has the freshest data; quest-list handler skips it because
// completion updates arrive via separate packets).
void QuestHandler::classifyGossipQuests(bool updateQuestLog) {
    // Icon values come from the server's QUEST_STATUS enum, not a client constant,
    // so these magic numbers are protocol-defined and stable across expansions.
    auto isCompletable = [](uint32_t icon) { return icon == 5 || icon == 6 || icon == 10; };
    auto isIncomplete  = [](uint32_t icon) { return icon == 3 || icon == 4; };
    auto isAvailable   = [](uint32_t icon) { return icon == 2 || icon == 7 || icon == 8; };

    bool hasAvailable = false, hasReward = false, hasIncomplete = false;
    for (const auto& q : currentGossip_.quests) {
        bool completable = isCompletable(q.questIcon);
        bool incomplete  = isIncomplete(q.questIcon);
        bool available   = isAvailable(q.questIcon);
        hasAvailable |= available;
        hasReward |= completable;
        hasIncomplete |= incomplete;

        if (updateQuestLog) {
            for (auto& entry : questLog_) {
                if (entry.questId == q.questId) {
                    entry.complete = completable;
                    entry.title = q.title;
                    break;
                }
            }
        }
    }
    if (currentGossip_.npcGuid != 0) {
        QuestGiverStatus status = QuestGiverStatus::NONE;
        if (hasReward) status = QuestGiverStatus::REWARD;
        else if (hasAvailable) status = QuestGiverStatus::AVAILABLE;
        else if (hasIncomplete) status = QuestGiverStatus::INCOMPLETE;
        if (status != QuestGiverStatus::NONE)
            npcQuestStatus_[currentGossip_.npcGuid] = status;
    }
}

void QuestHandler::handleGossipComplete(network::Packet& packet) {
    (void)packet;

    // Play farewell sound before closing
    if (owner_.npcFarewellCallback_ && currentGossip_.npcGuid != 0) {
        auto entity = owner_.getEntityManager().getEntity(currentGossip_.npcGuid);
        if (entity && entity->getType() == ObjectType::UNIT) {
            glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
            owner_.npcFarewellCallback_(currentGossip_.npcGuid, pos);
        }
    }

    gossipWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
}

void QuestHandler::handleQuestPoiQueryResponse(network::Packet& packet) {
    // WotLK 3.3.5a SMSG_QUEST_POI_QUERY_RESPONSE format:
    //   uint32 questCount
    //   per quest:
    //     uint32 questId
    //     uint32 poiCount
    //     per poi:
    //       uint32 poiId
    //       int32  objIndex      (-1 = no specific objective)
    //       uint32 mapId
    //       uint32 areaId
    //       uint32 floorId
    //       uint32 unk1
    //       uint32 unk2
    //       uint32 pointCount
    //       per point: int32 x, int32 y
    if (!packet.hasRemaining(4)) return;
    const uint32_t questCount = packet.readUInt32();
    for (uint32_t qi = 0; qi < questCount; ++qi) {
        if (!packet.hasRemaining(8)) return;
        const uint32_t questId  = packet.readUInt32();
        const uint32_t poiCount = packet.readUInt32();

        // Remove any previously added POI markers for this quest
        gossipPois_.erase(
            std::remove_if(gossipPois_.begin(), gossipPois_.end(),
                [questId](const GossipPoi& p) {
                    return p.data == questId;
                }),
            gossipPois_.end());

        // Find the quest title for the marker label.
        std::string questTitle;
        for (const auto& q : questLog_) {
            if (q.questId == questId) { questTitle = q.title; break; }
        }

        for (uint32_t pi = 0; pi < poiCount; ++pi) {
            if (!packet.hasRemaining(28)) return;
            packet.readUInt32();  // poiId
            packet.readUInt32();  // objIndex (int32)
            const uint32_t mapId    = packet.readUInt32();
            packet.readUInt32();  // areaId
            packet.readUInt32();  // floorId
            packet.readUInt32();  // unk1
            packet.readUInt32();  // unk2
            const uint32_t pointCount = packet.readUInt32();
            if (pointCount == 0) continue;
            if (packet.getRemainingSize() < pointCount * 8) return;
            float sumX = 0.0f, sumY = 0.0f;
            for (uint32_t pt = 0; pt < pointCount; ++pt) {
                const int32_t px = static_cast<int32_t>(packet.readUInt32());
                const int32_t py = static_cast<int32_t>(packet.readUInt32());
                sumX += static_cast<float>(px);
                sumY += static_cast<float>(py);
            }
            // Skip POIs for maps other than the player's current map.
            if (mapId != owner_.currentMapId_) continue;
            GossipPoi poi;
            poi.x    = sumX / static_cast<float>(pointCount);
            poi.y    = sumY / static_cast<float>(pointCount);
            poi.icon = 6;  // generic quest POI icon
            poi.data = questId;
            poi.name = questTitle.empty() ? "Quest objective" : questTitle;
            LOG_DEBUG("Quest POI: questId=", questId, " mapId=", mapId,
                      " centroid=(", poi.x, ",", poi.y, ") title=", poi.name);
            if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
            gossipPois_.push_back(std::move(poi));
        }
    }
}

void QuestHandler::handleQuestDetails(network::Packet& packet) {
    QuestDetailsData data;
    bool ok = owner_.packetParsers_ ? owner_.packetParsers_->parseQuestDetails(packet, data)
                                    : QuestDetailsParser::parse(packet, data);
    if (!ok) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_QUEST_DETAILS");
        return;
    }
    currentQuestDetails_ = data;
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        if (!data.title.empty() && (isPlaceholderQuestTitle(q.title) || data.title.size() >= q.title.size())) {
            q.title = data.title;
        }
        if (!data.objectives.empty() && (q.objectives.empty() || data.objectives.size() > q.objectives.size())) {
            q.objectives = data.objectives;
        }
        break;
    }
    // Pre-fetch item info for all reward items
    for (const auto& item : data.rewardChoiceItems) owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.rewardItems)       owner_.queryItemInfo(item.itemId, 0);
    // Delay opening the window slightly to allow item queries to complete
    questDetailsOpenTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    gossipWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("QUEST_DETAIL", {});
}

void QuestHandler::handleQuestRequestItems(network::Packet& packet) {
    QuestRequestItemsData data;
    if (!QuestRequestItemsParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_REQUEST_ITEMS");
        return;
    }
    clearPendingQuestAccept(data.questId);

    if (pendingTurnInRewardRequest_ &&
        data.questId == pendingTurnInQuestId_ &&
        data.npcGuid == pendingTurnInNpcGuid_ &&
        data.isCompletable() &&
        owner_.socket) {
        auto rewardReq = QuestgiverRequestRewardPacket::build(data.npcGuid, data.questId);
        owner_.socket->send(rewardReq);
        pendingTurnInRewardRequest_ = false;
    }

    currentQuestRequestItems_ = data;
    questRequestItemsOpen_ = true;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};

    // Query item names for required items
    for (const auto& item : data.requiredItems) {
        owner_.queryItemInfo(item.itemId, 0);
    }

    // Server-authoritative turn-in requirements
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        q.complete = data.isCompletable();
        q.requiredItemCounts.clear();

        std::ostringstream oss;
        if (!data.completionText.empty()) {
            oss << data.completionText;
            if (!data.requiredItems.empty() || data.requiredMoney > 0) oss << "\n\n";
        }
        if (!data.requiredItems.empty()) {
            oss << "Required items:";
            for (const auto& item : data.requiredItems) {
                std::string itemLabel = "Item " + std::to_string(item.itemId);
                if (const auto* info = owner_.getItemInfo(item.itemId)) {
                    if (!info->name.empty()) itemLabel = info->name;
                }
                q.requiredItemCounts[item.itemId] = item.count;
                oss << "\n- " << itemLabel << " x" << item.count;
            }
        }
        if (data.requiredMoney > 0) {
            if (!data.requiredItems.empty()) oss << "\n";
            oss << "\nRequired money: " << formatCopperAmount(data.requiredMoney);
        }
        q.objectives = oss.str();
        break;
    }
}

void QuestHandler::handleQuestOfferReward(network::Packet& packet) {
    QuestOfferRewardData data;
    if (!QuestOfferRewardParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_OFFER_REWARD");
        return;
    }
    clearPendingQuestAccept(data.questId);
    LOG_INFO("Quest offer reward: questId=", data.questId, " title=\"", data.title, "\"");
    if (pendingTurnInQuestId_ == data.questId) {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
    }
    currentQuestOfferReward_ = data;
    questOfferRewardOpen_ = true;
    questRequestItemsOpen_ = false;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("QUEST_COMPLETE", {});

    // Query item names for reward items
    for (const auto& item : data.choiceRewards)
        owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.fixedRewards)
        owner_.queryItemInfo(item.itemId, 0);
}

void QuestHandler::handleQuestConfirmAccept(network::Packet& packet) {
    size_t rem = packet.getRemainingSize();
    if (rem < 4) return;

    sharedQuestId_    = packet.readUInt32();
    sharedQuestTitle_ = packet.readString();
    if (packet.hasRemaining(8)) {
        sharedQuestSharerGuid_ = packet.readUInt64();
    }

    sharedQuestSharerName_.clear();
    auto entity = owner_.getEntityManager().getEntity(sharedQuestSharerGuid_);
    if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
        sharedQuestSharerName_ = unit->getName();
    }
    if (sharedQuestSharerName_.empty()) {
        auto nit = owner_.getPlayerNameCache().find(sharedQuestSharerGuid_);
        if (nit != owner_.getPlayerNameCache().end())
            sharedQuestSharerName_ = nit->second;
    }
    if (sharedQuestSharerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(sharedQuestSharerGuid_));
        sharedQuestSharerName_ = tmp;
    }

    pendingSharedQuest_ = true;
    owner_.addSystemChatMessage(sharedQuestSharerName_ + " has shared the quest \"" +
                                sharedQuestTitle_ + "\" with you.");
    LOG_INFO("SMSG_QUEST_CONFIRM_ACCEPT: questId=", sharedQuestId_,
             " title=", sharedQuestTitle_, " sharer=", sharedQuestSharerName_);
}

} // namespace game
} // namespace wowee
