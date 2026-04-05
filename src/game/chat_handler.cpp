#include "game/chat_handler.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee {
namespace game {

ChatHandler::ChatHandler(GameHandler& owner)
    : owner_(owner) {}

void ChatHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet);
    };
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        // SMSG_GM_MESSAGECHAT has the same header as SMSG_MESSAGECHAT
        // (type[1]+lang[4]+senderGuid[8]+unk[4] = 17 bytes) followed by an
        // extra gmNameLen[4]+gmName[N] before the type-specific body.
        // Strip the GM name field to produce standard SMSG_MESSAGECHAT format.
        if (!packet.hasRemaining(21)) return; // 17 header + 4 gmNameLen min
        uint8_t  type       = packet.readUInt8();
        uint32_t lang       = packet.readUInt32();
        uint64_t senderGuid = packet.readUInt64();
        uint32_t unk        = packet.readUInt32();
        uint32_t gmNameLen  = packet.readUInt32();
        if (!packet.hasRemaining(gmNameLen)) return;
        packet.setReadPos(packet.getReadPos() + gmNameLen); // skip gmName

        // Rebuild as regular SMSG_MESSAGECHAT (header + remaining body)
        network::Packet regular(0);
        regular.writeUInt8(type);
        regular.writeUInt32(lang);
        regular.writeUInt64(senderGuid);
        regular.writeUInt32(unk);
        const auto& raw = packet.getData();
        size_t pos = packet.getReadPos();
        if (pos < raw.size())
            regular.writeBytes(raw.data() + pos, raw.size() - pos);
        handleMessageChat(regular);
    };
    table[Opcode::SMSG_TEXT_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleTextEmote(packet);
    };
    table[Opcode::SMSG_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!packet.hasRemaining(12)) return;
        uint32_t emoteAnim  = packet.readUInt32();
        uint64_t sourceGuid = packet.readUInt64();
        if (owner_.emoteAnimCallback_ && sourceGuid != 0)
            owner_.emoteAnimCallback_(sourceGuid, emoteAnim);
    };
    table[Opcode::SMSG_CHANNEL_NOTIFY] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD ||
            owner_.getState() == WorldState::ENTERING_WORLD)
            handleChannelNotify(packet);
    };
    table[Opcode::SMSG_CHAT_PLAYER_NOT_FOUND] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) addSystemChatMessage("No player named '" + name + "' is currently playing.");
    };
    table[Opcode::SMSG_CHAT_PLAYER_AMBIGUOUS] = [this](network::Packet& packet) {
        std::string name = packet.readString();
        if (!name.empty()) addSystemChatMessage("Player name '" + name + "' is ambiguous.");
    };
    table[Opcode::SMSG_CHAT_WRONG_FACTION] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You cannot send messages to members of that faction.");
        addSystemChatMessage("You cannot send messages to members of that faction.");
    };
    table[Opcode::SMSG_CHAT_NOT_IN_PARTY] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You are not in a party.");
        addSystemChatMessage("You are not in a party.");
    };
    table[Opcode::SMSG_CHAT_RESTRICTED] = [this](network::Packet& /*packet*/) {
        owner_.addUIError("You cannot send chat messages in this area.");
        addSystemChatMessage("You cannot send chat messages in this area.");
    };

    // ---- Channel list ----

    // ---- Server / defense / area-trigger messages (moved from GameHandler) ----
    table[Opcode::SMSG_DEFENSE_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(5)) {
            /*uint32_t zoneId =*/ packet.readUInt32();
            std::string defMsg = packet.readString();
            if (!defMsg.empty()) addSystemChatMessage("[Defense] " + defMsg);
        }
    };
    // Server messages
    table[Opcode::SMSG_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t msgType = packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                std::string prefix;
                switch (msgType) {
                    case 1: prefix = "[Shutdown] ";   owner_.addUIError("Server shutdown: " + msg);  break;
                    case 2: prefix = "[Restart] ";    owner_.addUIError("Server restart: " + msg);   break;
                    case 4: prefix = "[Shutdown cancelled] "; break;
                    case 5: prefix = "[Restart cancelled] ";  break;
                    default: prefix = "[Server] "; break;
                }
                addSystemChatMessage(prefix + msg);
            }
        }
    };
    table[Opcode::SMSG_CHAT_SERVER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t msgType =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) addSystemChatMessage("[Announcement] " + msg);
        }
    };
    table[Opcode::SMSG_AREA_TRIGGER_MESSAGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            /*uint32_t len =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) {
                owner_.addUIError(msg);
                addSystemChatMessage(msg);
                owner_.areaTriggerMsgs_.push_back(msg);
            }
        }
    };

    table[Opcode::SMSG_CHANNEL_LIST] = [this](network::Packet& p) { handleChannelList(p); };
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }

    if (message.empty()) {
        LOG_WARNING("Cannot send empty chat message");
        return;
    }

    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");

    // Use the player's faction language. AzerothCore rejects wrong language.
    // Alliance races: Human(1), Dwarf(3), NightElf(4), Gnome(7), Draenei(11) → COMMON (7)
    // Horde races: Orc(2), Undead(5), Tauren(6), Troll(8), BloodElf(10) → ORCISH (1)
    uint8_t race = owner_.getPlayerRace();
    bool isHorde = (race == 2 || race == 5 || race == 6 || race == 8 || race == 10);
    ChatLanguage language = isHorde ? ChatLanguage::ORCISH : ChatLanguage::COMMON;

    auto packet = MessageChatPacket::build(type, language, message, target);
    owner_.socket->send(packet);

    // Add local echo so the player sees their own message immediately
    MessageChatData echo;
    echo.senderGuid = owner_.playerGuid;
    echo.language = language;
    echo.message = message;

    auto nameIt = owner_.getPlayerNameCache().find(owner_.playerGuid);
    if (nameIt != owner_.getPlayerNameCache().end()) {
        echo.senderName = nameIt->second;
    }

    if (type == ChatType::WHISPER) {
        echo.type = ChatType::WHISPER_INFORM;
        echo.senderName = target;
    } else {
        echo.type = type;
    }

    if (type == ChatType::CHANNEL) {
        echo.channelName = target;
    }

    addLocalChatMessage(echo);
}

void ChatHandler::handleMessageChat(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");

    MessageChatData data;
    if (!owner_.packetParsers_->parseMessageChat(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MESSAGECHAT, size=", packet.getSize());
        return;
    }
    LOG_INFO("INCOMING CHAT: type=", static_cast<int>(data.type),
             " (", getChatTypeString(data.type), ") sender=0x", std::hex, data.senderGuid, std::dec,
             " '", data.senderName, "' msg='", data.message.substr(0, 60), "'");

    // Skip server echo of our own messages (we already added a local echo)
    if (data.senderGuid == owner_.playerGuid && data.senderGuid != 0) {
        if (data.type == ChatType::WHISPER && !data.senderName.empty()) {
            owner_.lastWhisperSender_ = data.senderName;
        }
        return;
    }

    // Resolve sender name from entity/cache if not already set by parser
    if (data.senderName.empty() && data.senderGuid != 0) {
        auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
        if (nameIt != owner_.getPlayerNameCache().end()) {
            data.senderName = nameIt->second;
        } else {
            auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
            if (entity) {
                if (entity->getType() == ObjectType::PLAYER) {
                    auto player = std::dynamic_pointer_cast<Player>(entity);
                    if (player && !player->getName().empty()) {
                        data.senderName = player->getName();
                    }
                } else if (entity->getType() == ObjectType::UNIT) {
                    auto unit = std::dynamic_pointer_cast<Unit>(entity);
                    if (unit && !unit->getName().empty()) {
                        data.senderName = unit->getName();
                    }
                }
            }
        }

        if (data.senderName.empty()) {
            owner_.queryPlayerName(data.senderGuid);
        }
    }

    // Filter BG queue announcer spam (server-side module on ChromieCraft/AzerothCore).
    // Arrives as SAY (type=0) with color codes: |cffff0000[BG Queue Announcer]:|r ...
    {
        const auto& msg = data.message;
        if (msg.find("BG Queue Announcer") != std::string::npos ||
            msg.find("Queue status") != std::string::npos) {
            return;
        }
    }

    // Filter officer chat if player doesn't have officer chat permission.
    // Some servers send officer chat to all guild members regardless of rank.
    // WoW guild right bit 0x40 = GR_RIGHT_OFFCHATSPEAK, 0x80 = GR_RIGHT_OFFCHATLISTEN
    if (data.type == ChatType::OFFICER) {
        const auto& roster = owner_.getGuildRoster();
        uint64_t myGuid = owner_.getPlayerGuid();
        uint32_t myRankIdx = 0;
        for (const auto& m : roster.members) {
            if (m.guid == myGuid) { myRankIdx = m.rankIndex; break; }
        }
        if (myRankIdx < roster.ranks.size()) {
            uint32_t rights = roster.ranks[myRankIdx].rights;
            if (!(rights & 0x80)) { // GR_RIGHT_OFFCHATLISTEN = 0x80
                return; // Don't show officer chat to non-officers
            }
        }
    }

    // Filter addon-to-addon whispers (GearScore, DBM, oRA, etc.) from player chat.
    // These are invisible in the real WoW client.
    if (data.type == ChatType::WHISPER || data.type == ChatType::WHISPER_INFORM) {
        const auto& msg = data.message;
        if (msg.size() >= 3 && (
            msg.rfind("GS_", 0) == 0 ||          // GearScore
            msg.rfind("DVNE", 0) == 0 ||          // DBM (DeadlyBossMods)
            msg.rfind("oRA", 0) == 0 ||            // oRA raid addon
            msg.rfind("BWVQ", 0) == 0 ||           // BigWigs
            msg.rfind("AVR", 0) == 0 ||            // AVR (Augmented Virtual Reality)
            msg.rfind("\t", 0) == 0 ||             // Tab-prefixed addon messages
            (msg.size() > 4 && static_cast<unsigned char>(msg[0]) > 127))) {  // Binary data
            return; // Silently discard addon whisper
        }
    }

    // Add to chat history
    chatHistory_.push_back(data);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.erase(chatHistory_.begin());
    }

    // Track whisper sender for /r command
    if (data.type == ChatType::WHISPER) {
        // Always store GUID so getLastWhisperSender() can resolve the name
        // from the player name cache even if name wasn't available yet
        if (data.senderGuid != 0)
            owner_.lastWhisperSenderGuid_ = data.senderGuid;
        if (!data.senderName.empty())
            owner_.lastWhisperSender_ = data.senderName;

        if (!data.senderName.empty()) {
            // Only auto-reply once per sender per AFK/DND session to prevent loops
            if (owner_.afkStatus_ && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.afkMessage_.empty() ? "Away from Keyboard" : owner_.afkMessage_;
                sendChatMessage(ChatType::WHISPER, "<AFK> " + reply, data.senderName);
            } else if (owner_.dndStatus_ && afkAutoRepliedSenders_.insert(data.senderName).second) {
                std::string reply = owner_.dndMessage_.empty() ? "Do Not Disturb" : owner_.dndMessage_;
                sendChatMessage(ChatType::WHISPER, "<DND> " + reply, data.senderName);
            }
        }
    }

    // Trigger chat bubble for SAY/YELL messages from others
    if (owner_.chatBubbleCallback_ && data.senderGuid != 0) {
        if (data.type == ChatType::SAY || data.type == ChatType::YELL ||
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_PARTY) {
            bool isYell = (data.type == ChatType::YELL || data.type == ChatType::MONSTER_YELL);
            owner_.chatBubbleCallback_(data.senderGuid, data.message, isYell);
        }
    }

    // Log the message
    std::string senderInfo;
    if (!data.senderName.empty()) {
        senderInfo = data.senderName;
    } else if (data.senderGuid != 0) {
        senderInfo = "Unknown-" + std::to_string(data.senderGuid);
    } else {
        senderInfo = "System";
    }

    std::string channelInfo;
    if (!data.channelName.empty()) {
        channelInfo = "[" + data.channelName + "] ";
    }

    LOG_DEBUG("[", getChatTypeString(data.type), "] ", channelInfo, senderInfo, ": ", data.message);

    // Detect addon messages
    if (owner_.addonEventCallback_ &&
        data.type != ChatType::SAY && data.type != ChatType::YELL &&
        data.type != ChatType::EMOTE && data.type != ChatType::TEXT_EMOTE &&
        data.type != ChatType::MONSTER_SAY && data.type != ChatType::MONSTER_YELL) {
        auto tabPos = data.message.find('\t');
        if (tabPos != std::string::npos && tabPos > 0 && tabPos <= 16 &&
            tabPos < data.message.size() - 1) {
            std::string prefix = data.message.substr(0, tabPos);
            if (prefix.find(' ') == std::string::npos) {
                std::string body = data.message.substr(tabPos + 1);
                std::string channel = getChatTypeString(data.type);
                owner_.addonEventCallback_("CHAT_MSG_ADDON", {prefix, body, channel, data.senderName});
                return;
            }
        }
    }

    // Fire CHAT_MSG_* addon events
    if (owner_.addonChatCallback_) owner_.addonChatCallback_(data);
    if (owner_.addonEventCallback_) {
        std::string eventName = "CHAT_MSG_";
        eventName += getChatTypeString(data.type);
        std::string lang = std::to_string(static_cast<int>(data.language));
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)data.senderGuid);
        owner_.addonEventCallback_(eventName, {
            data.message,
            data.senderName,
            lang,
            data.channelName,
            senderInfo,
            "",
            "0",
            "0",
            "",
            "0",
            "0",
            guidBuf
        });
    }
}

void ChatHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = TextEmotePacket::build(textEmoteId, targetGuid);
    owner_.socket->send(packet);
}

void ChatHandler::handleTextEmote(network::Packet& packet) {
    const bool legacyFormat = isClassicLikeExpansion() || isActiveExpansion("tbc");
    TextEmoteData data;
    if (!TextEmoteParser::parse(packet, data, legacyFormat)) {
        LOG_WARNING("Failed to parse SMSG_TEXT_EMOTE");
        return;
    }

    if (data.senderGuid == owner_.playerGuid && data.senderGuid != 0) {
        return;
    }

    std::string senderName;
    auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
    if (nameIt != owner_.getPlayerNameCache().end()) {
        senderName = nameIt->second;
    } else {
        auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
        if (entity) {
            auto unit = std::dynamic_pointer_cast<Unit>(entity);
            if (unit) senderName = unit->getName();
        }
    }
    if (senderName.empty()) {
        senderName = "Unknown";
        owner_.queryPlayerName(data.senderGuid);
    }

    const std::string* targetPtr = data.targetName.empty() ? nullptr : &data.targetName;
    std::string emoteText = rendering::Renderer::getEmoteTextByDbcId(data.textEmoteId, senderName, targetPtr);
    if (emoteText.empty()) {
        emoteText = data.targetName.empty()
            ? senderName + " performs an emote."
            : senderName + " performs an emote at " + data.targetName + ".";
    }

    MessageChatData chatMsg;
    chatMsg.type = ChatType::TEXT_EMOTE;
    chatMsg.language = ChatLanguage::COMMON;
    chatMsg.senderGuid = data.senderGuid;
    chatMsg.senderName = senderName;
    chatMsg.message = emoteText;

    addLocalChatMessage(chatMsg);

    uint32_t animId = rendering::Renderer::getEmoteAnimByDbcId(data.textEmoteId);
    if (animId != 0 && owner_.emoteAnimCallback_) {
        owner_.emoteAnimCallback_(data.senderGuid, animId);
    }

    LOG_INFO("TEXT_EMOTE from ", senderName, " (emoteId=", data.textEmoteId, ", anim=", animId, ")");
}

void ChatHandler::joinChannel(const std::string& channelName, const std::string& password) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = owner_.packetParsers_
        ? owner_.packetParsers_->buildJoinChannel(channelName, password)
        : JoinChannelPacket::build(channelName, password);
    owner_.socket->send(packet);
    LOG_INFO("Requesting to join channel: ", channelName);
}

void ChatHandler::leaveChannel(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = owner_.packetParsers_
        ? owner_.packetParsers_->buildLeaveChannel(channelName)
        : LeaveChannelPacket::build(channelName);
    owner_.socket->send(packet);
    LOG_INFO("Requesting to leave channel: ", channelName);
}

std::string ChatHandler::getChannelByIndex(int index) const {
    if (index < 1 || index > static_cast<int>(joinedChannels_.size())) return "";
    return joinedChannels_[index - 1];
}

int ChatHandler::getChannelIndex(const std::string& channelName) const {
    for (int i = 0; i < static_cast<int>(joinedChannels_.size()); ++i) {
        if (joinedChannels_[i] == channelName) return i + 1;
    }
    return 0;
}

void ChatHandler::handleChannelNotify(network::Packet& packet) {
    ChannelNotifyData data;
    if (!ChannelNotifyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CHANNEL_NOTIFY");
        return;
    }

    switch (data.notifyType) {
        case ChannelNotifyType::YOU_JOINED: {
            if (std::find(joinedChannels_.begin(), joinedChannels_.end(), data.channelName) == joinedChannels_.end()) {
                joinedChannels_.push_back(data.channelName);
            }
            MessageChatData msg;
            msg.type = ChatType::SYSTEM;
            msg.message = "Joined channel: " + data.channelName;
            addLocalChatMessage(msg);
            LOG_INFO("Joined channel: ", data.channelName);
            break;
        }
        case ChannelNotifyType::YOU_LEFT: {
            joinedChannels_.erase(
                std::remove(joinedChannels_.begin(), joinedChannels_.end(), data.channelName),
                joinedChannels_.end());
            MessageChatData msg;
            msg.type = ChatType::SYSTEM;
            msg.message = "Left channel: " + data.channelName;
            addLocalChatMessage(msg);
            LOG_INFO("Left channel: ", data.channelName);
            break;
        }
        case ChannelNotifyType::PLAYER_ALREADY_MEMBER: {
            // Server confirms we're in this channel but our local list doesn't have it yet —
            // can happen after reconnect or if the join notification was missed.
            if (std::find(joinedChannels_.begin(), joinedChannels_.end(), data.channelName) == joinedChannels_.end()) {
                joinedChannels_.push_back(data.channelName);
                LOG_INFO("Already in channel: ", data.channelName);
            }
            break;
        }
        case ChannelNotifyType::NOT_IN_AREA:
            addSystemChatMessage("You must be in the area to join '" + data.channelName + "'.");
            LOG_DEBUG("Cannot join channel ", data.channelName, " (not in area)");
            break;
        case ChannelNotifyType::WRONG_PASSWORD:
            addSystemChatMessage("Wrong password for channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MEMBER:
            addSystemChatMessage("You are not in channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MODERATOR:
            addSystemChatMessage("You are not a moderator of '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::MUTED:
            addSystemChatMessage("You are muted in channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::BANNED:
            addSystemChatMessage("You are banned from channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::THROTTLED:
            addSystemChatMessage("Channel '" + data.channelName + "' is throttled. Please wait.");
            break;
        case ChannelNotifyType::NOT_IN_LFG:
            addSystemChatMessage("You must be in a LFG queue to join '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_KICKED:
            addSystemChatMessage("A player was kicked from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PASSWORD_CHANGED:
            addSystemChatMessage("Password for '" + data.channelName + "' changed.");
            break;
        case ChannelNotifyType::OWNER_CHANGED:
            addSystemChatMessage("Owner of '" + data.channelName + "' changed.");
            break;
        case ChannelNotifyType::NOT_OWNER:
            addSystemChatMessage("You are not the owner of '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVALID_NAME:
            addSystemChatMessage("Invalid channel name '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_NOT_FOUND:
            addSystemChatMessage("Player not found.");
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_ON:
            addSystemChatMessage("Channel '" + data.channelName + "': announcements enabled.");
            break;
        case ChannelNotifyType::ANNOUNCEMENTS_OFF:
            addSystemChatMessage("Channel '" + data.channelName + "': announcements disabled.");
            break;
        case ChannelNotifyType::MODERATION_ON:
            addSystemChatMessage("Channel '" + data.channelName + "' is now moderated.");
            break;
        case ChannelNotifyType::MODERATION_OFF:
            addSystemChatMessage("Channel '" + data.channelName + "' is no longer moderated.");
            break;
        case ChannelNotifyType::PLAYER_BANNED:
            addSystemChatMessage("A player was banned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_UNBANNED:
            addSystemChatMessage("A player was unbanned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_NOT_BANNED:
            addSystemChatMessage("That player is not banned from '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVITE:
            addSystemChatMessage("You have been invited to join channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::INVITE_WRONG_FACTION:
        case ChannelNotifyType::WRONG_FACTION:
            addSystemChatMessage("Wrong faction for channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::NOT_MODERATED:
            addSystemChatMessage("Channel '" + data.channelName + "' is not moderated.");
            break;
        case ChannelNotifyType::PLAYER_INVITED:
            addSystemChatMessage("Player invited to channel '" + data.channelName + "'.");
            break;
        case ChannelNotifyType::PLAYER_INVITE_BANNED:
            addSystemChatMessage("That player is banned from '" + data.channelName + "'.");
            break;
        default:
            LOG_DEBUG("Channel notify type ", static_cast<int>(data.notifyType),
                     " for channel ", data.channelName);
            break;
    }
}

void ChatHandler::autoJoinDefaultChannels() {
    LOG_INFO("autoJoinDefaultChannels: general=", chatAutoJoin.general,
             " trade=", chatAutoJoin.trade, " localDefense=", chatAutoJoin.localDefense,
             " lfg=", chatAutoJoin.lfg, " local=", chatAutoJoin.local);
    if (chatAutoJoin.general) joinChannel("General");
    if (chatAutoJoin.trade) joinChannel("Trade");
    if (chatAutoJoin.localDefense) joinChannel("LocalDefense");
    if (chatAutoJoin.lfg) joinChannel("LookingForGroup");
    if (chatAutoJoin.local) joinChannel("Local");
}

void ChatHandler::addLocalChatMessage(const MessageChatData& msg) {
    chatHistory_.push_back(msg);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.pop_front();
    }
    if (owner_.addonChatCallback_) owner_.addonChatCallback_(msg);

    if (owner_.addonEventCallback_) {
        std::string eventName = "CHAT_MSG_";
        eventName += getChatTypeString(msg.type);
        const Character* ac = owner_.getActiveCharacter();
        std::string senderName = msg.senderName.empty()
            ? (ac ? ac->name : std::string{}) : msg.senderName;
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX",
                 (unsigned long long)(msg.senderGuid != 0 ? msg.senderGuid : owner_.playerGuid));
        owner_.addonEventCallback_(eventName, {
            msg.message, senderName,
            std::to_string(static_cast<int>(msg.language)),
            msg.channelName, senderName, "", "0", "0", "", "0", "0", guidBuf
        });
    }
}

void ChatHandler::addSystemChatMessage(const std::string& message) {
    if (message.empty()) return;
    MessageChatData msg;
    msg.type = ChatType::SYSTEM;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.message = message;
    addLocalChatMessage(msg);
}

void ChatHandler::toggleAfk(const std::string& message) {
    owner_.afkStatus_ = !owner_.afkStatus_;
    owner_.afkMessage_ = message;

    if (owner_.afkStatus_) {
        if (message.empty()) {
            addSystemChatMessage("You are now AFK.");
        } else {
            addSystemChatMessage("You are now AFK: " + message);
        }
        // If DND was active, turn it off
        if (owner_.dndStatus_) {
            owner_.dndStatus_ = false;
            owner_.dndMessage_.clear();
        }
    } else {
        addSystemChatMessage("You are no longer AFK.");
        owner_.afkMessage_.clear();
        afkAutoRepliedSenders_.clear();
    }

    LOG_INFO("AFK status: ", owner_.afkStatus_, ", message: ", message);
}

void ChatHandler::toggleDnd(const std::string& message) {
    owner_.dndStatus_ = !owner_.dndStatus_;
    owner_.dndMessage_ = message;

    if (owner_.dndStatus_) {
        if (message.empty()) {
            addSystemChatMessage("You are now DND (Do Not Disturb).");
        } else {
            addSystemChatMessage("You are now DND: " + message);
        }
        // If AFK was active, turn it off
        if (owner_.afkStatus_) {
            owner_.afkStatus_ = false;
            owner_.afkMessage_.clear();
        }
    } else {
        addSystemChatMessage("You are no longer DND.");
        owner_.dndMessage_.clear();
        afkAutoRepliedSenders_.clear();
    }

    LOG_INFO("DND status: ", owner_.dndStatus_, ", message: ", message);
}

void ChatHandler::replyToLastWhisper(const std::string& message) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot send whisper: not in world or not connected");
        return;
    }

    if (owner_.lastWhisperSender_.empty()) {
        addSystemChatMessage("No one has whispered you yet.");
        return;
    }

    if (message.empty()) {
        addSystemChatMessage("You must specify a message to send.");
        return;
    }

    // Send whisper using the standard message chat function
    sendChatMessage(ChatType::WHISPER, message, owner_.lastWhisperSender_);
    LOG_INFO("Replied to ", owner_.lastWhisperSender_, ": ", message);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void ChatHandler::handleChannelList(network::Packet& packet) {
    std::string chanName = packet.readString();
    if (!packet.hasRemaining(5)) return;
    /*uint8_t chanFlags =*/ packet.readUInt8();
    uint32_t memberCount = packet.readUInt32();
    memberCount = std::min(memberCount, 200u);
    addSystemChatMessage(chanName + " has " + std::to_string(memberCount) + " member(s):");
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(9)) break;
        uint64_t memberGuid = packet.readUInt64();
        uint8_t memberFlags = packet.readUInt8();
        std::string name;
        auto entity = owner_.getEntityManager().getEntity(memberGuid);
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) name = player->getName();
        }
        if (name.empty()) name = owner_.lookupName(memberGuid);
        if (name.empty()) name = "(unknown)";
        std::string entry = "  " + name;
        if (memberFlags & 0x01) entry += " [Moderator]";
        if (memberFlags & 0x02) entry += " [Muted]";
        addSystemChatMessage(entry);
    }
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void ChatHandler::submitGmTicket(const std::string& text) {
    if (!owner_.isInWorld()) return;

    // CMSG_GMTICKET_CREATE (WotLK 3.3.5a):
    // string   ticket_text
    // float[3] position (server coords)
    // float    facing
    // uint32   mapId
    // uint8    need_response (1 = yes)
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_CREATE));
    pkt.writeString(text);
    pkt.writeFloat(owner_.movementInfo.x);
    pkt.writeFloat(owner_.movementInfo.y);
    pkt.writeFloat(owner_.movementInfo.z);
    pkt.writeFloat(owner_.movementInfo.orientation);
    pkt.writeUInt32(owner_.currentMapId_);
    pkt.writeUInt8(1);  // need_response = yes
    owner_.socket->send(pkt);
    LOG_INFO("Submitted GM ticket: '", text, "'");
}

void ChatHandler::handleMotd(network::Packet& packet) {
    LOG_INFO("Handling SMSG_MOTD");

    MotdData data;
    if (!MotdParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MOTD");
        return;
    }

    if (!data.isEmpty()) {
        LOG_INFO("========================================");
        LOG_INFO("   MESSAGE OF THE DAY");
        LOG_INFO("========================================");
        for (const auto& line : data.lines) {
            LOG_INFO(line);
            addSystemChatMessage(std::string("MOTD: ") + line);
        }
        // Add a visual separator after MOTD block so subsequent messages don't
        // appear glued to the last MOTD line.
        MessageChatData spacer;
        spacer.type = ChatType::SYSTEM;
        spacer.language = ChatLanguage::UNIVERSAL;
        spacer.message = "";
        addLocalChatMessage(spacer);
        LOG_INFO("========================================");
    }
}

void ChatHandler::handleNotification(network::Packet& packet) {
    // SMSG_NOTIFICATION: single null-terminated string
    std::string message = packet.readString();
    if (!message.empty()) {
        LOG_INFO("Server notification: ", message);
        addSystemChatMessage(message);
    }
}

} // namespace game
} // namespace wowee
