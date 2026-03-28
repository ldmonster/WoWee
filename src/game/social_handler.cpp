#include "game/social_handler.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "audio/ui_sound_manager.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace wowee {
namespace game {

// Free function defined in game_handler.cpp
std::string buildItemLink(uint32_t itemId, uint32_t quality, const std::string& name);

static bool packetHasRemaining(const network::Packet& packet, size_t need) {
    const size_t size = packet.getSize();
    const size_t pos = packet.getReadPos();
    return pos <= size && need <= (size - pos);
}

static const char* lfgJoinResultString(uint8_t result) {
    switch (result) {
        case 0:  return nullptr;
        case 1:  return "Role check failed.";
        case 2:  return "No LFG slots available for your group.";
        case 3:  return "No LFG object found.";
        case 4:  return "No slots available (player).";
        case 5:  return "No slots available (party).";
        case 6:  return "Dungeon requirements not met by all members.";
        case 7:  return "Party members are from different realms.";
        case 8:  return "Not all members are present.";
        case 9:  return "Get info timeout.";
        case 10: return "Invalid dungeon slot.";
        case 11: return "You are marked as a deserter.";
        case 12: return "A party member is marked as a deserter.";
        case 13: return "You are on a random dungeon cooldown.";
        case 14: return "A party member is on a random dungeon cooldown.";
        case 16: return "No spec/role available.";
        default: return "Cannot join dungeon finder.";
    }
}

static const char* lfgTeleportDeniedString(uint8_t reason) {
    switch (reason) {
        case 0:  return "You are not in a LFG group.";
        case 1:  return "You are not in the dungeon.";
        case 2:  return "You have a summon pending.";
        case 3:  return "You are dead.";
        case 4:  return "You have Deserter.";
        case 5:  return "You do not meet the requirements.";
        default: return "Teleport to dungeon denied.";
    }
}

static const std::string kEmptyString;

SocialHandler::SocialHandler(GameHandler& owner)
    : owner_(owner) {}

// ============================================================
// registerOpcodes
// ============================================================

void SocialHandler::registerOpcodes(DispatchTable& table) {
    // ---- Player info queries / social ----
    table[Opcode::SMSG_QUERY_TIME_RESPONSE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleQueryTimeResponse(packet);
    };
    table[Opcode::SMSG_PLAYED_TIME] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handlePlayedTime(packet);
    };
    table[Opcode::SMSG_WHO] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleWho(packet);
    };
    table[Opcode::SMSG_WHOIS] = [this](network::Packet& packet) {
        if (packet.getReadPos() < packet.getSize()) {
            std::string whoisText = packet.readString();
            if (!whoisText.empty()) {
                std::string line;
                for (char c : whoisText) {
                    if (c == '\n') { if (!line.empty()) owner_.addSystemChatMessage("[Whois] " + line); line.clear(); }
                    else line += c;
                }
                if (!line.empty()) owner_.addSystemChatMessage("[Whois] " + line);
                LOG_INFO("SMSG_WHOIS: ", whoisText);
            }
        }
    };
    table[Opcode::SMSG_FRIEND_STATUS] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleFriendStatus(packet);
    };
    table[Opcode::SMSG_CONTACT_LIST] = [this](network::Packet& packet) { handleContactList(packet); };
    table[Opcode::SMSG_FRIEND_LIST] = [this](network::Packet& packet) { handleFriendList(packet); };
    table[Opcode::SMSG_IGNORE_LIST] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 1) return;
        uint8_t ignCount = packet.readUInt8();
        for (uint8_t i = 0; i < ignCount; ++i) {
            if (packet.getSize() - packet.getReadPos() < 8) break;
            uint64_t ignGuid = packet.readUInt64();
            std::string ignName = packet.readString();
            if (!ignName.empty() && ignGuid != 0) owner_.ignoreCache[ignName] = ignGuid;
        }
        LOG_DEBUG("SMSG_IGNORE_LIST: loaded ", (int)ignCount, " ignored players");
    };
    table[Opcode::MSG_RANDOM_ROLL] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleRandomRoll(packet);
    };

    // ---- Logout ----
    table[Opcode::SMSG_LOGOUT_RESPONSE] = [this](network::Packet& packet) { handleLogoutResponse(packet); };
    table[Opcode::SMSG_LOGOUT_COMPLETE] = [this](network::Packet& packet) { handleLogoutComplete(packet); };

    // ---- Inspect ----
    table[Opcode::SMSG_INSPECT_TALENT] = [this](network::Packet& packet) { handleInspectResults(packet); };
    table[Opcode::SMSG_INSPECT_RESULTS_UPDATE] = [this](network::Packet& packet) { handleInspectResults(packet); };

    // ---- Group ----
    table[Opcode::SMSG_GROUP_INVITE] = [this](network::Packet& packet) { handleGroupInvite(packet); };
    table[Opcode::SMSG_GROUP_DECLINE] = [this](network::Packet& packet) { handleGroupDecline(packet); };
    table[Opcode::SMSG_GROUP_LIST] = [this](network::Packet& packet) { handleGroupList(packet); };
    table[Opcode::SMSG_GROUP_DESTROYED] = [this](network::Packet& /*packet*/) {
        partyData.members.clear();
        partyData.memberCount = 0;
        partyData.leaderGuid = 0;
        owner_.addUIError("Your party has been disbanded.");
        owner_.addSystemChatMessage("Your party has been disbanded.");
        if (owner_.addonEventCallback_) {
            owner_.addonEventCallback_("GROUP_ROSTER_UPDATE", {});
            owner_.addonEventCallback_("PARTY_MEMBERS_CHANGED", {});
        }
    };
    table[Opcode::SMSG_GROUP_CANCEL] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("Group invite cancelled.");
    };
    table[Opcode::SMSG_GROUP_UNINVITE] = [this](network::Packet& packet) { handleGroupUninvite(packet); };
    table[Opcode::SMSG_PARTY_COMMAND_RESULT] = [this](network::Packet& packet) { handlePartyCommandResult(packet); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS] = [this](network::Packet& packet) { handlePartyMemberStats(packet, false); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS_FULL] = [this](network::Packet& packet) { handlePartyMemberStats(packet, true); };

    // ---- Ready check ----
    table[Opcode::MSG_RAID_READY_CHECK] = [this](network::Packet& packet) {
        pendingReadyCheck_ = true;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckInitiator_.clear();
        readyCheckResults_.clear();
        if (packet.getSize() - packet.getReadPos() >= 8) {
            uint64_t initiatorGuid = packet.readUInt64();
            auto entity = owner_.entityManager.getEntity(initiatorGuid);
            if (auto* unit = dynamic_cast<Unit*>(entity.get()))
                readyCheckInitiator_ = unit->getName();
        }
        if (readyCheckInitiator_.empty() && partyData.leaderGuid != 0) {
            for (const auto& member : partyData.members) {
                if (member.guid == partyData.leaderGuid) { readyCheckInitiator_ = member.name; break; }
            }
        }
        owner_.addSystemChatMessage(readyCheckInitiator_.empty()
            ? "Ready check initiated!"
            : readyCheckInitiator_ + " initiated a ready check!");
        if (owner_.addonEventCallback_)
            owner_.addonEventCallback_("READY_CHECK", {readyCheckInitiator_});
    };
    table[Opcode::MSG_RAID_READY_CHECK_CONFIRM] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 9) { packet.setReadPos(packet.getSize()); return; }
        uint64_t respGuid = packet.readUInt64();
        uint8_t  isReady  = packet.readUInt8();
        if (isReady) ++readyCheckReadyCount_; else ++readyCheckNotReadyCount_;
        auto nit = owner_.playerNameCache.find(respGuid);
        std::string rname;
        if (nit != owner_.playerNameCache.end()) rname = nit->second;
        else {
            auto ent = owner_.entityManager.getEntity(respGuid);
            if (ent) rname = std::static_pointer_cast<game::Unit>(ent)->getName();
        }
        if (!rname.empty()) {
            bool found = false;
            for (auto& r : readyCheckResults_) {
                if (r.name == rname) { r.ready = (isReady != 0); found = true; break; }
            }
            if (!found) readyCheckResults_.push_back({ rname, isReady != 0 });
            char rbuf[128];
            std::snprintf(rbuf, sizeof(rbuf), "%s is %s.", rname.c_str(), isReady ? "Ready" : "Not Ready");
            owner_.addSystemChatMessage(rbuf);
        }
        if (owner_.addonEventCallback_) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)respGuid);
            owner_.addonEventCallback_("READY_CHECK_CONFIRM", {guidBuf, isReady ? "1" : "0"});
        }
    };
    table[Opcode::MSG_RAID_READY_CHECK_FINISHED] = [this](network::Packet& /*packet*/) {
        char fbuf[128];
        std::snprintf(fbuf, sizeof(fbuf), "Ready check complete: %u ready, %u not ready.",
                     readyCheckReadyCount_, readyCheckNotReadyCount_);
        owner_.addSystemChatMessage(fbuf);
        pendingReadyCheck_ = false;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckResults_.clear();
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("READY_CHECK_FINISHED", {});
    };
    table[Opcode::SMSG_RAID_INSTANCE_INFO] = [this](network::Packet& packet) { handleRaidInstanceInfo(packet); };

    // ---- Duels ----
    table[Opcode::SMSG_DUEL_REQUESTED] = [this](network::Packet& packet) { handleDuelRequested(packet); };
    table[Opcode::SMSG_DUEL_COMPLETE] = [this](network::Packet& packet) { handleDuelComplete(packet); };
    table[Opcode::SMSG_DUEL_WINNER] = [this](network::Packet& packet) { handleDuelWinner(packet); };
    table[Opcode::SMSG_DUEL_OUTOFBOUNDS] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You are out of the duel area!");
        owner_.addSystemChatMessage("You are out of the duel area!");
    };
    table[Opcode::SMSG_DUEL_INBOUNDS] = [this](network::Packet& /*packet*/) {};
    table[Opcode::SMSG_DUEL_COUNTDOWN] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 4) {
            uint32_t ms = packet.readUInt32();
            duelCountdownMs_ = (ms > 0 && ms <= 30000) ? ms : 3000;
            duelCountdownStartedAt_ = std::chrono::steady_clock::now();
        }
    };
    table[Opcode::SMSG_PARTYKILLLOG] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 16) return;
        uint64_t killerGuid = packet.readUInt64();
        uint64_t victimGuid = packet.readUInt64();
        auto nameFor = [this](uint64_t g) -> std::string {
            auto nit = owner_.playerNameCache.find(g);
            if (nit != owner_.playerNameCache.end()) return nit->second;
            auto ent = owner_.entityManager.getEntity(g);
            if (ent && (ent->getType() == game::ObjectType::UNIT ||
                        ent->getType() == game::ObjectType::PLAYER))
                return std::static_pointer_cast<game::Unit>(ent)->getName();
            return {};
        };
        std::string killerName = nameFor(killerGuid);
        std::string victimName = nameFor(victimGuid);
        if (!killerName.empty() && !victimName.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s killed %s.", killerName.c_str(), victimName.c_str());
            owner_.addSystemChatMessage(buf);
        }
    };

    // ---- Guild ----
    table[Opcode::SMSG_GUILD_INFO] = [this](network::Packet& packet) { handleGuildInfo(packet); };
    table[Opcode::SMSG_GUILD_ROSTER] = [this](network::Packet& packet) { handleGuildRoster(packet); };
    table[Opcode::SMSG_GUILD_QUERY_RESPONSE] = [this](network::Packet& packet) { handleGuildQueryResponse(packet); };
    table[Opcode::SMSG_GUILD_EVENT] = [this](network::Packet& packet) { handleGuildEvent(packet); };
    table[Opcode::SMSG_GUILD_INVITE] = [this](network::Packet& packet) { handleGuildInvite(packet); };
    table[Opcode::SMSG_GUILD_COMMAND_RESULT] = [this](network::Packet& packet) { handleGuildCommandResult(packet); };
    table[Opcode::SMSG_PETITION_SHOWLIST] = [this](network::Packet& packet) { handlePetitionShowlist(packet); };
    table[Opcode::SMSG_TURN_IN_PETITION_RESULTS] = [this](network::Packet& packet) { handleTurnInPetitionResults(packet); };
    table[Opcode::SMSG_OFFER_PETITION_ERROR] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 4) {
            uint32_t err = packet.readUInt32();
            if (err == 1) owner_.addSystemChatMessage("Player is already in a guild.");
            else if (err == 2) owner_.addSystemChatMessage("Player already has a petition.");
            else owner_.addSystemChatMessage("Cannot offer petition to that player.");
        }
    };
    table[Opcode::SMSG_PETITION_QUERY_RESPONSE] = [this](network::Packet& packet) { handlePetitionQueryResponse(packet); };
    table[Opcode::SMSG_PETITION_SHOW_SIGNATURES] = [this](network::Packet& packet) { handlePetitionShowSignatures(packet); };
    table[Opcode::SMSG_PETITION_SIGN_RESULTS] = [this](network::Packet& packet) { handlePetitionSignResults(packet); };

    // ---- Battlefield / BG ----
    table[Opcode::SMSG_BATTLEFIELD_STATUS] = [this](network::Packet& packet) { handleBattlefieldStatus(packet); };
    table[Opcode::SMSG_BATTLEFIELD_LIST] = [this](network::Packet& packet) { handleBattlefieldList(packet); };
    table[Opcode::SMSG_BATTLEFIELD_PORT_DENIED] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("Battlefield port denied.");
        owner_.addSystemChatMessage("Battlefield port denied.");
    };
    table[Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS] = [this](network::Packet& packet) {
        bgPlayerPositions_.clear();
        for (int grp = 0; grp < 2; ++grp) {
            if (packet.getSize() - packet.getReadPos() < 4) break;
            uint32_t count = packet.readUInt32();
            for (uint32_t i = 0; i < count && packet.getSize() - packet.getReadPos() >= 16; ++i) {
                BgPlayerPosition pos;
                pos.guid = packet.readUInt64();
                pos.wowX = packet.readFloat();
                pos.wowY = packet.readFloat();
                pos.group = grp;
                bgPlayerPositions_.push_back(pos);
            }
        }
    };
    table[Opcode::SMSG_REMOVED_FROM_PVP_QUEUE] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("You have been removed from the PvP queue.");
    };
    table[Opcode::SMSG_GROUP_JOINED_BATTLEGROUND] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("Your group has joined the battleground.");
    };
    table[Opcode::SMSG_JOINED_BATTLEGROUND_QUEUE] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("You have joined the battleground queue.");
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 8) {
            uint64_t guid = packet.readUInt64();
            auto it = owner_.playerNameCache.find(guid);
            if (it != owner_.playerNameCache.end() && !it->second.empty())
                owner_.addSystemChatMessage(it->second + " has entered the battleground.");
        }
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 8) {
            uint64_t guid = packet.readUInt64();
            auto it = owner_.playerNameCache.find(guid);
            if (it != owner_.playerNameCache.end() && !it->second.empty())
                owner_.addSystemChatMessage(it->second + " has left the battleground.");
        }
    };

    // ---- Instance ----
    for (auto op : { Opcode::SMSG_INSTANCE_DIFFICULTY, Opcode::MSG_SET_DUNGEON_DIFFICULTY }) {
        table[op] = [this](network::Packet& packet) { handleInstanceDifficulty(packet); };

    // ---- Guild / RAF / PvP AFK (moved from GameHandler) ----
    table[Opcode::SMSG_GUILD_DECLINE] = [this](network::Packet& packet) {
        if (packet.hasData()) {
            std::string name = packet.readString();
            owner_.addSystemChatMessage(name + " declined your guild invitation.");
        }
    };
    table[Opcode::SMSG_REFER_A_FRIEND_EXPIRED] = [this](network::Packet& packet) {
        owner_.addSystemChatMessage("Your Recruit-A-Friend link has expired.");
        packet.skipAll();
    };
    table[Opcode::SMSG_REFER_A_FRIEND_FAILURE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t reason = packet.readUInt32();
            static const char* kRafErrors[] = {
                "Not eligible",            // 0
                "Target not eligible",     // 1
                "Too many referrals",      // 2
                "Wrong faction",           // 3
                "Not a recruit",           // 4
                "Recruit requirements not met", // 5
                "Level above requirement", // 6
                "Friend needs account upgrade", // 7
            };
            const char* msg = (reason < 8) ? kRafErrors[reason]
                                           : "Recruit-A-Friend failed.";
            owner_.addSystemChatMessage(std::string("Recruit-A-Friend: ") + msg);
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_REPORT_PVP_AFK_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t result = packet.readUInt8();
            if (result == 0)
                owner_.addSystemChatMessage("AFK report submitted.");
            else
                owner_.addSystemChatMessage("Cannot report that player as AFK right now.");
        }
        packet.skipAll();
    };
    }
    table[Opcode::SMSG_INSTANCE_SAVE_CREATED] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("You are now saved to this instance.");
    };
    table[Opcode::SMSG_RAID_INSTANCE_MESSAGE] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 12) return;
        uint32_t msgType = packet.readUInt32();
        uint32_t mapId   = packet.readUInt32();
        packet.readUInt32(); // diff
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        if (msgType == 1 && packet.getSize() - packet.getReadPos() >= 4) {
            uint32_t timeLeft = packet.readUInt32();
            owner_.addSystemChatMessage(mapLabel + " will reset in " + std::to_string(timeLeft / 60) + " minute(s).");
        } else if (msgType == 2) {
            owner_.addSystemChatMessage("You have been saved to " + mapLabel + ".");
        } else if (msgType == 3) {
            owner_.addSystemChatMessage("Welcome to " + mapLabel + ".");
        }
    };
    table[Opcode::SMSG_INSTANCE_RESET] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 4) return;
        uint32_t mapId = packet.readUInt32();
        auto it = std::remove_if(instanceLockouts_.begin(), instanceLockouts_.end(),
            [mapId](const InstanceLockout& lo){ return lo.mapId == mapId; });
        instanceLockouts_.erase(it, instanceLockouts_.end());
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        owner_.addSystemChatMessage(mapLabel + " has been reset.");
    };
    table[Opcode::SMSG_INSTANCE_RESET_FAILED] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 8) return;
        uint32_t mapId  = packet.readUInt32();
        uint32_t reason = packet.readUInt32();
        static const char* resetFailReasons[] = {
            "Not max level.", "Offline party members.", "Party members inside.",
            "Party members changing zone.", "Heroic difficulty only."
        };
        const char* reasonMsg = (reason < 5) ? resetFailReasons[reason] : "Unknown reason.";
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = "instance #" + std::to_string(mapId);
        owner_.addUIError("Cannot reset " + mapLabel + ": " + reasonMsg);
        owner_.addSystemChatMessage("Cannot reset " + mapLabel + ": " + reasonMsg);
    };
    table[Opcode::SMSG_INSTANCE_LOCK_WARNING_QUERY] = [this](network::Packet& packet) {
        if (!owner_.socket || packet.getSize() - packet.getReadPos() < 17) return;
        uint32_t ilMapId    = packet.readUInt32();
        uint32_t ilDiff     = packet.readUInt32();
        uint32_t ilTimeLeft = packet.readUInt32();
        packet.readUInt32(); // unk
        uint8_t  ilLocked   = packet.readUInt8();
        std::string ilName = owner_.getMapName(ilMapId);
        if (ilName.empty()) ilName = "instance #" + std::to_string(ilMapId);
        static const char* kDiff[] = {"Normal","Heroic","25-Man","25-Man Heroic"};
        std::string ilMsg = "Entering " + ilName;
        if (ilDiff < 4) ilMsg += std::string(" (") + kDiff[ilDiff] + ")";
        if (ilLocked && ilTimeLeft > 0)
            ilMsg += " — " + std::to_string(ilTimeLeft / 60) + " min remaining.";
        else
            ilMsg += ".";
        owner_.addSystemChatMessage(ilMsg);
        network::Packet resp(wireOpcode(Opcode::CMSG_INSTANCE_LOCK_RESPONSE));
        resp.writeUInt8(1);
        owner_.socket->send(resp);
    };

    // ---- LFG ----
    table[Opcode::SMSG_LFG_JOIN_RESULT] = [this](network::Packet& packet) { handleLfgJoinResult(packet); };
    table[Opcode::SMSG_LFG_QUEUE_STATUS] = [this](network::Packet& packet) { handleLfgQueueStatus(packet); };
    table[Opcode::SMSG_LFG_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_ROLE_CHECK_UPDATE] = [this](network::Packet& packet) { handleLfgRoleCheckUpdate(packet); };
    for (auto op : { Opcode::SMSG_LFG_UPDATE_PLAYER, Opcode::SMSG_LFG_UPDATE_PARTY }) {
        table[op] = [this](network::Packet& packet) { handleLfgUpdatePlayer(packet); };
    }
    table[Opcode::SMSG_LFG_PLAYER_REWARD] = [this](network::Packet& packet) { handleLfgPlayerReward(packet); };
    table[Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgBootProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_TELEPORT_DENIED] = [this](network::Packet& packet) { handleLfgTeleportDenied(packet); };
    table[Opcode::SMSG_LFG_DISABLED] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("The Dungeon Finder is currently disabled.");
    };
    table[Opcode::SMSG_LFG_OFFER_CONTINUE] = [this](network::Packet& /*packet*/) {
        owner_.addSystemChatMessage("Dungeon Finder: You may continue your dungeon.");
    };
    table[Opcode::SMSG_LFG_ROLE_CHOSEN] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 13) { packet.setReadPos(packet.getSize()); return; }
        uint64_t roleGuid = packet.readUInt64();
        uint8_t  ready    = packet.readUInt8();
        uint32_t roles    = packet.readUInt32();
        std::string roleName;
        if (roles & 0x02) roleName += "Tank ";
        if (roles & 0x04) roleName += "Healer ";
        if (roles & 0x08) roleName += "DPS ";
        if (roleName.empty()) roleName = "None";
        std::string pName = "A player";
        if (auto e = owner_.entityManager.getEntity(roleGuid))
            if (auto u = std::dynamic_pointer_cast<Unit>(e))
                pName = u->getName();
        if (ready) owner_.addSystemChatMessage(pName + " has chosen: " + roleName);
        packet.setReadPos(packet.getSize());
    };
    for (auto op : { Opcode::SMSG_LFG_UPDATE_SEARCH, Opcode::SMSG_UPDATE_LFG_LIST,
                     Opcode::SMSG_LFG_PLAYER_INFO, Opcode::SMSG_LFG_PARTY_INFO }) {
        table[op] = [](network::Packet& packet) { packet.setReadPos(packet.getSize()); };
    }
    table[Opcode::SMSG_OPEN_LFG_DUNGEON_FINDER] = [this](network::Packet& packet) {
        packet.setReadPos(packet.getSize());
        if (owner_.openLfgCallback_) owner_.openLfgCallback_();
    };

    // ---- Arena ----
    table[Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT] = [this](network::Packet& packet) { handleArenaTeamCommandResult(packet); };
    table[Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE] = [this](network::Packet& packet) { handleArenaTeamQueryResponse(packet); };
    table[Opcode::SMSG_ARENA_TEAM_ROSTER] = [this](network::Packet& packet) { handleArenaTeamRoster(packet); };
    table[Opcode::SMSG_ARENA_TEAM_INVITE] = [this](network::Packet& packet) { handleArenaTeamInvite(packet); };
    table[Opcode::SMSG_ARENA_TEAM_EVENT] = [this](network::Packet& packet) { handleArenaTeamEvent(packet); };
    table[Opcode::SMSG_ARENA_TEAM_STATS] = [this](network::Packet& packet) { handleArenaTeamStats(packet); };
    table[Opcode::SMSG_ARENA_ERROR] = [this](network::Packet& packet) { handleArenaError(packet); };
    table[Opcode::MSG_PVP_LOG_DATA] = [this](network::Packet& packet) { handlePvpLogData(packet); };

    // ---- Factions / group leader ----
    table[Opcode::SMSG_INITIALIZE_FACTIONS] = [this](network::Packet& p) { handleInitializeFactions(p); };
    table[Opcode::SMSG_SET_FACTION_STANDING] = [this](network::Packet& p) { handleSetFactionStanding(p); };
    table[Opcode::SMSG_SET_FACTION_ATWAR] = [this](network::Packet& p) { handleSetFactionAtWar(p); };
    table[Opcode::SMSG_SET_FACTION_VISIBLE] = [this](network::Packet& p) { handleSetFactionVisible(p); };
    table[Opcode::SMSG_GROUP_SET_LEADER] = [this](network::Packet& p) { handleGroupSetLeader(p); };
}

// ============================================================
// Non-inline accessors requiring GameHandler
// ============================================================

std::string SocialHandler::getWhoAreaName(uint32_t zoneId) const {
    return owner_.getAreaName(zoneId);
}

std::string SocialHandler::getCurrentLfgDungeonName() const {
    return owner_.getLfgDungeonName(lfgDungeonId_);
}

bool SocialHandler::isInGuild() const {
    if (!guildName_.empty()) return true;
    const Character* ch = owner_.getActiveCharacter();
    return ch && ch->hasGuild();
}

uint32_t SocialHandler::getEntityGuildId(uint64_t guid) const {
    auto entity = owner_.entityManager.getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return 0;
    const uint16_t ufUnitEnd = fieldIndex(UF::UNIT_END);
    if (ufUnitEnd == 0xFFFF) return 0;
    return entity->getField(ufUnitEnd + 3);
}

const std::string& SocialHandler::lookupGuildName(uint32_t guildId) {
    if (guildId == 0) return kEmptyString;
    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end()) return it->second;
    if (pendingGuildNameQueries_.insert(guildId).second) {
        queryGuildInfo(guildId);
    }
    return kEmptyString;
}

// ============================================================
// Inspection
// ============================================================

void SocialHandler::inspectTarget() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) {
        LOG_WARNING("Cannot inspect: not in world or not connected");
        return;
    }
    if (owner_.targetGuid == 0) {
        owner_.addSystemChatMessage("You must target a player to inspect.");
        return;
    }
    auto target = owner_.getTarget();
    if (!target || target->getType() != ObjectType::PLAYER) {
        owner_.addSystemChatMessage("You can only inspect players.");
        return;
    }
    auto packet = InspectPacket::build(owner_.targetGuid);
    owner_.socket->send(packet);
    if (isActiveExpansion("wotlk")) {
        auto achPkt = QueryInspectAchievementsPacket::build(owner_.targetGuid);
        owner_.socket->send(achPkt);
    }
    auto player = std::static_pointer_cast<Player>(target);
    std::string name = player->getName().empty() ? "Target" : player->getName();
    owner_.addSystemChatMessage("Inspecting " + name + "...");
    LOG_INFO("Sent inspect request for player: ", name, " (GUID: 0x", std::hex, owner_.targetGuid, std::dec, ")");
}

void SocialHandler::handleInspectResults(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t talentType = packet.readUInt8();

    if (talentType == 0) {
        // Own talent info
        if (packet.getSize() - packet.getReadPos() < 6) {
            LOG_DEBUG("SMSG_TALENTS_INFO type=0: too short");
            return;
        }
        uint32_t unspentTalents    = packet.readUInt32();
        uint8_t  talentGroupCount  = packet.readUInt8();
        uint8_t  activeTalentGroup = packet.readUInt8();
        if (activeTalentGroup > 1) activeTalentGroup = 0;
        owner_.activeTalentSpec_ = activeTalentGroup;
        for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
            if (packet.getSize() - packet.getReadPos() < 1) break;
            uint8_t talentCount = packet.readUInt8();
            owner_.learnedTalents_[g].clear();
            for (uint8_t t = 0; t < talentCount; ++t) {
                if (packet.getSize() - packet.getReadPos() < 5) break;
                uint32_t talentId = packet.readUInt32();
                uint8_t  rank     = packet.readUInt8();
                owner_.learnedTalents_[g][talentId] = rank + 1u;
            }
            if (packet.getSize() - packet.getReadPos() < 1) break;
            owner_.learnedGlyphs_[g].fill(0);
            uint8_t glyphCount = packet.readUInt8();
            for (uint8_t gl = 0; gl < glyphCount; ++gl) {
                if (packet.getSize() - packet.getReadPos() < 2) break;
                uint16_t glyphId = packet.readUInt16();
                if (gl < GameHandler::MAX_GLYPH_SLOTS) owner_.learnedGlyphs_[g][gl] = glyphId;
            }
        }
        owner_.unspentTalentPoints_[activeTalentGroup] = static_cast<uint8_t>(
            unspentTalents > 255 ? 255 : unspentTalents);
        if (!owner_.talentsInitialized_) {
            owner_.talentsInitialized_ = true;
            if (unspentTalents > 0) {
                owner_.addSystemChatMessage("You have " + std::to_string(unspentTalents)
                    + " unspent talent point" + (unspentTalents != 1 ? "s" : "") + ".");
            }
        }
        LOG_INFO("SMSG_TALENTS_INFO type=0: unspent=", unspentTalents,
                 " groups=", (int)talentGroupCount, " active=", (int)activeTalentGroup,
                 " learned=", owner_.learnedTalents_[activeTalentGroup].size());
        return;
    }

    // talentType == 1: inspect result
    const bool talentTbc = isClassicLikeExpansion() || isActiveExpansion("tbc");
    if (packet.getSize() - packet.getReadPos() < (talentTbc ? 8u : 2u)) return;
    uint64_t guid = talentTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (guid == 0) return;

    size_t bytesLeft = packet.getSize() - packet.getReadPos();
    if (bytesLeft < 6) {
        LOG_WARNING("SMSG_TALENTS_INFO: too short after guid, ", bytesLeft, " bytes");
        auto entity = owner_.entityManager.getEntity(guid);
        std::string name = "Target";
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) name = player->getName();
        }
        owner_.addSystemChatMessage("Inspecting " + name + " (no talent data available).");
        return;
    }

    uint32_t unspentTalents = packet.readUInt32();
    uint8_t talentGroupCount = packet.readUInt8();
    uint8_t activeTalentGroup = packet.readUInt8();

    auto entity = owner_.entityManager.getEntity(guid);
    std::string playerName = "Target";
    if (entity) {
        auto player = std::dynamic_pointer_cast<Player>(entity);
        if (player && !player->getName().empty()) playerName = player->getName();
    }

    uint32_t totalTalents = 0;
    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        bytesLeft = packet.getSize() - packet.getReadPos();
        if (bytesLeft < 1) break;
        uint8_t talentCount = packet.readUInt8();
        for (uint8_t t = 0; t < talentCount; ++t) {
            bytesLeft = packet.getSize() - packet.getReadPos();
            if (bytesLeft < 5) break;
            packet.readUInt32();
            packet.readUInt8();
            totalTalents++;
        }
        bytesLeft = packet.getSize() - packet.getReadPos();
        if (bytesLeft < 1) break;
        uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount; ++gl) {
            bytesLeft = packet.getSize() - packet.getReadPos();
            if (bytesLeft < 2) break;
            packet.readUInt16();
        }
    }

    std::array<uint16_t, 19> enchantIds{};
    bytesLeft = packet.getSize() - packet.getReadPos();
    if (bytesLeft >= 4) {
        uint32_t slotMask = packet.readUInt32();
        for (int slot = 0; slot < 19; ++slot) {
            if (slotMask & (1u << slot)) {
                bytesLeft = packet.getSize() - packet.getReadPos();
                if (bytesLeft < 2) break;
                enchantIds[slot] = packet.readUInt16();
            }
        }
    }

    inspectResult_.guid              = guid;
    inspectResult_.playerName        = playerName;
    inspectResult_.totalTalents      = totalTalents;
    inspectResult_.unspentTalents    = unspentTalents;
    inspectResult_.talentGroups      = talentGroupCount;
    inspectResult_.activeTalentGroup = activeTalentGroup;
    inspectResult_.enchantIds        = enchantIds;

    auto gearIt = owner_.inspectedPlayerItemEntries_.find(guid);
    if (gearIt != owner_.inspectedPlayerItemEntries_.end()) {
        inspectResult_.itemEntries = gearIt->second;
    } else {
        inspectResult_.itemEntries = {};
    }

    LOG_INFO("Inspect results for ", playerName, ": ", totalTalents, " talents, ",
             unspentTalents, " unspent, ", (int)talentGroupCount, " specs");
    if (owner_.addonEventCallback_) {
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
        owner_.addonEventCallback_("INSPECT_READY", {guidBuf});
    }
}

// ============================================================
// Server Info / Who / Social
// ============================================================

void SocialHandler::queryServerTime() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = QueryTimePacket::build();
    owner_.socket->send(packet);
    LOG_INFO("Requested server time");
}

void SocialHandler::requestPlayedTime() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = RequestPlayedTimePacket::build(true);
    owner_.socket->send(packet);
    LOG_INFO("Requested played time");
}

void SocialHandler::queryWho(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = WhoPacket::build(0, 0, playerName);
    owner_.socket->send(packet);
    LOG_INFO("Sent WHO query", playerName.empty() ? "" : " for: " + playerName);
}

void SocialHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto packet = AddFriendPacket::build(playerName, note);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Sending friend request to " + playerName + "...");
    LOG_INFO("Sent friend request to: ", playerName);
}

void SocialHandler::removeFriend(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto it = owner_.friendsCache.find(playerName);
    if (it == owner_.friendsCache.end()) {
        owner_.addSystemChatMessage(playerName + " is not in your friends list.");
        return;
    }
    auto packet = DelFriendPacket::build(it->second);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Removing " + playerName + " from friends list...");
    LOG_INFO("Sent remove friend request for: ", playerName);
}

void SocialHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto it = owner_.friendsCache.find(playerName);
    if (it == owner_.friendsCache.end()) {
        owner_.addSystemChatMessage(playerName + " is not in your friends list.");
        return;
    }
    auto packet = SetContactNotesPacket::build(it->second, note);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Updated note for " + playerName);
    LOG_INFO("Set friend note for: ", playerName);
}

void SocialHandler::addIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto packet = AddIgnorePacket::build(playerName);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Adding " + playerName + " to ignore list...");
    LOG_INFO("Sent ignore request for: ", playerName);
}

void SocialHandler::removeIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto it = owner_.ignoreCache.find(playerName);
    if (it == owner_.ignoreCache.end()) {
        owner_.addSystemChatMessage(playerName + " is not in your ignore list.");
        return;
    }
    auto packet = DelIgnorePacket::build(it->second);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Removing " + playerName + " from ignore list...");
    owner_.ignoreCache.erase(it);
    LOG_INFO("Sent remove ignore request for: ", playerName);
}

void SocialHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (minRoll > maxRoll) std::swap(minRoll, maxRoll);
    if (maxRoll > 10000) maxRoll = 10000;
    auto packet = RandomRollPacket::build(minRoll, maxRoll);
    owner_.socket->send(packet);
    LOG_INFO("Rolled ", minRoll, "-", maxRoll);
}

// ============================================================
// Logout
// ============================================================

void SocialHandler::requestLogout() {
    if (!owner_.socket) return;
    if (loggingOut_) { owner_.addSystemChatMessage("Already logging out."); return; }
    auto packet = LogoutRequestPacket::build();
    owner_.socket->send(packet);
    loggingOut_ = true;
    LOG_INFO("Sent logout request");
}

void SocialHandler::cancelLogout() {
    if (!owner_.socket) return;
    if (!loggingOut_) { owner_.addSystemChatMessage("Not currently logging out."); return; }
    auto packet = LogoutCancelPacket::build();
    owner_.socket->send(packet);
    loggingOut_ = false;
    logoutCountdown_ = 0.0f;
    owner_.addSystemChatMessage("Logout cancelled.");
    LOG_INFO("Cancelled logout");
}

// ============================================================
// Guild
// ============================================================

void SocialHandler::requestGuildInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildInfoPacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::requestGuildRoster() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildRosterPacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Requesting guild roster...");
}

void SocialHandler::setGuildMotd(const std::string& motd) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildMotdPacket::build(motd);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Guild MOTD updated.");
}

void SocialHandler::promoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto packet = GuildPromotePacket::build(playerName);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Promoting " + playerName + "...");
}

void SocialHandler::demoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto packet = GuildDemotePacket::build(playerName);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Demoting " + playerName + "...");
}

void SocialHandler::leaveGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildLeavePacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Leaving guild...");
}

void SocialHandler::inviteToGuild(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name."); return; }
    auto packet = GuildInvitePacket::build(playerName);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Inviting " + playerName + " to guild...");
}

void SocialHandler::kickGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildRemovePacket::build(playerName);
    owner_.socket->send(packet);
}

void SocialHandler::disbandGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildDisbandPacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::setGuildLeader(const std::string& name) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildLeaderPacket::build(name);
    owner_.socket->send(packet);
}

void SocialHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildSetPublicNotePacket::build(name, note);
    owner_.socket->send(packet);
}

void SocialHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildSetOfficerNotePacket::build(name, note);
    owner_.socket->send(packet);
}

void SocialHandler::acceptGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingGuildInvite_ = false;
    auto packet = GuildAcceptPacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::declineGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingGuildInvite_ = false;
    auto packet = GuildDeclineInvitationPacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::queryGuildInfo(uint32_t guildId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildQueryPacket::build(guildId);
    owner_.socket->send(packet);
}

void SocialHandler::createGuild(const std::string& guildName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildCreatePacket::build(guildName);
    owner_.socket->send(packet);
}

void SocialHandler::addGuildRank(const std::string& rankName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildAddRankPacket::build(rankName);
    owner_.socket->send(packet);
    requestGuildRoster();
}

void SocialHandler::deleteGuildRank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GuildDelRankPacket::build();
    owner_.socket->send(packet);
    requestGuildRoster();
}

void SocialHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = PetitionShowlistPacket::build(npcGuid);
    owner_.socket->send(packet);
}

void SocialHandler::buyPetition(uint64_t npcGuid, const std::string& guildName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = PetitionBuyPacket::build(npcGuid, guildName);
    owner_.socket->send(packet);
}

void SocialHandler::signPetition(uint64_t petitionGuid) {
    if (!owner_.socket || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PETITION_SIGN));
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt8(0);
    owner_.socket->send(pkt);
}

void SocialHandler::turnInPetition(uint64_t petitionGuid) {
    if (!owner_.socket || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_TURN_IN_PETITION));
    pkt.writeUInt64(petitionGuid);
    owner_.socket->send(pkt);
}

// ============================================================
// Ready Check
// ============================================================

void SocialHandler::initiateReadyCheck() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (!isInGroup()) { owner_.addSystemChatMessage("You must be in a group to initiate a ready check."); return; }
    auto packet = ReadyCheckPacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Ready check initiated.");
}

void SocialHandler::respondToReadyCheck(bool ready) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = ReadyCheckConfirmPacket::build(ready);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage(ready ? "You are ready." : "You are not ready.");
}

// ============================================================
// Duel
// ============================================================

void SocialHandler::acceptDuel() {
    if (!pendingDuelRequest_ || owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingDuelRequest_ = false;
    auto pkt = DuelAcceptPacket::build();
    owner_.socket->send(pkt);
    owner_.addSystemChatMessage("You accept the duel.");
}

void SocialHandler::forfeitDuel() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingDuelRequest_ = false;
    auto packet = DuelCancelPacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("You have forfeited the duel.");
}

void SocialHandler::proposeDuel(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (targetGuid == 0) { owner_.addSystemChatMessage("You must target a player to challenge to a duel."); return; }
    auto packet = DuelProposedPacket::build(targetGuid);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("You have challenged your target to a duel.");
}

void SocialHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (targetGuid == 0) {
        owner_.addSystemChatMessage("You must target a player to report.");
        return;
    }
    auto packet = ComplainPacket::build(targetGuid, reason);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Player report submitted.");
    LOG_INFO("Reported player: 0x", std::hex, targetGuid, std::dec, " reason=", reason);
}

void SocialHandler::handleDuelRequested(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 16) { packet.setReadPos(packet.getSize()); return; }
    duelChallengerGuid_ = packet.readUInt64();
    duelFlagGuid_       = packet.readUInt64();
    duelChallengerName_.clear();
    auto entity = owner_.entityManager.getEntity(duelChallengerGuid_);
    if (auto* unit = dynamic_cast<Unit*>(entity.get()))
        duelChallengerName_ = unit->getName();
    if (duelChallengerName_.empty()) {
        auto nit = owner_.playerNameCache.find(duelChallengerGuid_);
        if (nit != owner_.playerNameCache.end()) duelChallengerName_ = nit->second;
    }
    if (duelChallengerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX", static_cast<unsigned long long>(duelChallengerGuid_));
        duelChallengerName_ = tmp;
    }
    pendingDuelRequest_ = true;
    owner_.addSystemChatMessage(duelChallengerName_ + " challenges you to a duel!");
    if (auto* renderer = core::Application::getInstance().getRenderer())
        if (auto* sfx = renderer->getUiSoundManager()) sfx->playTargetSelect();
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("DUEL_REQUESTED", {duelChallengerName_});
}

void SocialHandler::handleDuelComplete(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t started = packet.readUInt8();
    pendingDuelRequest_ = false;
    duelCountdownMs_ = 0;
    if (!started) owner_.addSystemChatMessage("The duel was cancelled.");
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("DUEL_FINISHED", {});
}

void SocialHandler::handleDuelWinner(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 3) return;
    uint8_t duelType = packet.readUInt8();
    std::string winner = packet.readString();
    std::string loser  = packet.readString();
    std::string msg = (duelType == 1)
        ? loser + " has fled from the duel. " + winner + " wins!"
        : winner + " has defeated " + loser + " in a duel!";
    owner_.addSystemChatMessage(msg);
}

// ============================================================
// Party / Raid
// ============================================================

void SocialHandler::inviteToGroup(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GroupInvitePacket::build(playerName);
    owner_.socket->send(packet);
}

void SocialHandler::acceptGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingGroupInvite = false;
    auto packet = GroupAcceptPacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::declineGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    pendingGroupInvite = false;
    auto packet = GroupDeclinePacket::build();
    owner_.socket->send(packet);
}

void SocialHandler::leaveGroup() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GroupDisbandPacket::build();
    owner_.socket->send(packet);
    partyData = GroupListData{};
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallback_("PARTY_MEMBERS_CHANGED", {});
    }
}

void SocialHandler::convertToRaid() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (!isInGroup()) {
        owner_.addSystemChatMessage("You are not in a group.");
        return;
    }
    if (partyData.leaderGuid != owner_.getPlayerGuid()) {
        owner_.addSystemChatMessage("You must be the party leader to convert to raid.");
        return;
    }
    if (partyData.groupType == 1) {
        owner_.addSystemChatMessage("You are already in a raid group.");
        return;
    }
    auto packet = GroupRaidConvertPacket::build();
    owner_.socket->send(packet);
    LOG_INFO("Sent CMSG_GROUP_RAID_CONVERT");
}

void SocialHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = SetLootMethodPacket::build(method, threshold, masterLooterGuid);
    owner_.socket->send(packet);
    LOG_INFO("sendSetLootMethod: method=", method, " threshold=", threshold);
}

void SocialHandler::uninvitePlayer(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (playerName.empty()) { owner_.addSystemChatMessage("You must specify a player name to uninvite."); return; }
    auto packet = GroupUninvitePacket::build(playerName);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Removed " + playerName + " from the group.");
}

void SocialHandler::leaveParty() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GroupDisbandPacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("You have left the group.");
}

void SocialHandler::setMainTank(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (targetGuid == 0) { owner_.addSystemChatMessage("You must have a target selected."); return; }
    auto packet = RaidTargetUpdatePacket::build(0, targetGuid);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Main tank set.");
}

void SocialHandler::setMainAssist(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (targetGuid == 0) { owner_.addSystemChatMessage("You must have a target selected."); return; }
    auto packet = RaidTargetUpdatePacket::build(1, targetGuid);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Main assist set.");
}

void SocialHandler::clearMainTank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = RaidTargetUpdatePacket::build(0, 0);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Main tank cleared.");
}

void SocialHandler::clearMainAssist() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = RaidTargetUpdatePacket::build(1, 0);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Main assist cleared.");
}

void SocialHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    if (icon == 0xFF) {
        for (int i = 0; i < 8; ++i) {
            if (raidTargetGuids_[i] == guid) {
                auto packet = RaidTargetUpdatePacket::build(static_cast<uint8_t>(i), 0);
                owner_.socket->send(packet);
                break;
            }
        }
    } else if (icon < 8) {
        auto packet = RaidTargetUpdatePacket::build(icon, guid);
        owner_.socket->send(packet);
    }
}

void SocialHandler::requestRaidInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = RequestRaidInfoPacket::build();
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Requesting raid lockout information...");
}

// ============================================================
// Group Handlers
// ============================================================

void SocialHandler::handleGroupInvite(network::Packet& packet) {
    GroupInviteResponseData data;
    if (!GroupInviteResponseParser::parse(packet, data)) return;
    pendingGroupInvite = true;
    pendingInviterName = data.inviterName;
    if (!data.inviterName.empty())
        owner_.addSystemChatMessage(data.inviterName + " has invited you to a group.");
    if (auto* renderer = core::Application::getInstance().getRenderer())
        if (auto* sfx = renderer->getUiSoundManager()) sfx->playTargetSelect();
    if (owner_.addonEventCallback_)
        owner_.addonEventCallback_("PARTY_INVITE_REQUEST", {data.inviterName});
}

void SocialHandler::handleGroupDecline(network::Packet& packet) {
    GroupDeclineData data;
    if (!GroupDeclineResponseParser::parse(packet, data)) return;
    owner_.addSystemChatMessage(data.playerName + " has declined your group invitation.");
}

void SocialHandler::handleGroupList(network::Packet& packet) {
    const bool hasRoles = isActiveExpansion("wotlk");
    const uint8_t prevLootMethod = partyData.lootMethod;
    const bool wasInGroup = !partyData.isEmpty();
    partyData = GroupListData{};
    if (!GroupListParser::parse(packet, partyData, hasRoles)) return;

    const bool nowInGroup = !partyData.isEmpty();
    if (!nowInGroup && wasInGroup) {
        owner_.addSystemChatMessage("You are no longer in a group.");
    } else if (nowInGroup && !wasInGroup) {
        owner_.addSystemChatMessage("You are now in a group.");
    }
    // Loot method change notification
    if (wasInGroup && nowInGroup && partyData.lootMethod != prevLootMethod) {
        static const char* kLootMethods[] = {
            "Free for All", "Round Robin", "Master Looter", "Group Loot", "Need Before Greed"
        };
        const char* methodName = (partyData.lootMethod < 5) ? kLootMethods[partyData.lootMethod] : "Unknown";
        owner_.addSystemChatMessage(std::string("Loot method changed to ") + methodName + ".");
    }
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallback_("PARTY_MEMBERS_CHANGED", {});
        if (partyData.groupType == 1)
            owner_.addonEventCallback_("RAID_ROSTER_UPDATE", {});
    }
}

void SocialHandler::handleGroupUninvite(network::Packet& packet) {
    (void)packet;
    partyData = GroupListData{};
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallback_("PARTY_MEMBERS_CHANGED", {});
        owner_.addonEventCallback_("RAID_ROSTER_UPDATE", {});
    }
    owner_.addUIError("You have been removed from the group.");
    owner_.addSystemChatMessage("You have been removed from the group.");
}

void SocialHandler::handlePartyCommandResult(network::Packet& packet) {
    PartyCommandResultData data;
    if (!PartyCommandResultParser::parse(packet, data)) return;
    if (data.result != PartyResult::OK) {
        const char* errText = nullptr;
        switch (data.result) {
            case PartyResult::BAD_PLAYER_NAME:       errText = "No player named \"%s\" is currently online."; break;
            case PartyResult::TARGET_NOT_IN_GROUP:   errText = "%s is not in your group."; break;
            case PartyResult::TARGET_NOT_IN_INSTANCE:errText = "%s is not in your instance."; break;
            case PartyResult::GROUP_FULL:            errText = "Your party is full."; break;
            case PartyResult::ALREADY_IN_GROUP:      errText = "%s is already in a group."; break;
            case PartyResult::NOT_IN_GROUP:          errText = "You are not in a group."; break;
            case PartyResult::NOT_LEADER:            errText = "You are not the group leader."; break;
            case PartyResult::PLAYER_WRONG_FACTION:  errText = "%s is the wrong faction for this group."; break;
            case PartyResult::IGNORING_YOU:          errText = "%s is ignoring you."; break;
            case PartyResult::LFG_PENDING:           errText = "You cannot do that while in a LFG queue."; break;
            case PartyResult::INVITE_RESTRICTED:     errText = "Target is not accepting group invites."; break;
            default:                                 errText = "Party command failed."; break;
        }
        char buf[256];
        if (!data.name.empty() && errText && std::strstr(errText, "%s"))
            std::snprintf(buf, sizeof(buf), errText, data.name.c_str());
        else if (errText)
            std::snprintf(buf, sizeof(buf), "%s", errText);
        else
            std::snprintf(buf, sizeof(buf), "Party command failed (error %u).", static_cast<uint32_t>(data.result));
        owner_.addUIError(buf);
        owner_.addSystemChatMessage(buf);
    }
}

void SocialHandler::handlePartyMemberStats(network::Packet& packet, bool isFull) {
    auto remaining = [&]() { return packet.getSize() - packet.getReadPos(); };
    const bool isWotLK = isActiveExpansion("wotlk");

    if (isFull) { if (remaining() < 1) return; packet.readUInt8(); }

    const bool pmsTbc = isActiveExpansion("tbc");
    if (remaining() < (pmsTbc ? 8u : 1u)) return;
    uint64_t memberGuid = pmsTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < 4) return;
    uint32_t updateFlags = packet.readUInt32();

    game::GroupMember* member = nullptr;
    for (auto& m : partyData.members) {
        if (m.guid == memberGuid) { member = &m; break; }
    }
    if (!member) { packet.setReadPos(packet.getSize()); return; }

    if (updateFlags & 0x0001) { if (remaining() >= 2) member->onlineStatus = packet.readUInt16(); }
    if (updateFlags & 0x0002) {
        if (isWotLK) { if (remaining() >= 4) member->curHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->curHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0004) {
        if (isWotLK) { if (remaining() >= 4) member->maxHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->maxHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0008) { if (remaining() >= 1) member->powerType = packet.readUInt8(); }
    if (updateFlags & 0x0010) { if (remaining() >= 2) member->curPower = packet.readUInt16(); }
    if (updateFlags & 0x0020) { if (remaining() >= 2) member->maxPower = packet.readUInt16(); }
    if (updateFlags & 0x0040) { if (remaining() >= 2) member->level = packet.readUInt16(); }
    if (updateFlags & 0x0080) { if (remaining() >= 2) member->zoneId = packet.readUInt16(); }
    if (updateFlags & 0x0100) {
        if (remaining() >= 4) {
            member->posX = static_cast<int16_t>(packet.readUInt16());
            member->posY = static_cast<int16_t>(packet.readUInt16());
        }
    }
    if (updateFlags & 0x0200) {
        if (remaining() >= 8) {
            uint64_t auraMask = packet.readUInt64();
            std::vector<AuraSlot> newAuras;
            for (int i = 0; i < 64; ++i) {
                if (auraMask & (uint64_t(1) << i)) {
                    AuraSlot a;
                    a.level = static_cast<uint8_t>(i);
                    if (isWotLK) {
                        if (remaining() < 5) break;
                        a.spellId = packet.readUInt32();
                        a.flags   = packet.readUInt8();
                    } else {
                        if (remaining() < 2) break;
                        a.spellId = packet.readUInt16();
                        uint8_t dt = owner_.getSpellDispelType(a.spellId);
                        if (dt > 0) a.flags = 0x80;
                    }
                    if (a.spellId != 0) newAuras.push_back(a);
                }
            }
            if (memberGuid != 0 && memberGuid != owner_.playerGuid && memberGuid != owner_.targetGuid) {
                owner_.unitAurasCache_[memberGuid] = std::move(newAuras);
            }
        }
    }
    // Skip pet fields and vehicle seat
    if (updateFlags & 0x0400) { if (remaining() >= 8) packet.readUInt64(); }
    if (updateFlags & 0x0800) { if (remaining() > 0) packet.readString(); }
    if (updateFlags & 0x1000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x2000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x4000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x8000) { if (remaining() >= 1) packet.readUInt8(); }
    if (updateFlags & 0x10000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x20000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x40000) {
        if (remaining() >= 8) {
            uint64_t petAuraMask = packet.readUInt64();
            for (int i = 0; i < 64; ++i) {
                if (petAuraMask & (uint64_t(1) << i)) {
                    if (isWotLK) { if (remaining() < 5) break; packet.readUInt32(); packet.readUInt8(); }
                    else { if (remaining() < 2) break; packet.readUInt16(); }
                }
            }
        }
    }
    if (isWotLK && (updateFlags & 0x80000)) { if (remaining() >= 4) packet.readUInt32(); }

    member->hasPartyStats = true;

    if (owner_.addonEventCallback_) {
        std::string unitId;
        if (partyData.groupType == 1) {
            for (size_t i = 0; i < partyData.members.size(); ++i) {
                if (partyData.members[i].guid == memberGuid) { unitId = "raid" + std::to_string(i + 1); break; }
            }
        } else {
            int found = 0;
            for (const auto& m : partyData.members) {
                if (m.guid == owner_.playerGuid) continue;
                ++found;
                if (m.guid == memberGuid) { unitId = "party" + std::to_string(found); break; }
            }
        }
        if (!unitId.empty()) {
            if (updateFlags & (0x0002 | 0x0004)) owner_.addonEventCallback_("UNIT_HEALTH", {unitId});
            if (updateFlags & (0x0010 | 0x0020)) owner_.addonEventCallback_("UNIT_POWER", {unitId});
            if (updateFlags & 0x0200) owner_.addonEventCallback_("UNIT_AURA", {unitId});
        }
    }
}

// ============================================================
// Guild Handlers
// ============================================================

void SocialHandler::handleGuildInfo(network::Packet& packet) {
    GuildInfoData data;
    if (!GuildInfoParser::parse(packet, data)) return;
    guildInfoData_ = data;
    owner_.addSystemChatMessage("Guild: " + data.guildName + " (" +
                         std::to_string(data.numMembers) + " members, " +
                         std::to_string(data.numAccounts) + " accounts)");
}

void SocialHandler::handleGuildRoster(network::Packet& packet) {
    GuildRosterData data;
    if (!owner_.packetParsers_->parseGuildRoster(packet, data)) return;
    guildRoster_ = std::move(data);
    hasGuildRoster_ = true;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GUILD_ROSTER_UPDATE", {});
}

void SocialHandler::handleGuildQueryResponse(network::Packet& packet) {
    GuildQueryResponseData data;
    if (!owner_.packetParsers_->parseGuildQueryResponse(packet, data)) return;
    if (data.guildId != 0 && !data.guildName.empty()) {
        guildNameCache_[data.guildId] = data.guildName;
        pendingGuildNameQueries_.erase(data.guildId);
    }
    const Character* ch = owner_.getActiveCharacter();
    bool isLocalGuild = (ch && ch->hasGuild() && ch->guildId == data.guildId);
    if (isLocalGuild) {
        const bool wasUnknown = guildName_.empty();
        guildName_ = data.guildName;
        guildQueryData_ = data;
        guildRankNames_.clear();
        for (uint32_t i = 0; i < 10; ++i) guildRankNames_.push_back(data.rankNames[i]);
        if (wasUnknown && !guildName_.empty()) {
            owner_.addSystemChatMessage("Guild: <" + guildName_ + ">");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYER_GUILD_UPDATE", {});
        }
    }
}

void SocialHandler::handleGuildEvent(network::Packet& packet) {
    GuildEventData data;
    if (!GuildEventParser::parse(packet, data)) return;

    std::string msg;
    switch (data.eventType) {
        case GuildEvent::PROMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has promoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::DEMOTION:
            if (data.numStrings >= 3)
                msg = data.strings[0] + " has demoted " + data.strings[1] + " to " + data.strings[2] + ".";
            break;
        case GuildEvent::MOTD:
            if (data.numStrings >= 1) msg = "Guild MOTD: " + data.strings[0];
            break;
        case GuildEvent::JOINED:
            if (data.numStrings >= 1) msg = data.strings[0] + " has joined the guild.";
            break;
        case GuildEvent::LEFT:
            if (data.numStrings >= 1) msg = data.strings[0] + " has left the guild.";
            break;
        case GuildEvent::REMOVED:
            if (data.numStrings >= 2) msg = data.strings[1] + " has been kicked from the guild by " + data.strings[0] + ".";
            break;
        case GuildEvent::LEADER_IS:
            if (data.numStrings >= 1) msg = data.strings[0] + " is the guild leader.";
            break;
        case GuildEvent::LEADER_CHANGED:
            if (data.numStrings >= 2) msg = data.strings[0] + " has made " + data.strings[1] + " the new guild leader.";
            break;
        case GuildEvent::DISBANDED:
            msg = "Guild has been disbanded.";
            guildName_.clear();
            guildRankNames_.clear();
            guildRoster_ = GuildRosterData{};
            hasGuildRoster_ = false;
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYER_GUILD_UPDATE", {});
            break;
        case GuildEvent::SIGNED_ON:
            if (data.numStrings >= 1) msg = "[Guild] " + data.strings[0] + " has come online.";
            break;
        case GuildEvent::SIGNED_OFF:
            if (data.numStrings >= 1) msg = "[Guild] " + data.strings[0] + " has gone offline.";
            break;
        default:
            msg = "Guild event " + std::to_string(data.eventType);
            if (!data.numStrings && data.numStrings >= 1) msg += ": " + data.strings[0];
            break;
    }

    if (!msg.empty()) {
        MessageChatData chatMsg;
        chatMsg.type = ChatType::GUILD;
        chatMsg.language = ChatLanguage::UNIVERSAL;
        chatMsg.message = msg;
        owner_.addLocalChatMessage(chatMsg);
    }

    if (owner_.addonEventCallback_) {
        switch (data.eventType) {
            case GuildEvent::MOTD:
                owner_.addonEventCallback_("GUILD_MOTD", {data.numStrings >= 1 ? data.strings[0] : ""});
                break;
            case GuildEvent::SIGNED_ON: case GuildEvent::SIGNED_OFF:
            case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
            case GuildEvent::JOINED: case GuildEvent::LEFT:
            case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
            case GuildEvent::DISBANDED:
                owner_.addonEventCallback_("GUILD_ROSTER_UPDATE", {});
                break;
            default: break;
        }
    }

    switch (data.eventType) {
        case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
        case GuildEvent::JOINED: case GuildEvent::LEFT:
        case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
            if (hasGuildRoster_) requestGuildRoster();
            break;
        default: break;
    }
}

void SocialHandler::handleGuildInvite(network::Packet& packet) {
    GuildInviteResponseData data;
    if (!GuildInviteResponseParser::parse(packet, data)) return;
    pendingGuildInvite_ = true;
    pendingGuildInviterName_ = data.inviterName;
    pendingGuildInviteGuildName_ = data.guildName;
    owner_.addSystemChatMessage(data.inviterName + " has invited you to join " + data.guildName + ".");
    if (owner_.addonEventCallback_)
        owner_.addonEventCallback_("GUILD_INVITE_REQUEST", {data.inviterName, data.guildName});
}

void SocialHandler::handleGuildCommandResult(network::Packet& packet) {
    GuildCommandResultData data;
    if (!GuildCommandResultParser::parse(packet, data)) return;
    if (data.errorCode == 0) {
        switch (data.command) {
            case 0: owner_.addSystemChatMessage("Guild created."); break;
            case 1:
                if (!data.name.empty()) owner_.addSystemChatMessage("You have invited " + data.name + " to the guild.");
                break;
            case 2:
                owner_.addSystemChatMessage("You have left the guild.");
                guildName_.clear(); guildRankNames_.clear();
                guildRoster_ = GuildRosterData{}; hasGuildRoster_ = false;
                break;
            default: break;
        }
        return;
    }
    const char* errStr = nullptr;
    switch (data.errorCode) {
        case 2:  errStr = "You are not in a guild."; break;
        case 4:  errStr = "No player named \"%s\" is online."; break;
        case 11: errStr = "\"%s\" is already in a guild."; break;
        case 13: errStr = "You are already in a guild."; break;
        case 14: errStr = "\"%s\" has already been invited to a guild."; break;
        case 16: case 17: errStr = "You are not the guild leader."; break;
        case 22: errStr = "That player is ignoring you."; break;
        default: break;
    }
    std::string errorMsg;
    if (errStr) {
        std::string fmt = errStr;
        auto pos = fmt.find("%s");
        if (pos != std::string::npos && !data.name.empty()) fmt.replace(pos, 2, data.name);
        else if (pos != std::string::npos) fmt.replace(pos, 2, "that player");
        errorMsg = fmt;
    } else {
        errorMsg = "Guild command failed";
        if (!data.name.empty()) errorMsg += " for " + data.name;
        errorMsg += " (error " + std::to_string(data.errorCode) + ")";
    }
    owner_.addUIError(errorMsg);
    owner_.addSystemChatMessage(errorMsg);
}

void SocialHandler::handlePetitionShowlist(network::Packet& packet) {
    PetitionShowlistData data;
    if (!PetitionShowlistParser::parse(packet, data)) return;
    petitionNpcGuid_ = data.npcGuid;
    petitionCost_ = data.cost;
    showPetitionDialog_ = true;
}

void SocialHandler::handlePetitionQueryResponse(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 12) return;
    /*uint32_t entry =*/ packet.readUInt32();
    uint64_t petGuid = packet.readUInt64();
    std::string guildName = packet.readString();
    /*std::string body =*/ packet.readString();
    if (petitionInfo_.petitionGuid == petGuid) petitionInfo_.guildName = guildName;
    packet.setReadPos(packet.getSize());
}

void SocialHandler::handlePetitionShowSignatures(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 21) return;
    petitionInfo_ = PetitionInfo{};
    petitionInfo_.petitionGuid = packet.readUInt64();
    petitionInfo_.ownerGuid    = packet.readUInt64();
    /*uint32_t petEntry =*/     packet.readUInt32();
    uint8_t sigCount           = packet.readUInt8();
    petitionInfo_.signatureCount = sigCount;
    petitionInfo_.signatures.reserve(sigCount);
    for (uint8_t i = 0; i < sigCount; ++i) {
        if (rem() < 12) break;
        PetitionSignature sig;
        sig.playerGuid = packet.readUInt64();
        /*uint32_t unk =*/ packet.readUInt32();
        petitionInfo_.signatures.push_back(sig);
    }
    petitionInfo_.showUI = true;
}

void SocialHandler::handlePetitionSignResults(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 20) return;
    uint64_t petGuid    = packet.readUInt64();
    uint64_t playerGuid = packet.readUInt64();
    uint32_t result     = packet.readUInt32();
    switch (result) {
        case 0:
            owner_.addSystemChatMessage("Petition signed successfully.");
            if (petitionInfo_.petitionGuid == petGuid) {
                petitionInfo_.signatureCount++;
                PetitionSignature sig; sig.playerGuid = playerGuid;
                petitionInfo_.signatures.push_back(sig);
            }
            break;
        case 1: owner_.addSystemChatMessage("You have already signed that petition."); break;
        case 2: owner_.addSystemChatMessage("You are already in a guild."); break;
        case 3: owner_.addSystemChatMessage("You cannot sign your own petition."); break;
        default: owner_.addSystemChatMessage("Cannot sign petition (error " + std::to_string(result) + ")."); break;
    }
}

void SocialHandler::handleTurnInPetitionResults(network::Packet& packet) {
    uint32_t result = 0;
    if (!TurnInPetitionResultsParser::parse(packet, result)) return;
    switch (result) {
        case 0: owner_.addSystemChatMessage("Guild created successfully!"); break;
        case 1: owner_.addSystemChatMessage("Guild creation failed: already in a guild."); break;
        case 2: owner_.addSystemChatMessage("Guild creation failed: not enough signatures."); break;
        case 3: owner_.addSystemChatMessage("Guild creation failed: name already taken."); break;
        default: owner_.addSystemChatMessage("Guild creation failed (error " + std::to_string(result) + ")."); break;
    }
}

// ============================================================
// Server Info Handlers
// ============================================================

void SocialHandler::handleQueryTimeResponse(network::Packet& packet) {
    QueryTimeResponseData data;
    if (!QueryTimeResponseParser::parse(packet, data)) return;
    time_t serverTime = static_cast<time_t>(data.serverTime);
    struct tm* timeInfo = localtime(&serverTime);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);
    owner_.addSystemChatMessage("Server time: " + std::string(timeStr));
}

void SocialHandler::handlePlayedTime(network::Packet& packet) {
    PlayedTimeData data;
    if (!PlayedTimeParser::parse(packet, data)) return;
    totalTimePlayed_ = data.totalTimePlayed;
    levelTimePlayed_ = data.levelTimePlayed;
    if (data.triggerMessage) {
        uint32_t totalDays = data.totalTimePlayed / 86400;
        uint32_t totalHours = (data.totalTimePlayed % 86400) / 3600;
        uint32_t totalMinutes = (data.totalTimePlayed % 3600) / 60;
        uint32_t levelDays = data.levelTimePlayed / 86400;
        uint32_t levelHours = (data.levelTimePlayed % 86400) / 3600;
        uint32_t levelMinutes = (data.levelTimePlayed % 3600) / 60;
        std::string totalMsg = "Total time played: ";
        if (totalDays > 0) totalMsg += std::to_string(totalDays) + " days, ";
        if (totalHours > 0 || totalDays > 0) totalMsg += std::to_string(totalHours) + " hours, ";
        totalMsg += std::to_string(totalMinutes) + " minutes";
        std::string levelMsg = "Time played this level: ";
        if (levelDays > 0) levelMsg += std::to_string(levelDays) + " days, ";
        if (levelHours > 0 || levelDays > 0) levelMsg += std::to_string(levelHours) + " hours, ";
        levelMsg += std::to_string(levelMinutes) + " minutes";
        owner_.addSystemChatMessage(totalMsg);
        owner_.addSystemChatMessage(levelMsg);
    }
}

void SocialHandler::handleWho(network::Packet& packet) {
    const bool hasGender = isActiveExpansion("wotlk");
    uint32_t displayCount = packet.readUInt32();
    uint32_t onlineCount = packet.readUInt32();
    whoResults_.clear();
    whoOnlineCount_ = onlineCount;
    if (displayCount == 0) { owner_.addSystemChatMessage("No players found."); return; }
    for (uint32_t i = 0; i < displayCount; ++i) {
        if (packet.getReadPos() >= packet.getSize()) break;
        std::string playerName = packet.readString();
        std::string guildName = packet.readString();
        if (packet.getSize() - packet.getReadPos() < 12) break;
        uint32_t level   = packet.readUInt32();
        uint32_t classId = packet.readUInt32();
        uint32_t raceId  = packet.readUInt32();
        if (hasGender && packet.getSize() - packet.getReadPos() >= 1) packet.readUInt8();
        uint32_t zoneId = 0;
        if (packet.getSize() - packet.getReadPos() >= 4) zoneId = packet.readUInt32();
        WhoEntry entry;
        entry.name = playerName; entry.guildName = guildName;
        entry.level = level; entry.classId = classId;
        entry.raceId = raceId; entry.zoneId = zoneId;
        whoResults_.push_back(std::move(entry));
    }
}

// ============================================================
// Social (Friend/Ignore/Random Roll) Handlers
// ============================================================

void SocialHandler::handleFriendList(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 1) return;
    uint8_t count = packet.readUInt8();
    owner_.contacts_.erase(std::remove_if(owner_.contacts_.begin(), owner_.contacts_.end(),
        [](const ContactEntry& e){ return e.isFriend(); }), owner_.contacts_.end());
    for (uint8_t i = 0; i < count && rem() >= 9; ++i) {
        uint64_t guid   = packet.readUInt64();
        uint8_t  status = packet.readUInt8();
        uint32_t area = 0, level = 0, classId = 0;
        if (status != 0 && rem() >= 12) {
            area    = packet.readUInt32();
            level   = packet.readUInt32();
            classId = packet.readUInt32();
        }
        owner_.friendGuids_.insert(guid);
        auto nit = owner_.playerNameCache.find(guid);
        std::string name;
        if (nit != owner_.playerNameCache.end()) {
            name = nit->second;
            owner_.friendsCache[name] = guid;
        } else {
            owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.name = name; entry.flags = 0x1;
        entry.status = status; entry.areaId = area; entry.level = level; entry.classId = classId;
        owner_.contacts_.push_back(std::move(entry));
    }
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("FRIENDLIST_UPDATE", {});
}

void SocialHandler::handleContactList(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 8) { packet.setReadPos(packet.getSize()); return; }
    owner_.lastContactListMask_  = packet.readUInt32();
    owner_.lastContactListCount_ = packet.readUInt32();
    owner_.contacts_.clear();
    for (uint32_t i = 0; i < owner_.lastContactListCount_ && rem() >= 8; ++i) {
        uint64_t guid  = packet.readUInt64();
        if (rem() < 4) break;
        uint32_t flags = packet.readUInt32();
        std::string note = packet.readString();
        uint8_t status = 0; uint32_t areaId = 0, level = 0, classId = 0;
        if (flags & 0x1) {
            if (rem() < 1) break;
            status = packet.readUInt8();
            if (status != 0 && rem() >= 12) {
                areaId = packet.readUInt32(); level = packet.readUInt32(); classId = packet.readUInt32();
            }
            owner_.friendGuids_.insert(guid);
            auto nit = owner_.playerNameCache.find(guid);
            if (nit != owner_.playerNameCache.end()) owner_.friendsCache[nit->second] = guid;
            else owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.flags = flags; entry.note = std::move(note);
        entry.status = status; entry.areaId = areaId; entry.level = level; entry.classId = classId;
        auto nit = owner_.playerNameCache.find(guid);
        if (nit != owner_.playerNameCache.end()) entry.name = nit->second;
        owner_.contacts_.push_back(std::move(entry));
    }
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("FRIENDLIST_UPDATE", {});
        if (owner_.lastContactListMask_ & 0x2) owner_.addonEventCallback_("IGNORELIST_UPDATE", {});
    }
}

void SocialHandler::handleFriendStatus(network::Packet& packet) {
    FriendStatusData data;
    if (!FriendStatusParser::parse(packet, data)) return;

    // Single lookup — reuse iterator for name resolution and update/erase below
    auto cit = std::find_if(owner_.contacts_.begin(), owner_.contacts_.end(),
        [&](const ContactEntry& e){ return e.guid == data.guid; });

    // Look up player name: contacts_ (populated by SMSG_FRIEND_LIST) > playerNameCache
    std::string playerName;
    if (cit != owner_.contacts_.end() && !cit->name.empty()) {
        playerName = cit->name;
    } else {
        auto it = owner_.playerNameCache.find(data.guid);
        if (it != owner_.playerNameCache.end()) playerName = it->second;
    }

    if (data.status == 1 || data.status == 2) owner_.friendsCache[playerName] = data.guid;
    else if (data.status == 0) owner_.friendsCache.erase(playerName);

    if (data.status == 0) {
        if (cit != owner_.contacts_.end())
            owner_.contacts_.erase(cit);
    } else {
        if (cit != owner_.contacts_.end()) {
            if (!playerName.empty() && playerName != "Unknown") cit->name = playerName;
            if (data.status == 2) cit->status = 1; else if (data.status == 3) cit->status = 0;
        } else {
            ContactEntry entry;
            entry.guid = data.guid; entry.name = playerName; entry.flags = 0x1;
            entry.status = (data.status == 2) ? 1 : 0;
            owner_.contacts_.push_back(std::move(entry));
        }
    }
    switch (data.status) {
        case 0: owner_.addSystemChatMessage(playerName + " has been removed from your friends list."); break;
        case 1: owner_.addSystemChatMessage(playerName + " has been added to your friends list."); break;
        case 2: owner_.addSystemChatMessage(playerName + " is now online."); break;
        case 3: owner_.addSystemChatMessage(playerName + " is now offline."); break;
        case 4: owner_.addSystemChatMessage("Player not found."); break;
        case 5: owner_.addSystemChatMessage(playerName + " is already in your friends list."); break;
        case 6: owner_.addSystemChatMessage("Your friends list is full."); break;
        case 7: owner_.addSystemChatMessage(playerName + " is ignoring you."); break;
        default: break;
    }
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("FRIENDLIST_UPDATE", {});
}

void SocialHandler::handleRandomRoll(network::Packet& packet) {
    RandomRollData data;
    if (!RandomRollParser::parse(packet, data)) return;
    std::string rollerName = (data.rollerGuid == owner_.playerGuid) ? "You" : "Someone";
    if (data.rollerGuid != owner_.playerGuid) {
        auto it = owner_.playerNameCache.find(data.rollerGuid);
        if (it != owner_.playerNameCache.end()) rollerName = it->second;
    }
    std::string msg = rollerName + ((data.rollerGuid == owner_.playerGuid) ? " roll " : " rolls ");
    msg += std::to_string(data.result) + " (" + std::to_string(data.minRoll) + "-" + std::to_string(data.maxRoll) + ")";
    owner_.addSystemChatMessage(msg);
}

// ============================================================
// Logout Handlers
// ============================================================

void SocialHandler::handleLogoutResponse(network::Packet& packet) {
    LogoutResponseData data;
    if (!LogoutResponseParser::parse(packet, data)) return;
    if (data.result == 0) {
        if (data.instant) { owner_.addSystemChatMessage("Logging out..."); logoutCountdown_ = 0.0f; }
        else { owner_.addSystemChatMessage("Logging out in 20 seconds..."); logoutCountdown_ = 20.0f; }
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYER_LOGOUT", {});
    } else {
        owner_.addSystemChatMessage("Cannot logout right now.");
        loggingOut_ = false; logoutCountdown_ = 0.0f;
    }
}

void SocialHandler::handleLogoutComplete(network::Packet& /*packet*/) {
    owner_.addSystemChatMessage("Logout complete.");
    loggingOut_ = false; logoutCountdown_ = 0.0f;
}

// ============================================================
// Battleground Handlers
// ============================================================

void SocialHandler::handleBattlefieldStatus(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t queueSlot = packet.readUInt32();
    const bool classicFormat = isClassicLikeExpansion();
    uint8_t arenaType = 0;
    if (!classicFormat) {
        if (packet.getSize() - packet.getReadPos() < 1) return;
        arenaType = packet.readUInt8();
        if (packet.getSize() - packet.getReadPos() < 1) return;
        packet.readUInt8();
    } else {
        if (packet.getSize() - packet.getReadPos() < 4) return;
    }
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t bgTypeId = packet.readUInt32();
    if (packet.getSize() - packet.getReadPos() < 2) return;
    packet.readUInt16();
    if (packet.getSize() - packet.getReadPos() < 4) return;
    packet.readUInt32(); // instanceId
    if (packet.getSize() - packet.getReadPos() < 1) return;
    packet.readUInt8(); // isRated
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t statusId = packet.readUInt32();

    static const std::pair<uint32_t, const char*> kBgNames[] = {
        {1,"Alterac Valley"},{2,"Warsong Gulch"},{3,"Arathi Basin"},
        {4,"Nagrand Arena"},{5,"Blade's Edge Arena"},{6,"All Arenas"},
        {7,"Eye of the Storm"},{8,"Ruins of Lordaeron"},{9,"Strand of the Ancients"},
        {10,"Dalaran Sewers"},{11,"Ring of Valor"},{30,"Isle of Conquest"},{32,"Random Battleground"},
    };
    std::string bgName = "Battleground";
    for (const auto& kv : kBgNames) { if (kv.first == bgTypeId) { bgName = kv.second; break; } }
    if (bgName == "Battleground") bgName = "Battleground #" + std::to_string(bgTypeId);
    if (arenaType > 0) {
        bgName = std::to_string(arenaType) + "v" + std::to_string(arenaType) + " Arena";
        for (const auto& kv : kBgNames) { if (kv.first == bgTypeId) { bgName += " (" + std::string(kv.second) + ")"; break; } }
    }

    uint32_t inviteTimeout = 80, avgWaitSec = 0, timeInQueueSec = 0;
    if (statusId == 1 && packet.getSize() - packet.getReadPos() >= 8) {
        avgWaitSec = packet.readUInt32() / 1000; timeInQueueSec = packet.readUInt32() / 1000;
    } else if (statusId == 2) {
        if (packet.getSize() - packet.getReadPos() >= 4) inviteTimeout = packet.readUInt32();
        if (packet.getSize() - packet.getReadPos() >= 4) packet.readUInt32();
    } else if (statusId == 3 && packet.getSize() - packet.getReadPos() >= 8) {
        packet.readUInt32(); packet.readUInt32();
    }

    if (queueSlot < bgQueues_.size()) {
        bool wasInvite = (bgQueues_[queueSlot].statusId == 2);
        bgQueues_[queueSlot].queueSlot = queueSlot;
        bgQueues_[queueSlot].bgTypeId = bgTypeId;
        bgQueues_[queueSlot].arenaType = arenaType;
        bgQueues_[queueSlot].statusId = statusId;
        bgQueues_[queueSlot].bgName = bgName;
        if (statusId == 1) { bgQueues_[queueSlot].avgWaitTimeSec = avgWaitSec; bgQueues_[queueSlot].timeInQueueSec = timeInQueueSec; }
        if (statusId == 2 && !wasInvite) { bgQueues_[queueSlot].inviteTimeout = inviteTimeout; bgQueues_[queueSlot].inviteReceivedTime = std::chrono::steady_clock::now(); }
    }

    switch (statusId) {
        case 1: owner_.addSystemChatMessage("Queued for " + bgName + "."); break;
        case 2: owner_.addSystemChatMessage(bgName + " is ready!"); break;
        case 3: owner_.addSystemChatMessage("Entered " + bgName + "."); break;
        default: break;
    }
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("UPDATE_BATTLEFIELD_STATUS", {std::to_string(statusId)});
}

void SocialHandler::handleBattlefieldList(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 5) return;
    AvailableBgInfo info;
    info.bgTypeId = packet.readUInt32();
    info.isRegistered = packet.readUInt8() != 0;
    const bool isWotlk = isActiveExpansion("wotlk");
    const bool isTbc = isActiveExpansion("tbc");
    if (isTbc || isWotlk) { if (packet.getSize() - packet.getReadPos() < 1) return; info.isHoliday = packet.readUInt8() != 0; }
    if (isWotlk) { if (packet.getSize() - packet.getReadPos() < 8) return; info.minLevel = packet.readUInt32(); info.maxLevel = packet.readUInt32(); }
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t count = std::min(packet.readUInt32(), 256u);
    info.instanceIds.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getSize() - packet.getReadPos() < 4) break;
        info.instanceIds.push_back(packet.readUInt32());
    }
    bool updated = false;
    for (auto& existing : availableBgs_) { if (existing.bgTypeId == info.bgTypeId) { existing = std::move(info); updated = true; break; } }
    if (!updated) availableBgs_.push_back(std::move(info));
}

bool SocialHandler::hasPendingBgInvite() const {
    for (const auto& slot : bgQueues_) { if (slot.statusId == 2) return true; }
    return false;
}

void SocialHandler::acceptBattlefield(uint32_t queueSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    const BgQueueSlot* slot = nullptr;
    if (queueSlot == 0xFFFFFFFF) { for (const auto& s : bgQueues_) { if (s.statusId == 2) { slot = &s; break; } } }
    else if (queueSlot < bgQueues_.size() && bgQueues_[queueSlot].statusId == 2) slot = &bgQueues_[queueSlot];
    if (!slot) { owner_.addSystemChatMessage("No battleground invitation pending."); return; }
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    pkt.writeUInt8(slot->arenaType); pkt.writeUInt8(0x00); pkt.writeUInt32(slot->bgTypeId);
    pkt.writeUInt16(0x0000); pkt.writeUInt8(1);
    owner_.socket->send(pkt);
    uint32_t clearSlot = slot->queueSlot;
    if (clearSlot < bgQueues_.size()) bgQueues_[clearSlot].statusId = 3;
    owner_.addSystemChatMessage("Accepting battleground invitation...");
}

void SocialHandler::declineBattlefield(uint32_t queueSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    const BgQueueSlot* slot = nullptr;
    if (queueSlot == 0xFFFFFFFF) { for (const auto& s : bgQueues_) { if (s.statusId == 2) { slot = &s; break; } } }
    else if (queueSlot < bgQueues_.size() && bgQueues_[queueSlot].statusId == 2) slot = &bgQueues_[queueSlot];
    if (!slot) { owner_.addSystemChatMessage("No battleground invitation pending."); return; }
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    pkt.writeUInt8(slot->arenaType); pkt.writeUInt8(0x00); pkt.writeUInt32(slot->bgTypeId);
    pkt.writeUInt16(0x0000); pkt.writeUInt8(0);
    owner_.socket->send(pkt);
    uint32_t clearSlot = slot->queueSlot;
    if (clearSlot < bgQueues_.size()) bgQueues_[clearSlot] = BgQueueSlot{};
    owner_.addSystemChatMessage("Battleground invitation declined.");
}

void SocialHandler::requestPvpLog() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    owner_.socket->send(pkt);
}

// ============================================================
// Instance Handlers
// ============================================================

void SocialHandler::handleRaidInstanceInfo(network::Packet& packet) {
    const bool isTbc = isActiveExpansion("tbc");
    const bool isClassic = isClassicLikeExpansion();
    const bool useTbcFormat = isTbc || isClassic;
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t count = packet.readUInt32();
    instanceLockouts_.clear();
    instanceLockouts_.reserve(count);
    const size_t kEntrySize = useTbcFormat ? 13 : 18;
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getSize() - packet.getReadPos() < kEntrySize) break;
        InstanceLockout lo;
        lo.mapId = packet.readUInt32(); lo.difficulty = packet.readUInt32();
        if (useTbcFormat) { lo.resetTime = packet.readUInt32(); lo.locked = packet.readUInt8() != 0; lo.extended = false; }
        else { lo.resetTime = packet.readUInt64(); lo.locked = packet.readUInt8() != 0; lo.extended = packet.readUInt8() != 0; }
        instanceLockouts_.push_back(lo);
    }
}

void SocialHandler::handleInstanceDifficulty(network::Packet& packet) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 4) return;
    uint32_t prevDifficulty = instanceDifficulty_;
    instanceDifficulty_ = packet.readUInt32();
    if (rem() >= 4) {
        uint32_t secondField = packet.readUInt32();
        if (rem() >= 4) instanceIsHeroic_ = (instanceDifficulty_ == 1);
        else instanceIsHeroic_ = (secondField != 0);
    } else {
        instanceIsHeroic_ = (instanceDifficulty_ == 1);
    }
    inInstance_ = true;
    if (instanceDifficulty_ != prevDifficulty) {
        static const char* kDiffLabels[] = {"Normal", "Heroic", "25-Man Normal", "25-Man Heroic"};
        const char* diffLabel = (instanceDifficulty_ < 4) ? kDiffLabels[instanceDifficulty_] : nullptr;
        if (diffLabel) owner_.addSystemChatMessage(std::string("Dungeon difficulty set to ") + diffLabel + ".");
    }
}

// ============================================================
// LFG Handlers
// ============================================================

void SocialHandler::handleLfgJoinResult(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 2) return;
    uint8_t result = packet.readUInt8();
    uint8_t state  = packet.readUInt8();
    if (result == 0) {
        lfgState_ = static_cast<LfgState>(state);
        std::string dName = owner_.getLfgDungeonName(lfgDungeonId_);
        if (!dName.empty()) owner_.addSystemChatMessage("Dungeon Finder: Joined the queue for " + dName + ".");
        else owner_.addSystemChatMessage("Dungeon Finder: Joined the queue.");
    } else {
        const char* msg = lfgJoinResultString(result);
        std::string errMsg = std::string("Dungeon Finder: ") + (msg ? msg : "Join failed.");
        owner_.addUIError(errMsg);
        owner_.addSystemChatMessage(errMsg);
    }
}

void SocialHandler::handleLfgQueueStatus(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 33) return;
    lfgDungeonId_ = packet.readUInt32();
    int32_t avgWait = static_cast<int32_t>(packet.readUInt32());
    int32_t waitTime = static_cast<int32_t>(packet.readUInt32());
    packet.readUInt32(); packet.readUInt32(); packet.readUInt32();
    packet.readUInt8();
    lfgTimeInQueueMs_ = packet.readUInt32();
    lfgAvgWaitSec_ = (waitTime >= 0) ? (waitTime / 1000) : (avgWait / 1000);
    lfgState_ = LfgState::Queued;
}

void SocialHandler::handleLfgProposalUpdate(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 17) return;
    uint32_t dungeonId = packet.readUInt32();
    uint32_t proposalId = packet.readUInt32();
    uint32_t proposalState = packet.readUInt32();
    packet.readUInt32(); packet.readUInt8();
    lfgDungeonId_ = dungeonId; lfgProposalId_ = proposalId;
    switch (proposalState) {
        case 0: lfgState_ = LfgState::Queued; lfgProposalId_ = 0;
            owner_.addUIError("Dungeon Finder: Group proposal failed."); owner_.addSystemChatMessage("Dungeon Finder: Group proposal failed."); break;
        case 1: { lfgState_ = LfgState::InDungeon; lfgProposalId_ = 0;
            std::string dName = owner_.getLfgDungeonName(dungeonId);
            owner_.addSystemChatMessage(dName.empty() ? "Dungeon Finder: Group found! Entering dungeon..." : "Dungeon Finder: Group found for " + dName + "! Entering dungeon..."); break; }
        case 2: { lfgState_ = LfgState::Proposal;
            std::string dName = owner_.getLfgDungeonName(dungeonId);
            owner_.addSystemChatMessage(dName.empty() ? "Dungeon Finder: A group has been found. Accept or decline." : "Dungeon Finder: A group has been found for " + dName + ". Accept or decline."); break; }
        default: break;
    }
}

void SocialHandler::handleLfgRoleCheckUpdate(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 6) return;
    packet.readUInt32();
    uint8_t roleCheckState = packet.readUInt8();
    packet.readUInt8();
    if (roleCheckState == 1) lfgState_ = LfgState::Queued;
    else if (roleCheckState == 3) { lfgState_ = LfgState::None; owner_.addUIError("Dungeon Finder: Role check failed — missing required role."); owner_.addSystemChatMessage("Dungeon Finder: Role check failed — missing required role."); }
    else if (roleCheckState == 2) { lfgState_ = LfgState::RoleCheck; owner_.addSystemChatMessage("Dungeon Finder: Performing role check..."); }
}

void SocialHandler::handleLfgUpdatePlayer(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t updateType = packet.readUInt8();
    bool hasExtra = (updateType != 0 && updateType != 1 && updateType != 15 && updateType != 17 && updateType != 18);
    if (!hasExtra || packet.getSize() - packet.getReadPos() < 3) {
        switch (updateType) {
            case 8:  lfgState_ = LfgState::None; owner_.addSystemChatMessage("Dungeon Finder: Removed from queue."); break;
            case 9:  lfgState_ = LfgState::Queued; owner_.addSystemChatMessage("Dungeon Finder: Proposal failed — re-queuing."); break;
            case 10: lfgState_ = LfgState::Queued; owner_.addSystemChatMessage("Dungeon Finder: A member declined the proposal."); break;
            case 15: lfgState_ = LfgState::None; owner_.addSystemChatMessage("Dungeon Finder: Left the queue."); break;
            case 18: lfgState_ = LfgState::None; owner_.addSystemChatMessage("Dungeon Finder: Your group disbanded."); break;
            default: break;
        }
        return;
    }
    packet.readUInt8(); packet.readUInt8(); packet.readUInt8();
    if (packet.getSize() - packet.getReadPos() >= 1) {
        uint8_t count = packet.readUInt8();
        for (uint8_t i = 0; i < count && packet.getSize() - packet.getReadPos() >= 4; ++i) {
            uint32_t dungeonEntry = packet.readUInt32();
            if (i == 0) lfgDungeonId_ = dungeonEntry;
        }
    }
    switch (updateType) {
        case 6:  lfgState_ = LfgState::Queued; owner_.addSystemChatMessage("Dungeon Finder: You have joined the queue."); break;
        case 11: lfgState_ = LfgState::Proposal; owner_.addSystemChatMessage("Dungeon Finder: A group has been found!"); break;
        case 12: lfgState_ = LfgState::Queued; owner_.addSystemChatMessage("Dungeon Finder: Added to queue."); break;
        case 14: lfgState_ = LfgState::InDungeon; break;
        default: break;
    }
}

void SocialHandler::handleLfgPlayerReward(network::Packet& packet) {
    if (!packetHasRemaining(packet, 13)) return;
    packet.readUInt32(); packet.readUInt32(); packet.readUInt8();
    uint32_t money = packet.readUInt32();
    uint32_t xp = packet.readUInt32();
    uint32_t gold = money / 10000, silver = (money % 10000) / 100, copper = money % 100;
    char moneyBuf[64];
    if (gold > 0) snprintf(moneyBuf, sizeof(moneyBuf), "%ug %us %uc", gold, silver, copper);
    else if (silver > 0) snprintf(moneyBuf, sizeof(moneyBuf), "%us %uc", silver, copper);
    else snprintf(moneyBuf, sizeof(moneyBuf), "%uc", copper);
    std::string rewardMsg = std::string("Dungeon Finder reward: ") + moneyBuf + ", " + std::to_string(xp) + " XP";
    if (packetHasRemaining(packet, 4)) {
        uint32_t rewardCount = packet.readUInt32();
        for (uint32_t i = 0; i < rewardCount && packetHasRemaining(packet, 9); ++i) {
            uint32_t itemId = packet.readUInt32();
            uint32_t itemCount = packet.readUInt32();
            packet.readUInt8();
            if (i == 0) {
                std::string itemLabel = "item #" + std::to_string(itemId);
                uint32_t lfgItemQuality = 1;
                if (const ItemQueryResponseData* info = owner_.getItemInfo(itemId)) {
                    if (!info->name.empty()) itemLabel = info->name;
                    lfgItemQuality = info->quality;
                }
                rewardMsg += ", " + buildItemLink(itemId, lfgItemQuality, itemLabel);
                if (itemCount > 1) rewardMsg += " x" + std::to_string(itemCount);
            }
        }
    }
    owner_.addSystemChatMessage(rewardMsg);
    lfgState_ = LfgState::FinishedDungeon;
}

void SocialHandler::handleLfgBootProposalUpdate(network::Packet& packet) {
    if (!packetHasRemaining(packet, 23)) return;
    bool inProgress = packet.readUInt8() != 0;
    packet.readUInt8(); packet.readUInt8();
    uint32_t totalVotes = packet.readUInt32();
    uint32_t bootVotes = packet.readUInt32();
    uint32_t timeLeft = packet.readUInt32();
    uint32_t votesNeeded = packet.readUInt32();
    lfgBootVotes_ = bootVotes; lfgBootTotal_ = totalVotes;
    lfgBootTimeLeft_ = timeLeft; lfgBootNeeded_ = votesNeeded;
    if (packet.getReadPos() < packet.getSize()) lfgBootReason_ = packet.readString();
    if (packet.getReadPos() < packet.getSize()) lfgBootTargetName_ = packet.readString();
    if (inProgress) { lfgState_ = LfgState::Boot; }
    else {
        const bool bootPassed = (bootVotes >= votesNeeded);
        lfgBootVotes_ = lfgBootTotal_ = lfgBootTimeLeft_ = lfgBootNeeded_ = 0;
        lfgBootTargetName_.clear(); lfgBootReason_.clear();
        lfgState_ = LfgState::InDungeon;
        owner_.addSystemChatMessage(bootPassed ? "Dungeon Finder: Vote kick passed — member removed." : "Dungeon Finder: Vote kick failed.");
    }
}

void SocialHandler::handleLfgTeleportDenied(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t reason = packet.readUInt8();
    owner_.addSystemChatMessage(std::string("Dungeon Finder: ") + lfgTeleportDeniedString(reason));
}

// ============================================================
// LFG Outgoing Packets
// ============================================================

void SocialHandler::lfgJoin(uint32_t dungeonId, uint8_t roles) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFG_JOIN));
    pkt.writeUInt8(roles); pkt.writeUInt8(0); pkt.writeUInt8(0);
    pkt.writeUInt8(1); pkt.writeUInt32(dungeonId); pkt.writeString("");
    owner_.socket->send(pkt);
}

void SocialHandler::lfgLeave() {
    if (!owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFG_LEAVE));
    pkt.writeUInt32(0); pkt.writeUInt32(0); pkt.writeUInt32(0);
    owner_.socket->send(pkt);
    lfgState_ = LfgState::None;
}

void SocialHandler::lfgSetRoles(uint8_t roles) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    const uint32_t wire = wireOpcode(Opcode::CMSG_LFG_SET_ROLES);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt8(roles);
    owner_.socket->send(pkt);
}

void SocialHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (!owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFG_PROPOSAL_RESULT));
    pkt.writeUInt32(proposalId); pkt.writeUInt8(accept ? 1 : 0);
    owner_.socket->send(pkt);
}

void SocialHandler::lfgTeleport(bool toLfgDungeon) {
    if (!owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFG_TELEPORT));
    pkt.writeUInt8(toLfgDungeon ? 0 : 1);
    owner_.socket->send(pkt);
}

void SocialHandler::lfgSetBootVote(bool vote) {
    if (!owner_.socket) return;
    uint16_t wireOp = wireOpcode(Opcode::CMSG_LFG_SET_BOOT_VOTE);
    if (wireOp == 0xFFFF) return;
    network::Packet pkt(wireOp);
    pkt.writeUInt8(vote ? 1 : 0);
    owner_.socket->send(pkt);
}

// ============================================================
// Arena Handlers
// ============================================================

void SocialHandler::handleArenaTeamCommandResult(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 8) return;
    uint32_t command = packet.readUInt32();
    std::string name = packet.readString();
    uint32_t error = packet.readUInt32();
    static const char* commands[] = {"create","invite","leave","remove","disband","leader"};
    std::string cmdName = (command < 6) ? commands[command] : "unknown";
    if (error == 0) owner_.addSystemChatMessage("Arena team " + cmdName + " successful" + (name.empty() ? "." : ": " + name));
    else owner_.addSystemChatMessage("Arena team " + cmdName + " failed" + (name.empty() ? "." : " for " + name + "."));
}

void SocialHandler::handleArenaTeamQueryResponse(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t teamId = packet.readUInt32();
    std::string teamName = packet.readString();
    uint32_t teamType = 0;
    if (packet.getSize() - packet.getReadPos() >= 4) teamType = packet.readUInt32();
    for (auto& s : arenaTeamStats_) { if (s.teamId == teamId) { s.teamName = teamName; s.teamType = teamType; return; } }
    ArenaTeamStats stub; stub.teamId = teamId; stub.teamName = teamName; stub.teamType = teamType;
    arenaTeamStats_.push_back(std::move(stub));
}

void SocialHandler::handleArenaTeamRoster(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 9) return;
    uint32_t teamId = packet.readUInt32();
    packet.readUInt8();
    uint32_t memberCount = std::min(packet.readUInt32(), 100u);
    ArenaTeamRoster roster; roster.teamId = teamId; roster.members.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (packet.getSize() - packet.getReadPos() < 12) break;
        ArenaTeamMember m;
        m.guid = packet.readUInt64(); m.online = (packet.readUInt8() != 0); m.name = packet.readString();
        if (packet.getSize() - packet.getReadPos() < 20) break;
        m.weekGames = packet.readUInt32(); m.weekWins = packet.readUInt32();
        m.seasonGames = packet.readUInt32(); m.seasonWins = packet.readUInt32(); m.personalRating = packet.readUInt32();
        if (packet.getSize() - packet.getReadPos() >= 8) { packet.readFloat(); packet.readFloat(); }
        roster.members.push_back(std::move(m));
    }
    for (auto& r : arenaTeamRosters_) { if (r.teamId == teamId) { r = std::move(roster); return; } }
    arenaTeamRosters_.push_back(std::move(roster));
}

void SocialHandler::handleArenaTeamInvite(network::Packet& packet) {
    std::string playerName = packet.readString();
    std::string teamName = packet.readString();
    owner_.addSystemChatMessage(playerName + " has invited you to join " + teamName + ".");
}

void SocialHandler::handleArenaTeamEvent(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t event = packet.readUInt8();
    uint8_t strCount = 0;
    if (packet.getSize() - packet.getReadPos() >= 1) strCount = packet.readUInt8();
    std::string param1, param2;
    if (strCount >= 1 && packet.getSize() > packet.getReadPos()) param1 = packet.readString();
    if (strCount >= 2 && packet.getSize() > packet.getReadPos()) param2 = packet.readString();
    std::string msg;
    switch (event) {
        case 0: msg = param1.empty() ? "A player has joined your arena team." : param1 + " has joined your arena team."; break;
        case 1: msg = param1.empty() ? "A player has left the arena team." : param1 + " has left the arena team."; break;
        case 2: msg = (!param1.empty() && !param2.empty()) ? param1 + " has been removed from the arena team by " + param2 + "." : "A player has been removed from the arena team."; break;
        case 3: msg = param1.empty() ? "The arena team captain has changed." : param1 + " is now the arena team captain."; break;
        case 4: msg = "Your arena team has been disbanded."; break;
        case 5: msg = param1.empty() ? "Your arena team has been created." : "Arena team \"" + param1 + "\" has been created."; break;
        default: msg = "Arena team event " + std::to_string(event); if (!param1.empty()) msg += ": " + param1; break;
    }
    owner_.addSystemChatMessage(msg);
}

void SocialHandler::handleArenaTeamStats(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 28) return;
    ArenaTeamStats stats;
    stats.teamId = packet.readUInt32(); stats.rating = packet.readUInt32();
    stats.weekGames = packet.readUInt32(); stats.weekWins = packet.readUInt32();
    stats.seasonGames = packet.readUInt32(); stats.seasonWins = packet.readUInt32();
    stats.rank = packet.readUInt32();
    for (auto& s : arenaTeamStats_) {
        if (s.teamId == stats.teamId) { stats.teamName = std::move(s.teamName); stats.teamType = s.teamType; s = std::move(stats); return; }
    }
    arenaTeamStats_.push_back(std::move(stats));
}

void SocialHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (!owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_ARENA_TEAM_ROSTER));
    pkt.writeUInt32(teamId);
    owner_.socket->send(pkt);
}

void SocialHandler::handleArenaError(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t error = packet.readUInt32();
    std::string msg;
    switch (error) {
        case 1: msg = "The other team is not big enough."; break;
        case 2: msg = "That team is full."; break;
        case 3: msg = "Not enough members to start."; break;
        case 4: msg = "Too many members."; break;
        default: msg = "Arena error (code " + std::to_string(error) + ")"; break;
    }
    owner_.addSystemChatMessage(msg);
}

void SocialHandler::handlePvpLogData(network::Packet& packet) {
    auto remaining = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (remaining() < 1) return;
    bgScoreboard_ = BgScoreboardData{};
    bgScoreboard_.isArena = (packet.readUInt8() != 0);
    if (bgScoreboard_.isArena) {
        for (int t = 0; t < 2; ++t) {
            if (remaining() < 20) { packet.setReadPos(packet.getSize()); return; }
            bgScoreboard_.arenaTeams[t].ratingChange = packet.readUInt32();
            bgScoreboard_.arenaTeams[t].newRating = packet.readUInt32();
            packet.readUInt32(); packet.readUInt32(); packet.readUInt32();
            bgScoreboard_.arenaTeams[t].teamName = remaining() > 0 ? packet.readString() : "";
        }
    }
    if (remaining() < 4) return;
    uint32_t playerCount = packet.readUInt32();
    bgScoreboard_.players.reserve(playerCount);
    for (uint32_t i = 0; i < playerCount && remaining() >= 13; ++i) {
        BgPlayerScore ps;
        ps.guid = packet.readUInt64(); ps.team = packet.readUInt8();
        ps.killingBlows = packet.readUInt32(); ps.honorableKills = packet.readUInt32();
        ps.deaths = packet.readUInt32(); ps.bonusHonor = packet.readUInt32();
        { auto ent = owner_.entityManager.getEntity(ps.guid);
          if (ent && (ent->getType() == game::ObjectType::PLAYER || ent->getType() == game::ObjectType::UNIT))
              { auto u = std::static_pointer_cast<game::Unit>(ent); if (!u->getName().empty()) ps.name = u->getName(); } }
        if (remaining() < 4) { bgScoreboard_.players.push_back(std::move(ps)); break; }
        uint32_t statCount = packet.readUInt32();
        for (uint32_t s = 0; s < statCount && remaining() >= 5; ++s) {
            std::string fieldName;
            while (remaining() > 0) { char c = static_cast<char>(packet.readUInt8()); if (c == '\0') break; fieldName += c; }
            uint32_t val = (remaining() >= 4) ? packet.readUInt32() : 0;
            ps.bgStats.emplace_back(std::move(fieldName), val);
        }
        bgScoreboard_.players.push_back(std::move(ps));
    }
    if (remaining() >= 1) {
        bgScoreboard_.hasWinner = (packet.readUInt8() != 0);
        if (bgScoreboard_.hasWinner && remaining() >= 1) bgScoreboard_.winner = packet.readUInt8();
    }
}

void SocialHandler::updateLogoutCountdown(float deltaTime) {
    if (loggingOut_ && logoutCountdown_ > 0.0f) {
        logoutCountdown_ -= deltaTime;
        if (logoutCountdown_ < 0.0f) logoutCountdown_ = 0.0f;
    }
}

void SocialHandler::resetTransferState() {
    encounterUnitGuids_.fill(0);
    raidTargetGuids_.fill(0);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void SocialHandler::handleInitializeFactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    size_t needed = static_cast<size_t>(count) * 5;
    if (!packet.hasRemaining(needed)) { packet.skipAll(); return; }
    owner_.initialFactions_.clear();
    owner_.initialFactions_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        GameHandler::FactionStandingInit fs{};
        fs.flags = packet.readUInt8();
        fs.standing = static_cast<int32_t>(packet.readUInt32());
        owner_.initialFactions_.push_back(fs);
    }
}

void SocialHandler::handleSetFactionStanding(network::Packet& packet) {
    if (!packet.hasRemaining(5)) return;
    /*uint8_t showVisual =*/ packet.readUInt8();
    uint32_t count = packet.readUInt32();
    count = std::min(count, 128u);
    owner_.loadFactionNameCache();
    for (uint32_t i = 0; i < count && packet.hasRemaining(8); ++i) {
        uint32_t factionId = packet.readUInt32();
        int32_t  standing  = static_cast<int32_t>(packet.readUInt32());
        int32_t  oldStanding = 0;
        auto it = owner_.factionStandings_.find(factionId);
        if (it != owner_.factionStandings_.end()) oldStanding = it->second;
        owner_.factionStandings_[factionId] = standing;
        int32_t delta = standing - oldStanding;
        if (delta != 0) {
            std::string name = owner_.getFactionName(factionId);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "Reputation with %s %s by %d.",
                          name.c_str(), delta > 0 ? "increased" : "decreased", std::abs(delta));
            owner_.addSystemChatMessage(buf);
            owner_.watchedFactionId_ = factionId;
            if (owner_.repChangeCallback_) owner_.repChangeCallback_(name, delta, standing);
                owner_.fireAddonEvent("UPDATE_FACTION", {});
                owner_.fireAddonEvent("CHAT_MSG_COMBAT_FACTION_CHANGE", {std::string(buf)});
        }
    }
}

void SocialHandler::handleSetFactionAtWar(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  setAtWar  = packet.readUInt8();
    if (repListId < owner_.initialFactions_.size()) {
        if (setAtWar)
            owner_.initialFactions_[repListId].flags |=  GameHandler::FACTION_FLAG_AT_WAR;
        else
            owner_.initialFactions_[repListId].flags &= ~GameHandler::FACTION_FLAG_AT_WAR;
    }
}

void SocialHandler::handleSetFactionVisible(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  visible   = packet.readUInt8();
    if (repListId < owner_.initialFactions_.size()) {
        if (visible)
            owner_.initialFactions_[repListId].flags |=  GameHandler::FACTION_FLAG_VISIBLE;
        else
            owner_.initialFactions_[repListId].flags &= ~GameHandler::FACTION_FLAG_VISIBLE;
    }
}

void SocialHandler::handleGroupSetLeader(network::Packet& packet) {
    if (!packet.hasData()) return;
    std::string leaderName = packet.readString();
    auto& pd = mutablePartyData();
    for (const auto& m : pd.members) {
        if (m.name == leaderName) { pd.leaderGuid = m.guid; break; }
    }
    if (!leaderName.empty())
        owner_.addSystemChatMessage(leaderName + " is now the group leader.");
    owner_.fireAddonEvent("PARTY_LEADER_CHANGED", {});
    owner_.fireAddonEvent("GROUP_ROSTER_UPDATE", {});
}

// ============================================================
// Minimap Ping
// ============================================================

void SocialHandler::sendMinimapPing(float wowX, float wowY) {
    if (owner_.state != WorldState::IN_WORLD) return;

    // MSG_MINIMAP_PING (CMSG direction): float posX + float posY
    // Server convention: posX = east/west axis = canonical Y (west)
    //                    posY = north/south axis = canonical X (north)
    const float serverX = wowY;  // canonical Y (west) → server posX
    const float serverY = wowX;  // canonical X (north) → server posY

    network::Packet pkt(wireOpcode(Opcode::MSG_MINIMAP_PING));
    pkt.writeFloat(serverX);
    pkt.writeFloat(serverY);
    owner_.socket->send(pkt);

    // Add ping locally so the sender sees their own ping immediately
    GameHandler::MinimapPing localPing;
    localPing.senderGuid = owner_.activeCharacterGuid_;
    localPing.wowX       = wowX;
    localPing.wowY       = wowY;
    localPing.age        = 0.0f;
    owner_.minimapPings_.push_back(localPing);
}

// ============================================================
// Summon Request
// ============================================================

void SocialHandler::handleSummonRequest(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;

    owner_.summonerGuid_        = packet.readUInt64();
    uint32_t zoneId             = packet.readUInt32();
    uint32_t timeoutMs          = packet.readUInt32();
    owner_.summonTimeoutSec_    = timeoutMs / 1000.0f;
    owner_.pendingSummonRequest_= true;

    owner_.summonerName_.clear();
    if (auto* unit = owner_.getUnitByGuid(owner_.summonerGuid_)) {
        owner_.summonerName_ = unit->getName();
    }
    if (owner_.summonerName_.empty()) {
        owner_.summonerName_ = owner_.lookupName(owner_.summonerGuid_);
    }
    if (owner_.summonerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(owner_.summonerGuid_));
        owner_.summonerName_ = tmp;
    }

    std::string msg = owner_.summonerName_ + " is summoning you";
    std::string zoneName = owner_.getAreaName(zoneId);
    if (!zoneName.empty())
        msg += " to " + zoneName;
    msg += '.';
    owner_.addSystemChatMessage(msg);
    LOG_INFO("SMSG_SUMMON_REQUEST: summoner=", owner_.summonerName_,
             " zoneId=", zoneId, " timeout=", owner_.summonTimeoutSec_, "s");
    owner_.fireAddonEvent("CONFIRM_SUMMON", {});
}

void SocialHandler::acceptSummon() {
    if (!owner_.pendingSummonRequest_ || !owner_.socket) return;
    owner_.pendingSummonRequest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SUMMON_RESPONSE));
    pkt.writeUInt8(1);  // 1 = accept
    owner_.socket->send(pkt);
    owner_.addSystemChatMessage("Accepting summon...");
    LOG_INFO("Accepted summon from ", owner_.summonerName_);
}

void SocialHandler::declineSummon() {
    if (!owner_.socket) return;
    owner_.pendingSummonRequest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SUMMON_RESPONSE));
    pkt.writeUInt8(0);  // 0 = decline
    owner_.socket->send(pkt);
    owner_.addSystemChatMessage("Summon declined.");
}

// ============================================================
// Battlefield Manager
// ============================================================

void SocialHandler::acceptBfMgrInvite() {
    if (!owner_.bfMgrInvitePending_ || owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    // CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE: uint8 accepted = 1
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE));
    pkt.writeUInt8(1);  // accepted
    owner_.socket->send(pkt);
    owner_.bfMgrInvitePending_ = false;
    LOG_INFO("acceptBfMgrInvite: sent CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE accepted=1");
}

void SocialHandler::declineBfMgrInvite() {
    if (!owner_.bfMgrInvitePending_ || owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    // CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE: uint8 accepted = 0
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE));
    pkt.writeUInt8(0);  // declined
    owner_.socket->send(pkt);
    owner_.bfMgrInvitePending_ = false;
    LOG_INFO("declineBfMgrInvite: sent CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE accepted=0");
}

// ============================================================
// Calendar
// ============================================================

void SocialHandler::requestCalendar() {
    if (!owner_.isInWorld()) return;
    // CMSG_CALENDAR_GET_CALENDAR has no payload
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_CALENDAR));
    owner_.socket->send(pkt);
    LOG_INFO("requestCalendar: sent CMSG_CALENDAR_GET_CALENDAR");
    // Also request pending invite count
    network::Packet numPkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));
    owner_.socket->send(numPkt);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void SocialHandler::sendSetDifficulty(uint32_t difficulty) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change difficulty: not in world");
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_CHANGEPLAYER_DIFFICULTY));
    packet.writeUInt32(difficulty);
    owner_.socket->send(packet);
    LOG_INFO("CMSG_CHANGEPLAYER_DIFFICULTY sent: difficulty=", difficulty);
}

void SocialHandler::toggleHelm() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle helm: not in world or not connected");
        return;
    }

    owner_.helmVisible_ = !owner_.helmVisible_;
    auto packet = ShowingHelmPacket::build(owner_.helmVisible_);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage(owner_.helmVisible_ ? "Helm is now visible." : "Helm is now hidden.");
    LOG_INFO("Helm visibility toggled: ", owner_.helmVisible_);
}

void SocialHandler::toggleCloak() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle cloak: not in world or not connected");
        return;
    }

    owner_.cloakVisible_ = !owner_.cloakVisible_;
    auto packet = ShowingCloakPacket::build(owner_.cloakVisible_);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage(owner_.cloakVisible_ ? "Cloak is now visible." : "Cloak is now hidden.");
    LOG_INFO("Cloak visibility toggled: ", owner_.cloakVisible_);
}

void SocialHandler::setStandState(uint8_t standState) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change stand state: not in world or not connected");
        return;
    }

    auto packet = StandStateChangePacket::build(standState);
    owner_.socket->send(packet);
    LOG_INFO("Changed stand state to: ", static_cast<int>(standState));
}

void SocialHandler::sendAlterAppearance(uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair) {
    if (!owner_.isInWorld()) return;
    auto pkt = AlterAppearancePacket::build(hairStyle, hairColor, facialHair);
    owner_.socket->send(pkt);
    LOG_INFO("sendAlterAppearance: hair=", hairStyle, " color=", hairColor, " facial=", facialHair);
}

void SocialHandler::deleteGmTicket() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_DELETETICKET));
    owner_.socket->send(pkt);
    owner_.gmTicketActive_ = false;
    owner_.gmTicketText_.clear();
    LOG_INFO("Deleting GM ticket");
}

void SocialHandler::requestGmTicket() {
    if (!owner_.isInWorld()) return;
    // CMSG_GMTICKET_GETTICKET has no payload — server responds with SMSG_GMTICKET_GETTICKET
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_GETTICKET));
    owner_.socket->send(pkt);
    LOG_DEBUG("Sent CMSG_GMTICKET_GETTICKET — querying open ticket status");
}

} // namespace game
} // namespace wowee
