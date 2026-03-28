#include "game/inventory_handler.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "rendering/renderer.hpp"
#include "audio/ui_sound_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "pipeline/dbc_layout.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <sstream>

namespace wowee {
namespace game {

// Free functions defined in game_handler.cpp
std::string buildItemLink(uint32_t itemId, uint32_t quality, const std::string& name);
std::string formatCopperAmount(uint32_t amount);

InventoryHandler::InventoryHandler(GameHandler& owner)
    : owner_(owner) {}

// ============================================================
// Opcode Registration
// ============================================================

void InventoryHandler::registerOpcodes(DispatchTable& table) {
    // ---- Item query response ----
    table[Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE] = [this](network::Packet& packet) { handleItemQueryResponse(packet); };
    table[Opcode::SMSG_ITEM_QUERY_MULTIPLE_RESPONSE] = [this](network::Packet& packet) { handleItemQueryResponse(packet); };

    // ---- Loot ----
    table[Opcode::SMSG_LOOT_RESPONSE] = [this](network::Packet& packet) { handleLootResponse(packet); };
    table[Opcode::SMSG_LOOT_RELEASE_RESPONSE] = [this](network::Packet& packet) { handleLootReleaseResponse(packet); };
    table[Opcode::SMSG_LOOT_REMOVED] = [this](network::Packet& packet) { handleLootRemoved(packet); };
    table[Opcode::SMSG_LOOT_ROLL] = [this](network::Packet& packet) { handleLootRoll(packet); };
    table[Opcode::SMSG_LOOT_ROLL_WON] = [this](network::Packet& packet) { handleLootRollWon(packet); };
    table[Opcode::SMSG_LOOT_MASTER_LIST] = [this](network::Packet& packet) {
        masterLootCandidates_.clear();
        if (packet.getSize() - packet.getReadPos() < 1) return;
        uint8_t mlCount = packet.readUInt8();
        masterLootCandidates_.reserve(mlCount);
        for (uint8_t i = 0; i < mlCount; ++i) {
            if (packet.getSize() - packet.getReadPos() < 8) break;
            masterLootCandidates_.push_back(packet.readUInt64());
        }
    };

    // ---- Loot money / misc consume ----
    table[Opcode::SMSG_LOOT_MONEY_NOTIFY] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() < 4) return;
        uint32_t amount = packet.readUInt32();
        if (packet.getSize() - packet.getReadPos() >= 1)
            /*uint8_t soleLooter =*/ packet.readUInt8();
        owner_.playerMoneyCopper_ += amount;
        owner_.pendingMoneyDelta_ = amount;
        owner_.pendingMoneyDeltaTimer_ = 2.0f;
        uint64_t notifyGuid = pendingLootMoneyGuid_ != 0 ? pendingLootMoneyGuid_ : currentLoot_.lootGuid;
        pendingLootMoneyGuid_ = 0;
        pendingLootMoneyAmount_ = 0;
        pendingLootMoneyNotifyTimer_ = 0.0f;
        bool alreadyAnnounced = false;
        auto it = localLootState_.find(notifyGuid);
        if (it != localLootState_.end()) {
            alreadyAnnounced = it->second.moneyTaken;
            it->second.moneyTaken = true;
        }
        if (!alreadyAnnounced) {
            owner_.addSystemChatMessage("Looted: " + formatCopperAmount(amount));
            auto* renderer = core::Application::getInstance().getRenderer();
            if (renderer) {
                if (auto* sfx = renderer->getUiSoundManager()) {
                    if (amount >= 10000) sfx->playLootCoinLarge();
                    else sfx->playLootCoinSmall();
                }
            }
            if (notifyGuid != 0)
                recentLootMoneyAnnounceCooldowns_[notifyGuid] = 1.5f;
        }
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYER_MONEY", {});
    };
    for (auto op : { Opcode::SMSG_LOOT_CLEAR_MONEY }) {
        table[op] = [](network::Packet& /*packet*/) {};

    // ---- Read item (books) (moved from GameHandler) ----
    table[Opcode::SMSG_READ_ITEM_OK] = [this](network::Packet& packet) {
        owner_.bookPages_.clear();  // fresh book for this item read
        packet.skipAll();
    };
    table[Opcode::SMSG_READ_ITEM_FAILED] = [this](network::Packet& packet) {
        owner_.addUIError("You cannot read this item.");
        owner_.addSystemChatMessage("You cannot read this item.");
        packet.skipAll();
    };
    }

    // ---- Loot roll start / notifications ----
    table[Opcode::SMSG_LOOT_START_ROLL] = [this](network::Packet& packet) {
        // objectGuid(8) + mapId(4) (WotLK) + lootSlot(4) + itemId(4) + randSuffix(4) +
        // randProp(4) + countdown(4) + voteMask(1)
        const bool hasMapId = isActiveExpansion("wotlk");
        const size_t minSz = hasMapId ? 33 : 29;
        if (packet.getSize() - packet.getReadPos() < minSz) return;
        uint64_t objectGuid = packet.readUInt64();
        if (hasMapId) packet.readUInt32(); // mapId
        uint32_t lootSlot = packet.readUInt32();
        uint32_t itemId   = packet.readUInt32();
        /*uint32_t randSuffix =*/ packet.readUInt32();
        /*int32_t  randProp   =*/ static_cast<int32_t>(packet.readUInt32());
        uint32_t countdown = packet.readUInt32();
        uint8_t  voteMask  = packet.readUInt8();

        // Resolve item name from cache
        owner_.ensureItemInfo(itemId);
        auto* info = owner_.getItemInfo(itemId);
        std::string itemName = (info && !info->name.empty()) ? info->name : ("Item #" + std::to_string(itemId));
        uint8_t quality = info ? static_cast<uint8_t>(info->quality) : 1;

        pendingLootRollActive_ = true;
        pendingLootRoll_ = {};
        pendingLootRoll_.objectGuid = objectGuid;
        pendingLootRoll_.slot = lootSlot;
        pendingLootRoll_.itemId = itemId;
        pendingLootRoll_.itemName = itemName;
        pendingLootRoll_.itemQuality = quality;
        pendingLootRoll_.rollCountdownMs = countdown;
        pendingLootRoll_.voteMask = voteMask;
        pendingLootRoll_.rollStartedAt = std::chrono::steady_clock::now();
        pendingLootRoll_.playerRolls.clear();
        std::string link = buildItemLink(itemId, quality, itemName);
        owner_.addSystemChatMessage("Loot roll started for " + link + ".");
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("START_LOOT_ROLL", {});
    };

    table[Opcode::SMSG_LOOT_ALL_PASSED] = [this](network::Packet& packet) {
        // objectGuid(8) + lootSlot(4) + itemId(4) + randSuffix(4) + randProp(4)
        if (packet.getSize() - packet.getReadPos() < 24) return;
        /*uint64_t objectGuid =*/ packet.readUInt64();
        /*uint32_t lootSlot   =*/ packet.readUInt32();
        uint32_t itemId     = packet.readUInt32();
        /*uint32_t randSuffix =*/ packet.readUInt32();
        /*int32_t  randProp   =*/ static_cast<int32_t>(packet.readUInt32());
        owner_.ensureItemInfo(itemId);
        auto* allPassInfo = owner_.getItemInfo(itemId);
        std::string allPassName = (allPassInfo && !allPassInfo->name.empty())
            ? allPassInfo->name : ("Item #" + std::to_string(itemId));
        uint32_t allPassQuality = allPassInfo ? allPassInfo->quality : 1u;
        owner_.addSystemChatMessage("Everyone passed on " + buildItemLink(itemId, allPassQuality, allPassName) + ".");
        pendingLootRollActive_ = false;
    };

    table[Opcode::SMSG_LOOT_ITEM_NOTIFY] = [this](network::Packet& packet) {
        // objectGuid(8) + lootSlot(1) [Classic: uint32; WotLK: uint8]
        if (packet.getSize() - packet.getReadPos() < 9) return;
        /*uint64_t objectGuid =*/ packet.readUInt64();
        uint32_t lootSlot;
        if (isActiveExpansion("wotlk")) {
            lootSlot = packet.readUInt8();
        } else {
            if (packet.getSize() - packet.getReadPos() < 4) return;
            lootSlot = packet.readUInt32();
        }
        // Try to resolve item name
        uint32_t itemId = 0;
        for (const auto& lootItem : currentLoot_.items) {
            if (lootItem.slotIndex == static_cast<uint8_t>(lootSlot)) {
                itemId = lootItem.itemId;
                break;
            }
        }
        if (itemId != 0) {
            auto* notifInfo = owner_.getItemInfo(itemId);
            std::string itemName = (notifInfo && !notifInfo->name.empty())
                ? notifInfo->name : ("Item #" + std::to_string(itemId));
            uint32_t notifyQuality = notifInfo ? notifInfo->quality : 1u;
            std::string itemLink2 = buildItemLink(itemId, notifyQuality, itemName);
            owner_.addSystemChatMessage("You receive loot: " + itemLink2 + ".");
        }
    };

    table[Opcode::SMSG_LOOT_SLOT_CHANGED] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 1) {
            uint8_t slotIdx = packet.readUInt8();
            LOG_DEBUG("SMSG_LOOT_SLOT_CHANGED: slot=", (int)slotIdx);
            // The server re-sends loot info for this slot; we can refresh from
            // the next SMSG_LOOT_RESPONSE or SMSG_LOOT_ITEM_NOTIFY.
        }
    };

    // ---- Item push result ----
    table[Opcode::SMSG_ITEM_PUSH_RESULT] = [this](network::Packet& packet) {
        // guid(8)+received(4)+created(4)+?+?+bag(1)+slot(4)+itemId(4)+...+count(4)+...
        if (packet.getSize() - packet.getReadPos() < 45) return;
        uint64_t guid = packet.readUInt64();
        if (guid != owner_.playerGuid) { packet.setReadPos(packet.getSize()); return; }
        /*uint32_t received =*/ packet.readUInt32();
        /*uint32_t created  =*/ packet.readUInt32();
        /*uint32_t unk1 =*/ packet.readUInt32();
        /*uint8_t  unk2 =*/ packet.readUInt8();
        /*uint8_t  bag  =*/ packet.readUInt8();
        /*uint32_t slot =*/ packet.readUInt32();
        uint32_t itemId = packet.readUInt32();
        /*uint32_t propSeed =*/ packet.readUInt32();
        int32_t randomProp = static_cast<int32_t>(packet.readUInt32());
        uint32_t count = packet.readUInt32();

        auto* info = owner_.getItemInfo(itemId);
        if (!info || info->name.empty()) {
            // Item info not yet cached — defer notification
            owner_.pendingItemPushNotifs_.push_back({itemId, count});
            owner_.ensureItemInfo(itemId);
            return;
        }
        std::string itemName = info->name;
        if (randomProp != 0) {
            std::string suffix = owner_.getRandomPropertyName(randomProp);
            if (!suffix.empty()) itemName += " " + suffix;
        }
        uint32_t quality = info->quality;
        std::string link = buildItemLink(itemId, quality, itemName);
        std::string msg = "Received item: " + link;
        if (count > 1) msg += " x" + std::to_string(count);
        owner_.addSystemChatMessage(msg);
        if (auto* renderer = core::Application::getInstance().getRenderer()) {
            if (auto* sfx = renderer->getUiSoundManager())
                sfx->playLootItem();
        }
        if (owner_.addonEventCallback_) {
            owner_.addonEventCallback_("BAG_UPDATE", {});
            owner_.addonEventCallback_("ITEM_PUSH", {std::to_string(itemId), std::to_string(count)});
        }
        if (owner_.itemLootCallback_)
            owner_.itemLootCallback_(itemId, count, quality, itemName);
    };

    // ---- Open container ----
    table[Opcode::SMSG_OPEN_CONTAINER] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 8) {
            uint64_t containerGuid = packet.readUInt64();
            LOG_DEBUG("SMSG_OPEN_CONTAINER: guid=0x", std::hex, containerGuid, std::dec);
        }
    };

    // ---- Sell / Buy / Inventory ----
    table[Opcode::SMSG_SELL_ITEM] = [this](network::Packet& packet) {
        if ((packet.getSize() - packet.getReadPos()) >= 17) {
            uint64_t vendorGuid = packet.readUInt64();
            uint64_t itemGuid = packet.readUInt64();
            uint8_t result = packet.readUInt8();
            LOG_INFO("SMSG_SELL_ITEM: vendorGuid=0x", std::hex, vendorGuid,
                     " itemGuid=0x", itemGuid, std::dec,
                     " result=", static_cast<int>(result));
            if (result == 0) {
                pendingSellToBuyback_.erase(itemGuid);
                if (auto* renderer = core::Application::getInstance().getRenderer()) {
                    if (auto* sfx = renderer->getUiSoundManager())
                        sfx->playDropOnGround();
                }
                if (owner_.addonEventCallback_) {
                    owner_.addonEventCallback_("BAG_UPDATE", {});
                    owner_.addonEventCallback_("PLAYER_MONEY", {});
                }
            } else {
                bool removedPending = false;
                auto it = pendingSellToBuyback_.find(itemGuid);
                if (it != pendingSellToBuyback_.end()) {
                    for (auto bit = buybackItems_.begin(); bit != buybackItems_.end(); ++bit) {
                        if (bit->itemGuid == itemGuid) {
                            buybackItems_.erase(bit);
                            return;
                        }
                    }
                    pendingSellToBuyback_.erase(it);
                    removedPending = true;
                }
                if (!removedPending) {
                    if (!buybackItems_.empty()) {
                        uint64_t frontGuid = buybackItems_.front().itemGuid;
                        if (pendingSellToBuyback_.erase(frontGuid) > 0) {
                            buybackItems_.pop_front();
                            removedPending = true;
                        }
                    }
                }
                if (!removedPending && !pendingSellToBuyback_.empty()) {
                    pendingSellToBuyback_.clear();
                    buybackItems_.clear();
                }
                static const char* sellErrors[] = {
                    "OK", "Can't find item", "Can't sell item",
                    "Can't find vendor", "You don't own that item",
                    "Unknown error", "Only empty bag"
                };
                const char* msg = (result < 7) ? sellErrors[result] : "Unknown sell error";
                owner_.addUIError(std::string("Sell failed: ") + msg);
                owner_.addSystemChatMessage(std::string("Sell failed: ") + msg);
                if (auto* renderer = core::Application::getInstance().getRenderer()) {
                    if (auto* sfx = renderer->getUiSoundManager())
                        sfx->playError();
                }
                LOG_WARNING("SMSG_SELL_ITEM error: ", (int)result, " (", msg, ")");
            }
        }
    };

    table[Opcode::SMSG_INVENTORY_CHANGE_FAILURE] = [this](network::Packet& packet) {
        if ((packet.getSize() - packet.getReadPos()) >= 1) {
            uint8_t error = packet.readUInt8();
            if (error != 0) {
                LOG_WARNING("SMSG_INVENTORY_CHANGE_FAILURE: error=", (int)error);
                uint32_t requiredLevel = 0;
                if (packet.getSize() - packet.getReadPos() >= 17) {
                    packet.readUInt64();
                    packet.readUInt64();
                    packet.readUInt8();
                    if (error == 1 && packet.getSize() - packet.getReadPos() >= 4)
                        requiredLevel = packet.readUInt32();
                }
                const char* errMsg = nullptr;
                char levelBuf[64];
                switch (error) {
                    case 1:
                        if (requiredLevel > 0) {
                            std::snprintf(levelBuf, sizeof(levelBuf),
                                          "You must reach level %u to use that item.", requiredLevel);
                            owner_.addUIError(levelBuf);
                            owner_.addSystemChatMessage(levelBuf);
                        } else {
                            owner_.addUIError("You must reach a higher level to use that item.");
                            owner_.addSystemChatMessage("You must reach a higher level to use that item.");
                        }
                        return;
                    case 2:  errMsg = "You don't have the required skill."; break;
                    case 3:  errMsg = "That item doesn't go in that slot."; break;
                    case 4:  errMsg = "That bag is full."; break;
                    case 5:  errMsg = "Can't put bags in bags."; break;
                    case 6:  errMsg = "Can't trade equipped bags."; break;
                    case 7:  errMsg = "That slot only holds ammo."; break;
                    case 8:  errMsg = "You can't use that item."; break;
                    case 9:  errMsg = "No equipment slot available."; break;
                    case 10: errMsg = "You can never use that item."; break;
                    case 11: errMsg = "You can never use that item."; break;
                    case 12: errMsg = "No equipment slot available."; break;
                    case 13: errMsg = "Can't equip with a two-handed weapon."; break;
                    case 14: errMsg = "Can't dual-wield."; break;
                    case 15: errMsg = "That item doesn't go in that bag."; break;
                    case 16: errMsg = "That item doesn't go in that bag."; break;
                    case 17: errMsg = "You can't carry any more of those."; break;
                    case 18: errMsg = "No equipment slot available."; break;
                    case 19: errMsg = "Can't stack those items."; break;
                    case 20: errMsg = "That item can't be equipped."; break;
                    case 21: errMsg = "Can't swap items."; break;
                    case 22: errMsg = "That slot is empty."; break;
                    case 23: errMsg = "Item not found."; break;
                    case 24: errMsg = "Can't drop soulbound items."; break;
                    case 25: errMsg = "Out of range."; break;
                    case 26: errMsg = "Need to split more than 1."; break;
                    case 27: errMsg = "Split failed."; break;
                    case 28: errMsg = "Not enough reagents."; break;
                    case 29: errMsg = "Not enough money."; break;
                    case 30: errMsg = "Not a bag."; break;
                    case 31: errMsg = "Can't destroy non-empty bag."; break;
                    case 32: errMsg = "You don't own that item."; break;
                    case 33: errMsg = "You can only have one quiver."; break;
                    case 34: errMsg = "No free bank slots."; break;
                    case 35: errMsg = "No bank here."; break;
                    case 36: errMsg = "Item is locked."; break;
                    case 37: errMsg = "You are stunned."; break;
                    case 38: errMsg = "You are dead."; break;
                    case 39: errMsg = "Can't do that right now."; break;
                    case 40: errMsg = "Internal bag error."; break;
                    case 49: errMsg = "Loot is gone."; break;
                    case 50: errMsg = "Inventory is full."; break;
                    case 51: errMsg = "Bank is full."; break;
                    case 52: errMsg = "That item is sold out."; break;
                    case 58: errMsg = "That object is busy."; break;
                    case 60: errMsg = "Can't do that in combat."; break;
                    case 61: errMsg = "Can't do that while disarmed."; break;
                    case 63: errMsg = "Requires a higher rank."; break;
                    case 64: errMsg = "Requires higher reputation."; break;
                    case 67: errMsg = "That item is unique-equipped."; break;
                    case 69: errMsg = "Not enough honor points."; break;
                    case 70: errMsg = "Not enough arena points."; break;
                    case 77: errMsg = "Too much gold."; break;
                    case 78: errMsg = "Can't do that during arena match."; break;
                    case 80: errMsg = "Requires a personal arena rating."; break;
                    case 87: errMsg = "Requires a higher level."; break;
                    case 88: errMsg = "Requires the right talent."; break;
                    default: break;
                }
                std::string msg = errMsg ? errMsg : "Inventory error (" + std::to_string(error) + ").";
                owner_.addUIError(msg);
                owner_.addSystemChatMessage(msg);
                if (auto* renderer = core::Application::getInstance().getRenderer()) {
                    if (auto* sfx = renderer->getUiSoundManager())
                        sfx->playError();
                }
            }
        }
    };

    table[Opcode::SMSG_BUY_FAILED] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 13) {
            uint64_t vendorGuid = packet.readUInt64();
            uint32_t itemIdOrSlot = packet.readUInt32();
            uint8_t errCode = packet.readUInt8();
            LOG_INFO("SMSG_BUY_FAILED: vendorGuid=0x", std::hex, vendorGuid, std::dec,
                     " item/slot=", itemIdOrSlot,
                     " err=", static_cast<int>(errCode),
                     " pendingBuybackSlot=", pendingBuybackSlot_,
                     " pendingBuybackWireSlot=", pendingBuybackWireSlot_,
                     " pendingBuyItemId=", pendingBuyItemId_,
                     " pendingBuyItemSlot=", pendingBuyItemSlot_);
            if (pendingBuybackSlot_ >= 0) {
                if (errCode == 0) {
                    constexpr uint16_t kWotlkCmsgBuybackItemOpcode = 0x290;
                    constexpr uint32_t kBuybackSlotEnd = 85;
                    if (pendingBuybackWireSlot_ >= 74 && pendingBuybackWireSlot_ < kBuybackSlotEnd &&
                        owner_.socket && owner_.state == WorldState::IN_WORLD && currentVendorItems_.vendorGuid != 0) {
                        ++pendingBuybackWireSlot_;
                        LOG_INFO("Buyback retry: vendorGuid=0x", std::hex, currentVendorItems_.vendorGuid,
                                 std::dec, " uiSlot=", pendingBuybackSlot_,
                                 " wireSlot=", pendingBuybackWireSlot_);
                        network::Packet retry(kWotlkCmsgBuybackItemOpcode);
                        retry.writeUInt64(currentVendorItems_.vendorGuid);
                        retry.writeUInt32(pendingBuybackWireSlot_);
                        owner_.socket->send(retry);
                        return;
                    }
                    if (pendingBuybackSlot_ < static_cast<int>(buybackItems_.size())) {
                        buybackItems_.erase(buybackItems_.begin() + pendingBuybackSlot_);
                    }
                    pendingBuybackSlot_ = -1;
                    pendingBuybackWireSlot_ = 0;
                    if (currentVendorItems_.vendorGuid != 0 && owner_.socket && owner_.state == WorldState::IN_WORLD) {
                        auto pkt = ListInventoryPacket::build(currentVendorItems_.vendorGuid);
                        owner_.socket->send(pkt);
                    }
                    return;
                }
                pendingBuybackSlot_ = -1;
                pendingBuybackWireSlot_ = 0;
            }
            const char* msg = "Purchase failed.";
            switch (errCode) {
                case 0: msg = "Purchase failed: item not found."; break;
                case 2: msg = "You don't have enough money."; break;
                case 4: msg = "Seller is too far away."; break;
                case 5: msg = "That item is sold out."; break;
                case 6: msg = "You can't carry any more items."; break;
                default: break;
            }
            owner_.addUIError(msg);
            owner_.addSystemChatMessage(msg);
            if (auto* renderer = core::Application::getInstance().getRenderer()) {
                if (auto* sfx = renderer->getUiSoundManager())
                    sfx->playError();
            }
        }
    };

    table[Opcode::SMSG_BUY_ITEM] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 20) {
            /*uint64_t vendorGuid =*/ packet.readUInt64();
            /*uint32_t vendorSlot =*/ packet.readUInt32();
            /*int32_t  newCount   =*/ static_cast<int32_t>(packet.readUInt32());
            uint32_t itemCount  = packet.readUInt32();
            if (pendingBuyItemId_ != 0) {
                std::string itemLabel;
                uint32_t buyQuality = 1;
                if (const ItemQueryResponseData* info = owner_.getItemInfo(pendingBuyItemId_)) {
                    if (!info->name.empty()) itemLabel = info->name;
                    buyQuality = info->quality;
                }
                if (itemLabel.empty()) itemLabel = "item #" + std::to_string(pendingBuyItemId_);
                std::string msg = "Purchased: " + buildItemLink(pendingBuyItemId_, buyQuality, itemLabel);
                if (itemCount > 1) msg += " x" + std::to_string(itemCount);
                owner_.addSystemChatMessage(msg);
                if (auto* renderer = core::Application::getInstance().getRenderer()) {
                    if (auto* sfx = renderer->getUiSoundManager())
                        sfx->playPickupBag();
                }
            }
            pendingBuyItemId_   = 0;
            pendingBuyItemSlot_ = 0;
            if (owner_.addonEventCallback_) {
                owner_.addonEventCallback_("MERCHANT_UPDATE", {});
                owner_.addonEventCallback_("BAG_UPDATE", {});
            }
        }
    };

    // ---- Vendor / Trainer ----
    table[Opcode::SMSG_LIST_INVENTORY] = [this](network::Packet& packet) { handleListInventory(packet); };
    table[Opcode::SMSG_TRAINER_LIST] = [this](network::Packet& packet) { handleTrainerList(packet); };

    // ---- Mail ----
    table[Opcode::SMSG_SHOW_MAILBOX] = [this](network::Packet& packet) { handleShowMailbox(packet); };
    table[Opcode::SMSG_MAIL_LIST_RESULT] = [this](network::Packet& packet) { handleMailListResult(packet); };
    table[Opcode::SMSG_SEND_MAIL_RESULT] = [this](network::Packet& packet) { handleSendMailResult(packet); };
    table[Opcode::SMSG_RECEIVED_MAIL] = [this](network::Packet& packet) { handleReceivedMail(packet); };
    table[Opcode::MSG_QUERY_NEXT_MAIL_TIME] = [this](network::Packet& packet) { handleQueryNextMailTime(packet); };

    // ---- Bank ----
    table[Opcode::SMSG_SHOW_BANK] = [this](network::Packet& packet) { handleShowBank(packet); };
    table[Opcode::SMSG_BUY_BANK_SLOT_RESULT] = [this](network::Packet& packet) { handleBuyBankSlotResult(packet); };

    // ---- Guild Bank ----
    table[Opcode::SMSG_GUILD_BANK_LIST] = [this](network::Packet& packet) { handleGuildBankList(packet); };

    // ---- Auction House ----
    table[Opcode::MSG_AUCTION_HELLO] = [this](network::Packet& packet) { handleAuctionHello(packet); };
    table[Opcode::SMSG_AUCTION_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionListResult(packet); };
    table[Opcode::SMSG_AUCTION_OWNER_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionOwnerListResult(packet); };
    table[Opcode::SMSG_AUCTION_BIDDER_LIST_RESULT] = [this](network::Packet& packet) { handleAuctionBidderListResult(packet); };
    table[Opcode::SMSG_AUCTION_COMMAND_RESULT] = [this](network::Packet& packet) { handleAuctionCommandResult(packet); };

    table[Opcode::SMSG_AUCTION_OWNER_NOTIFICATION] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 16) {
            /*uint32_t auctionId =*/ packet.readUInt32();
            uint32_t action    = packet.readUInt32();
            /*uint32_t error   =*/ packet.readUInt32();
            uint32_t itemEntry = packet.readUInt32();
            int32_t ownerRandProp = 0;
            if (packet.getSize() - packet.getReadPos() >= 4)
                ownerRandProp = static_cast<int32_t>(packet.readUInt32());
            owner_.ensureItemInfo(itemEntry);
            auto* info = owner_.getItemInfo(itemEntry);
            std::string rawName = info && !info->name.empty() ? info->name : ("Item #" + std::to_string(itemEntry));
            if (ownerRandProp != 0) {
                std::string suffix = owner_.getRandomPropertyName(ownerRandProp);
                if (!suffix.empty()) rawName += " " + suffix;
            }
            uint32_t aucQuality = info ? info->quality : 1u;
            std::string itemLink = buildItemLink(itemEntry, aucQuality, rawName);
            if (action == 1)
                owner_.addSystemChatMessage("Your auction of " + itemLink + " has expired.");
            else if (action == 2)
                owner_.addSystemChatMessage("A bid has been placed on your auction of " + itemLink + ".");
            else
                owner_.addSystemChatMessage("Your auction of " + itemLink + " has sold!");
        }
        packet.setReadPos(packet.getSize());
    };

    table[Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 8) {
            /*uint32_t auctionId =*/ packet.readUInt32();
            uint32_t itemEntry = packet.readUInt32();
            int32_t bidRandProp = 0;
            if (packet.getSize() - packet.getReadPos() >= 4)
                bidRandProp = static_cast<int32_t>(packet.readUInt32());
            owner_.ensureItemInfo(itemEntry);
            auto* info = owner_.getItemInfo(itemEntry);
            std::string rawName2 = info && !info->name.empty() ? info->name : ("Item #" + std::to_string(itemEntry));
            if (bidRandProp != 0) {
                std::string suffix = owner_.getRandomPropertyName(bidRandProp);
                if (!suffix.empty()) rawName2 += " " + suffix;
            }
            uint32_t bidQuality = info ? info->quality : 1u;
            std::string bidLink = buildItemLink(itemEntry, bidQuality, rawName2);
            owner_.addSystemChatMessage("You have been outbid on " + bidLink + ".");
        }
        packet.setReadPos(packet.getSize());
    };

    table[Opcode::SMSG_AUCTION_REMOVED_NOTIFICATION] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 12) {
            /*uint32_t auctionId =*/ packet.readUInt32();
            uint32_t itemEntry = packet.readUInt32();
            int32_t itemRandom = static_cast<int32_t>(packet.readUInt32());
            owner_.ensureItemInfo(itemEntry);
            auto* info = owner_.getItemInfo(itemEntry);
            std::string rawName3 = info && !info->name.empty() ? info->name : ("Item #" + std::to_string(itemEntry));
            if (itemRandom != 0) {
                std::string suffix = owner_.getRandomPropertyName(itemRandom);
                if (!suffix.empty()) rawName3 += " " + suffix;
            }
            uint32_t remQuality = info ? info->quality : 1u;
            std::string remLink = buildItemLink(itemEntry, remQuality, rawName3);
            owner_.addSystemChatMessage("Your auction of " + remLink + " has expired.");
        }
        packet.setReadPos(packet.getSize());
    };

    // ---- Equipment Sets ----
    table[Opcode::SMSG_EQUIPMENT_SET_LIST] = [this](network::Packet& packet) { handleEquipmentSetList(packet); };
    table[Opcode::SMSG_EQUIPMENT_SET_SAVED] = [this](network::Packet& packet) {
        std::string setName;
        if (packet.getSize() - packet.getReadPos() >= 12) {
            uint32_t setIndex = packet.readUInt32();
            uint64_t setGuid  = packet.readUInt64();
            bool found = false;
            for (auto& es : equipmentSets_) {
                if (es.setGuid == setGuid || es.setId == setIndex) {
                    es.setGuid = setGuid;
                    setName = es.name;
                    found = true;
                    break;
                }
            }
            for (auto& info : equipmentSetInfo_) {
                if (info.setGuid == setGuid || info.setId == setIndex) {
                    info.setGuid = setGuid;
                    break;
                }
            }
            if (!found && setGuid != 0) {
                EquipmentSet newEs;
                newEs.setGuid = setGuid;
                newEs.setId   = setIndex;
                newEs.name    = pendingSaveSetName_;
                newEs.iconName = pendingSaveSetIcon_;
                for (int s = 0; s < 19; ++s)
                    newEs.itemGuids[s] = owner_.getEquipSlotGuid(s);
                equipmentSets_.push_back(std::move(newEs));
                EquipmentSetInfo newInfo;
                newInfo.setGuid = setGuid;
                newInfo.setId   = setIndex;
                newInfo.name    = pendingSaveSetName_;
                newInfo.iconName = pendingSaveSetIcon_;
                equipmentSetInfo_.push_back(std::move(newInfo));
                setName = pendingSaveSetName_;
            }
            pendingSaveSetName_.clear();
            pendingSaveSetIcon_.clear();
            LOG_INFO("SMSG_EQUIPMENT_SET_SAVED: index=", setIndex,
                     " guid=", setGuid, " name=", setName);
        }
        owner_.addSystemChatMessage(setName.empty()
            ? std::string("Equipment set saved.")
            : "Equipment set \"" + setName + "\" saved.");
    };

    table[Opcode::SMSG_EQUIPMENT_SET_USE_RESULT] = [this](network::Packet& packet) {
        if (packet.getSize() - packet.getReadPos() >= 1) {
            uint8_t result = packet.readUInt8();
            if (result != 0) { owner_.addUIError("Failed to equip item set."); owner_.addSystemChatMessage("Failed to equip item set."); }
        }
    };

    // ---- Item text ----
    table[Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE] = [this](network::Packet& packet) { handleItemTextQueryResponse(packet); };

    // ---- Trade ----
    table[Opcode::SMSG_TRADE_STATUS] = [this](network::Packet& packet) { handleTradeStatus(packet); };
    table[Opcode::SMSG_TRADE_STATUS_EXTENDED] = [this](network::Packet& packet) { handleTradeStatusExtended(packet); };

    // ---- Trainer buy ----
    table[Opcode::SMSG_TRAINER_BUY_SUCCEEDED] = [this](network::Packet& p) { handleTrainerBuySucceeded(p); };
    table[Opcode::SMSG_TRAINER_BUY_FAILED] = [this](network::Packet& p) { handleTrainerBuyFailed(p); };
}

// ============================================================
// Loot
// ============================================================

void InventoryHandler::lootTarget(uint64_t targetGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    currentLoot_.items.clear();
    LOG_INFO("Looting target 0x", std::hex, targetGuid, std::dec);
    auto packet = LootPacket::build(targetGuid);
    owner_.socket->send(packet);
}

void InventoryHandler::lootItem(uint8_t slotIndex) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = AutostoreLootItemPacket::build(slotIndex);
    owner_.socket->send(packet);
}

void InventoryHandler::closeLoot() {
    if (!lootWindowOpen_) return;
    if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
        auto packet = LootReleasePacket::build(currentLoot_.lootGuid);
        owner_.socket->send(packet);
    }
    lootWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("LOOT_CLOSED", {});
    currentLoot_ = LootResponseData{};
}

void InventoryHandler::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_MASTER_GIVE));
    pkt.writeUInt64(currentLoot_.lootGuid);
    pkt.writeUInt8(lootSlot);
    pkt.writeUInt64(targetGuid);
    owner_.socket->send(pkt);
}

void InventoryHandler::handleLootResponse(network::Packet& packet) {
    const bool wotlkLoot = isActiveExpansion("wotlk");
    if (!LootResponseParser::parse(packet, currentLoot_, wotlkLoot)) return;
    const bool hasLoot = !currentLoot_.items.empty() || currentLoot_.gold > 0;
    if (!hasLoot && owner_.isCasting() && owner_.getCurrentCastSpellId() != 0 && lastInteractedGoGuid_ != 0) {
        LOG_DEBUG("Ignoring empty SMSG_LOOT_RESPONSE during gather cast");
        return;
    }
    lootWindowOpen_ = true;
    if (owner_.addonEventCallback_) {
        owner_.addonEventCallback_("LOOT_OPENED", {});
        owner_.addonEventCallback_("LOOT_READY", {});
    }
    lastInteractedGoGuid_ = 0;
    pendingGameObjectLootOpens_.erase(
        std::remove_if(pendingGameObjectLootOpens_.begin(), pendingGameObjectLootOpens_.end(),
                       [&](const PendingLootOpen& p) { return p.guid == currentLoot_.lootGuid; }),
        pendingGameObjectLootOpens_.end());
    auto& localLoot = localLootState_[currentLoot_.lootGuid];
    localLoot.data = currentLoot_;

    for (const auto& item : currentLoot_.items) {
        owner_.queryItemInfo(item.itemId, 0);
    }

    if (currentLoot_.gold > 0) {
        if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
            bool suppressFallback = false;
            auto cooldownIt = recentLootMoneyAnnounceCooldowns_.find(currentLoot_.lootGuid);
            if (cooldownIt != recentLootMoneyAnnounceCooldowns_.end() && cooldownIt->second > 0.0f) {
                suppressFallback = true;
            }
            pendingLootMoneyGuid_ = suppressFallback ? 0 : currentLoot_.lootGuid;
            pendingLootMoneyAmount_ = suppressFallback ? 0 : currentLoot_.gold;
            pendingLootMoneyNotifyTimer_ = suppressFallback ? 0.0f : 0.4f;
            auto pkt = LootMoneyPacket::build();
            owner_.socket->send(pkt);
            currentLoot_.gold = 0;
        }
    }

    if (autoLoot_ && owner_.state == WorldState::IN_WORLD && owner_.socket && !localLoot.itemAutoLootSent) {
        for (const auto& item : currentLoot_.items) {
            auto pkt = AutostoreLootItemPacket::build(item.slotIndex);
            owner_.socket->send(pkt);
        }
        localLoot.itemAutoLootSent = true;
    }
}

void InventoryHandler::handleLootReleaseResponse(network::Packet& packet) {
    (void)packet;
    localLootState_.erase(currentLoot_.lootGuid);
    lootWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("LOOT_CLOSED", {});
    currentLoot_ = LootResponseData{};
}

void InventoryHandler::handleLootRemoved(network::Packet& packet) {
    uint8_t slotIndex = packet.readUInt8();
    for (auto it = currentLoot_.items.begin(); it != currentLoot_.items.end(); ++it) {
        if (it->slotIndex == slotIndex) {
            std::string itemName = "item #" + std::to_string(it->itemId);
            uint32_t quality = 1;
            if (const ItemQueryResponseData* info = owner_.getItemInfo(it->itemId)) {
                if (!info->name.empty()) itemName = info->name;
                quality = info->quality;
            }
            std::string link = buildItemLink(it->itemId, quality, itemName);
            std::string msgStr = "Looted: " + link;
            if (it->count > 1) msgStr += " x" + std::to_string(it->count);
            owner_.addSystemChatMessage(msgStr);
            if (auto* renderer = core::Application::getInstance().getRenderer()) {
                if (auto* sfx = renderer->getUiSoundManager())
                    sfx->playLootItem();
            }
            currentLoot_.items.erase(it);
            if (owner_.addonEventCallback_)
                owner_.addonEventCallback_("LOOT_SLOT_CLEARED", {std::to_string(slotIndex + 1)});
            break;
        }
    }
}

// ============================================================
// Loot Roll
// ============================================================

void InventoryHandler::sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_ROLL));
    pkt.writeUInt64(objectGuid);
    pkt.writeUInt32(slot);
    pkt.writeUInt8(rollType);
    owner_.socket->send(pkt);
    if (rollType == 128) { // pass
        pendingLootRollActive_ = false;
    }
}

void InventoryHandler::handleLootRoll(network::Packet& packet) {
    // objectGuid(8) + lootSlot(4) + playerGuid(8) + itemId(4) + itemRandSuffix(4) +
    // itemRandProp(4) + rollNumber(1) + rollType(1) + autoPass(1)
    if (packet.getSize() - packet.getReadPos() < 35) return;
    uint64_t objectGuid = packet.readUInt64();
    uint32_t lootSlot   = packet.readUInt32();
    uint64_t playerGuid = packet.readUInt64();
    /*uint32_t itemId     =*/ packet.readUInt32();
    /*uint32_t randSuffix =*/ packet.readUInt32();
    /*int32_t  randProp   =*/ static_cast<int32_t>(packet.readUInt32());
    uint8_t rollNumber  = packet.readUInt8();
    uint8_t rollType    = packet.readUInt8();
    /*uint8_t autoPass   =*/ packet.readUInt8();

    // Resolve player name
    std::string playerName;
    auto nit = owner_.playerNameCache.find(playerGuid);
    if (nit != owner_.playerNameCache.end()) playerName = nit->second;
    if (playerName.empty()) playerName = "Player";

    if (pendingLootRollActive_ &&
        pendingLootRoll_.objectGuid == objectGuid &&
        pendingLootRoll_.slot == lootSlot) {
        LootRollEntry::PlayerRollResult result;
        result.playerName = playerName;
        result.rollNum = rollNumber;
        result.rollType = rollType;
        pendingLootRoll_.playerRolls.push_back(result);
    }

    const char* typeStr = "passed on";
    if (rollType == 0) typeStr = "rolled Need";
    else if (rollType == 1) typeStr = "rolled Greed";
    else if (rollType == 2) typeStr = "rolled Disenchant";
    if (rollType <= 2) {
        owner_.addSystemChatMessage(playerName + " " + typeStr + " - " + std::to_string(rollNumber));
    } else {
        owner_.addSystemChatMessage(playerName + " passed.");
    }
}

void InventoryHandler::handleLootRollWon(network::Packet& packet) {
    // objectGuid(8) + lootSlot(4) + itemId(4) + itemSuffix(4) + itemProp(4) + playerGuid(8) + rollNumber(1) + rollType(1)
    if (packet.getSize() - packet.getReadPos() < 34) return;
    /*uint64_t objectGuid =*/ packet.readUInt64();
    /*uint32_t lootSlot   =*/ packet.readUInt32();
    uint32_t itemId     = packet.readUInt32();
    /*uint32_t randSuffix =*/ packet.readUInt32();
    int32_t wonRandProp = static_cast<int32_t>(packet.readUInt32());
    uint64_t winnerGuid = packet.readUInt64();
    uint8_t rollNumber  = packet.readUInt8();
    uint8_t rollType    = packet.readUInt8();

    std::string winnerName;
    auto nit = owner_.playerNameCache.find(winnerGuid);
    if (nit != owner_.playerNameCache.end()) winnerName = nit->second;
    if (winnerName.empty()) winnerName = "Player";

    owner_.ensureItemInfo(itemId);
    auto* info = owner_.getItemInfo(itemId);
    std::string itemName = (info && !info->name.empty()) ? info->name : ("Item #" + std::to_string(itemId));
    if (wonRandProp != 0) {
        std::string suffix = owner_.getRandomPropertyName(wonRandProp);
        if (!suffix.empty()) itemName += " " + suffix;
    }
    uint32_t wonQuality = info ? info->quality : 1u;
    std::string link = buildItemLink(itemId, wonQuality, itemName);

    const char* typeStr = "Need";
    if (rollType == 1) typeStr = "Greed";
    else if (rollType == 2) typeStr = "Disenchant";

    owner_.addSystemChatMessage(winnerName + " won " + link + " (" + typeStr + " - " + std::to_string(rollNumber) + ")");
    pendingLootRollActive_ = false;
}

// ============================================================
// Vendor
// ============================================================

void InventoryHandler::openVendor(uint64_t npcGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    buybackItems_.clear();
    auto packet = ListInventoryPacket::build(npcGuid);
    owner_.socket->send(packet);
}

void InventoryHandler::closeVendor() {
    bool wasOpen = vendorWindowOpen_;
    vendorWindowOpen_ = false;
    currentVendorItems_ = ListInventoryData{};
    buybackItems_.clear();
    pendingSellToBuyback_.clear();
    pendingBuybackSlot_ = -1;
    pendingBuybackWireSlot_ = 0;
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
    if (wasOpen && owner_.addonEventCallback_) owner_.addonEventCallback_("MERCHANT_CLOSED", {});
}

void InventoryHandler::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    LOG_INFO("Buy request: vendorGuid=0x", std::hex, vendorGuid, std::dec,
             " itemId=", itemId, " slot=", slot, " count=", count,
             " wire=0x", std::hex, wireOpcode(Opcode::CMSG_BUY_ITEM), std::dec);
    pendingBuyItemId_ = itemId;
    pendingBuyItemSlot_ = slot;
    network::Packet packet(wireOpcode(Opcode::CMSG_BUY_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt32(itemId);
    packet.writeUInt32(slot);
    packet.writeUInt32(count);
    const bool isWotLk = isActiveExpansion("wotlk");
    if (isWotLk) {
        packet.writeUInt8(0);
    }
    owner_.socket->send(packet);
}

void InventoryHandler::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    LOG_INFO("Sell request: vendorGuid=0x", std::hex, vendorGuid,
             " itemGuid=0x", itemGuid, std::dec,
             " count=", count, " wire=0x", std::hex, wireOpcode(Opcode::CMSG_SELL_ITEM), std::dec);
    auto packet = SellItemPacket::build(vendorGuid, itemGuid, count);
    owner_.socket->send(packet);
}

void InventoryHandler::sellItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventory.getBackpackSize()) return;
    const auto& slot = owner_.inventory.getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    uint32_t sellPrice = slot.item.sellPrice;
    if (sellPrice == 0) {
        if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
            sellPrice = info->sellPrice;
        }
    }
    if (sellPrice == 0) {
        owner_.addSystemChatMessage("Cannot sell: this item has no vendor value.");
        return;
    }

    uint64_t itemGuid = owner_.backpackSlotGuids_[backpackIndex];
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }
    LOG_DEBUG("sellItemBySlot: slot=", backpackIndex,
              " item=", slot.item.name,
              " itemGuid=0x", std::hex, itemGuid, std::dec,
              " vendorGuid=0x", std::hex, currentVendorItems_.vendorGuid, std::dec);
    if (itemGuid != 0 && currentVendorItems_.vendorGuid != 0) {
        BuybackItem sold;
        sold.itemGuid = itemGuid;
        sold.item = slot.item;
        sold.count = 1;
        buybackItems_.push_front(sold);
        if (buybackItems_.size() > 12) buybackItems_.pop_back();
        pendingSellToBuyback_[itemGuid] = sold;
        sellItem(currentVendorItems_.vendorGuid, itemGuid, 1);
    } else if (itemGuid == 0) {
        owner_.addSystemChatMessage("Cannot sell: item not found in inventory.");
        LOG_WARNING("Sell failed: missing item GUID for slot ", backpackIndex);
    } else {
        owner_.addSystemChatMessage("Cannot sell: no vendor.");
    }
}

void InventoryHandler::sellItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventory.NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventory.getBagSize(bagIndex)) return;
    const auto& slot = owner_.inventory.getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return;

    uint32_t sellPrice = slot.item.sellPrice;
    if (sellPrice == 0) {
        if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
            sellPrice = info->sellPrice;
        }
    }
    if (sellPrice == 0) {
        owner_.addSystemChatMessage("Cannot sell: this item has no vendor value.");
        return;
    }

    uint64_t itemGuid = 0;
    uint64_t bagGuid = owner_.equipSlotGuids_[19 + bagIndex];
    if (bagGuid != 0) {
        auto it = owner_.containerContents_.find(bagGuid);
        if (it != owner_.containerContents_.end() && slotIndex < static_cast<int>(it->second.numSlots)) {
            itemGuid = it->second.slotGuids[slotIndex];
        }
    }
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    if (itemGuid != 0 && currentVendorItems_.vendorGuid != 0) {
        BuybackItem sold;
        sold.itemGuid = itemGuid;
        sold.item = slot.item;
        sold.count = 1;
        buybackItems_.push_front(sold);
        if (buybackItems_.size() > 12) buybackItems_.pop_back();
        pendingSellToBuyback_[itemGuid] = sold;
        sellItem(currentVendorItems_.vendorGuid, itemGuid, 1);
    } else if (itemGuid == 0) {
        owner_.addSystemChatMessage("Cannot sell: item not found.");
    } else {
        owner_.addSystemChatMessage("Cannot sell: no vendor.");
    }
}

void InventoryHandler::buyBackItem(uint32_t buybackSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || currentVendorItems_.vendorGuid == 0) return;
    constexpr uint32_t kBuybackSlotStart = 74;
    uint32_t wireSlot = kBuybackSlotStart + buybackSlot;
    pendingBuyItemId_ = 0;
    pendingBuyItemSlot_ = 0;
    constexpr uint16_t kWotlkCmsgBuybackItemOpcode = 0x290;
    LOG_INFO("Buyback request: vendorGuid=0x", std::hex, currentVendorItems_.vendorGuid,
             std::dec, " uiSlot=", buybackSlot, " wireSlot=", wireSlot,
             " source=absolute-buyback-slot",
             " wire=0x", std::hex, kWotlkCmsgBuybackItemOpcode, std::dec);
    pendingBuybackSlot_ = static_cast<int>(buybackSlot);
    pendingBuybackWireSlot_ = wireSlot;
    network::Packet packet(kWotlkCmsgBuybackItemOpcode);
    packet.writeUInt64(currentVendorItems_.vendorGuid);
    packet.writeUInt32(wireSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::repairItem(uint64_t vendorGuid, uint64_t itemGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_REPAIR_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(itemGuid);
    packet.writeUInt8(0);
    owner_.socket->send(packet);
}

void InventoryHandler::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_REPAIR_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(0);
    packet.writeUInt8(useGuildBank ? 1 : 0);
    owner_.socket->send(packet);
}

void InventoryHandler::autoEquipItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventory.getBackpackSize()) return;
    const auto& slot = owner_.inventory.getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
        auto packet = AutoEquipItemPacket::build(0xFF, static_cast<uint8_t>(23 + backpackIndex));
        owner_.socket->send(packet);
    }
}

void InventoryHandler::autoEquipItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventory.NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventory.getBagSize(bagIndex)) return;

    if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
        auto packet = AutoEquipItemPacket::build(
            static_cast<uint8_t>(19 + bagIndex), static_cast<uint8_t>(slotIndex));
        owner_.socket->send(packet);
    }
}

void InventoryHandler::useItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventory.getBackpackSize()) return;
    const auto& slot = owner_.inventory.getBackpackSlot(backpackIndex);
    if (slot.empty()) return;

    uint64_t itemGuid = owner_.backpackSlotGuids_[backpackIndex];
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    if (itemGuid != 0 && owner_.state == WorldState::IN_WORLD && owner_.socket) {
        uint32_t useSpellId = 0;
        if (auto* info = owner_.getItemInfo(slot.item.itemId)) {
            for (const auto& sp : info->spells) {
                if (sp.spellId != 0 && (sp.spellTrigger == 0 || sp.spellTrigger == 5)) {
                    useSpellId = sp.spellId;
                    break;
                }
            }
        }
        auto packet = owner_.packetParsers_
            ? owner_.packetParsers_->buildUseItem(0xFF, static_cast<uint8_t>(23 + backpackIndex), itemGuid, useSpellId)
            : UseItemPacket::build(0xFF, static_cast<uint8_t>(23 + backpackIndex), itemGuid, useSpellId);
        owner_.socket->send(packet);
    } else if (itemGuid == 0) {
        owner_.addSystemChatMessage("Cannot use that item right now.");
    }
}

void InventoryHandler::useItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventory.NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventory.getBagSize(bagIndex)) return;
    const auto& slot = owner_.inventory.getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return;

    uint64_t itemGuid = 0;
    uint64_t bagGuid = owner_.equipSlotGuids_[19 + bagIndex];
    if (bagGuid != 0) {
        auto it = owner_.containerContents_.find(bagGuid);
        if (it != owner_.containerContents_.end() && slotIndex < static_cast<int>(it->second.numSlots)) {
            itemGuid = it->second.slotGuids[slotIndex];
        }
    }
    if (itemGuid == 0) {
        itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
    }

    LOG_INFO("useItemInBag: bag=", bagIndex, " slot=", slotIndex, " itemId=", slot.item.itemId,
             " itemGuid=0x", std::hex, itemGuid, std::dec);

    if (itemGuid != 0 && owner_.state == WorldState::IN_WORLD && owner_.socket) {
        uint32_t useSpellId = 0;
        if (auto* info = owner_.getItemInfo(slot.item.itemId)) {
            for (const auto& sp : info->spells) {
                if (sp.spellId != 0 && (sp.spellTrigger == 0 || sp.spellTrigger == 5)) {
                    useSpellId = sp.spellId;
                    break;
                }
            }
        }
        uint8_t wowBag = static_cast<uint8_t>(19 + bagIndex);
        auto packet = owner_.packetParsers_
            ? owner_.packetParsers_->buildUseItem(wowBag, static_cast<uint8_t>(slotIndex), itemGuid, useSpellId)
            : UseItemPacket::build(wowBag, static_cast<uint8_t>(slotIndex), itemGuid, useSpellId);
        LOG_INFO("useItemInBag: sending CMSG_USE_ITEM, bag=", (int)wowBag, " slot=", slotIndex,
                 " packetSize=", packet.getSize());
        owner_.socket->send(packet);
    } else if (itemGuid == 0) {
        LOG_WARNING("Use item in bag failed: missing item GUID for bag ", bagIndex, " slot ", slotIndex);
        owner_.addSystemChatMessage("Cannot use that item right now.");
    }
}

void InventoryHandler::openItemBySlot(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventory.getBackpackSize()) return;
    if (owner_.inventory.getBackpackSlot(backpackIndex).empty()) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = OpenItemPacket::build(0xFF, static_cast<uint8_t>(23 + backpackIndex));
    LOG_INFO("openItemBySlot: CMSG_OPEN_ITEM bag=0xFF slot=", (23 + backpackIndex));
    owner_.socket->send(packet);
}

void InventoryHandler::openItemInBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventory.NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= owner_.inventory.getBagSize(bagIndex)) return;
    if (owner_.inventory.getBagSlot(bagIndex, slotIndex).empty()) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    uint8_t wowBag = static_cast<uint8_t>(19 + bagIndex);
    auto packet = OpenItemPacket::build(wowBag, static_cast<uint8_t>(slotIndex));
    LOG_INFO("openItemInBag: CMSG_OPEN_ITEM bag=", (int)wowBag, " slot=", slotIndex);
    owner_.socket->send(packet);
}

void InventoryHandler::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    if (count == 0) count = 1;
    constexpr uint16_t kCmsgDestroyItem = 0x111;
    network::Packet packet(kCmsgDestroyItem);
    packet.writeUInt8(bag);
    packet.writeUInt8(slot);
    packet.writeUInt32(static_cast<uint32_t>(count));
    LOG_DEBUG("Destroy item request: bag=", (int)bag, " slot=", (int)slot,
              " count=", (int)count, " wire=0x", std::hex, kCmsgDestroyItem, std::dec);
    owner_.socket->send(packet);
}

void InventoryHandler::splitItem(uint8_t srcBag, uint8_t srcSlot, uint8_t count) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    if (count == 0) return;

    int freeBp = owner_.inventory.findFreeBackpackSlot();
    if (freeBp >= 0) {
        uint8_t dstBag = 0xFF;
        uint8_t dstSlot = static_cast<uint8_t>(23 + freeBp);
        LOG_INFO("splitItem: src(bag=", (int)srcBag, " slot=", (int)srcSlot,
                 ") count=", (int)count, " -> dst(bag=0xFF slot=", (int)dstSlot, ")");
        auto packet = SplitItemPacket::build(srcBag, srcSlot, dstBag, dstSlot, count);
        owner_.socket->send(packet);
        return;
    }
    for (int b = 0; b < owner_.inventory.NUM_BAG_SLOTS; b++) {
        int bagSize = owner_.inventory.getBagSize(b);
        for (int s = 0; s < bagSize; s++) {
            if (owner_.inventory.getBagSlot(b, s).empty()) {
                uint8_t dstBag = static_cast<uint8_t>(19 + b);
                uint8_t dstSlot = static_cast<uint8_t>(s);
                LOG_INFO("splitItem: src(bag=", (int)srcBag, " slot=", (int)srcSlot,
                         ") count=", (int)count, " -> dst(bag=", (int)dstBag,
                         " slot=", (int)dstSlot, ")");
                auto packet = SplitItemPacket::build(srcBag, srcSlot, dstBag, dstSlot, count);
                owner_.socket->send(packet);
                return;
            }
        }
    }
    owner_.addSystemChatMessage("Cannot split: no free inventory slots.");
}

void InventoryHandler::swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot) {
    if (!owner_.socket || !owner_.socket->isConnected()) return;
    LOG_INFO("swapContainerItems: src(bag=", (int)srcBag, " slot=", (int)srcSlot,
             ") -> dst(bag=", (int)dstBag, " slot=", (int)dstSlot, ")");
    auto packet = SwapItemPacket::build(dstBag, dstSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (srcBagIndex < 0 || srcBagIndex > 3 || dstBagIndex < 0 || dstBagIndex > 3) return;
    if (srcBagIndex == dstBagIndex) return;

    auto srcEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + srcBagIndex);
    auto dstEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + dstBagIndex);
    auto srcItem = owner_.inventory.getEquipSlot(srcEquip).item;
    auto dstItem = owner_.inventory.getEquipSlot(dstEquip).item;
    owner_.inventory.setEquipSlot(srcEquip, dstItem);
    owner_.inventory.setEquipSlot(dstEquip, srcItem);
    owner_.inventory.swapBagContents(srcBagIndex, dstBagIndex);

    if (owner_.socket && owner_.socket->isConnected()) {
        uint8_t srcSlot = static_cast<uint8_t>(19 + srcBagIndex);
        uint8_t dstSlot = static_cast<uint8_t>(19 + dstBagIndex);
        LOG_INFO("swapBagSlots: bag ", srcBagIndex, " (slot ", (int)srcSlot,
                 ") <-> bag ", dstBagIndex, " (slot ", (int)dstSlot, ")");
        auto packet = SwapItemPacket::build(255, dstSlot, 255, srcSlot);
        owner_.socket->send(packet);
    }
}

void InventoryHandler::unequipToBackpack(EquipSlot equipSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;

    int freeSlot = owner_.inventory.findFreeBackpackSlot();
    if (freeSlot < 0) {
        owner_.addSystemChatMessage("Cannot unequip: no free backpack slots.");
        return;
    }

    uint8_t srcBag = 0xFF;
    uint8_t srcSlot = static_cast<uint8_t>(equipSlot);
    uint8_t dstBag = 0xFF;
    uint8_t dstSlot = static_cast<uint8_t>(23 + freeSlot);

    LOG_INFO("UnequipToBackpack: equipSlot=", (int)srcSlot,
             " -> backpackIndex=", freeSlot, " (dstSlot=", (int)dstSlot, ")");

    auto packet = SwapItemPacket::build(dstBag, dstSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::useItemById(uint32_t itemId) {
    if (itemId == 0) return;
    LOG_DEBUG("useItemById: searching for itemId=", itemId);
    for (int i = 0; i < owner_.inventory.getBackpackSize(); i++) {
        const auto& slot = owner_.inventory.getBackpackSlot(i);
        if (!slot.empty() && slot.item.itemId == itemId) {
            LOG_DEBUG("useItemById: found itemId=", itemId, " at backpack slot ", i);
            useItemBySlot(i);
            return;
        }
    }
    for (int bag = 0; bag < owner_.inventory.NUM_BAG_SLOTS; bag++) {
        int bagSize = owner_.inventory.getBagSize(bag);
        for (int slot = 0; slot < bagSize; slot++) {
            const auto& bagSlot = owner_.inventory.getBagSlot(bag, slot);
            if (!bagSlot.empty() && bagSlot.item.itemId == itemId) {
                LOG_DEBUG("useItemById: found itemId=", itemId, " in bag ", bag, " slot ", slot);
                useItemInBag(bag, slot);
                return;
            }
        }
    }
    LOG_WARNING("useItemById: itemId=", itemId, " not found in inventory");
}

void InventoryHandler::handleListInventory(network::Packet& packet) {
    if (!ListInventoryParser::parse(packet, currentVendorItems_)) return;
    vendorWindowOpen_ = true;
    owner_.gossipWindowOpen = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("MERCHANT_SHOW", {});

    // Auto-sell grey items
    if (autoSellGrey_ && currentVendorItems_.vendorGuid != 0 && owner_.state == WorldState::IN_WORLD && owner_.socket) {
        int itemsSold = 0;
        uint32_t totalSellPrice = 0;
        for (int i = 0; i < owner_.inventory.getBackpackSize(); ++i) {
            const auto& slot = owner_.inventory.getBackpackSlot(i);
            if (slot.empty()) continue;
            uint32_t quality = 0;
            uint32_t sellPrice = slot.item.sellPrice;
            if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
                quality = info->quality;
                if (sellPrice == 0) sellPrice = info->sellPrice;
            }
            if (quality == 0 && sellPrice > 0) {
                uint64_t itemGuid = owner_.backpackSlotGuids_[i];
                if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
                if (itemGuid != 0) {
                    sellItem(currentVendorItems_.vendorGuid, itemGuid, 1);
                    totalSellPrice += sellPrice;
                    ++itemsSold;
                }
            }
        }
        for (int b = 0; b < owner_.inventory.NUM_BAG_SLOTS; ++b) {
            int bagSize = owner_.inventory.getBagSize(b);
            for (int s = 0; s < bagSize; ++s) {
                const auto& slot = owner_.inventory.getBagSlot(b, s);
                if (slot.empty()) continue;
                uint32_t quality = 0;
                uint32_t sellPrice = slot.item.sellPrice;
                if (auto* info = owner_.getItemInfo(slot.item.itemId); info && info->valid) {
                    quality = info->quality;
                    if (sellPrice == 0) sellPrice = info->sellPrice;
                }
                if (quality == 0 && sellPrice > 0) {
                    uint64_t itemGuid = 0;
                    uint64_t bagGuid = owner_.equipSlotGuids_[19 + b];
                    if (bagGuid != 0) {
                        auto cit = owner_.containerContents_.find(bagGuid);
                        if (cit != owner_.containerContents_.end() && s < static_cast<int>(cit->second.numSlots))
                            itemGuid = cit->second.slotGuids[s];
                    }
                    if (itemGuid == 0) itemGuid = owner_.resolveOnlineItemGuid(slot.item.itemId);
                    if (itemGuid != 0) {
                        sellItem(currentVendorItems_.vendorGuid, itemGuid, 1);
                        totalSellPrice += sellPrice;
                        ++itemsSold;
                    }
                }
            }
        }
        if (itemsSold > 0) {
            uint32_t gold = totalSellPrice / 10000;
            uint32_t silver = (totalSellPrice % 10000) / 100;
            uint32_t copper = totalSellPrice % 100;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "|cffaaaaaaAuto-sold %d grey item%s for %ug %us %uc.|r",
                itemsSold, itemsSold == 1 ? "" : "s", gold, silver, copper);
            owner_.addSystemChatMessage(buf);
        }
    }

    // Auto-repair
    if (autoRepair_ && currentVendorItems_.canRepair && currentVendorItems_.vendorGuid != 0) {
        bool anyDamaged = false;
        for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i) {
            const auto& slot = owner_.inventory.getEquipSlot(static_cast<EquipSlot>(i));
            if (!slot.empty() && slot.item.maxDurability > 0
                    && slot.item.curDurability < slot.item.maxDurability) {
                anyDamaged = true;
                break;
            }
        }
        if (anyDamaged) {
            repairAll(currentVendorItems_.vendorGuid, false);
            owner_.addSystemChatMessage("|cffaaaaaaAuto-repair triggered.|r");
        }
    }

    // Play vendor sound
    if (owner_.npcVendorCallback_ && currentVendorItems_.vendorGuid != 0) {
        auto entity = owner_.entityManager.getEntity(currentVendorItems_.vendorGuid);
        if (entity && entity->getType() == ObjectType::UNIT) {
            glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
            owner_.npcVendorCallback_(currentVendorItems_.vendorGuid, pos);
        }
    }

    for (const auto& item : currentVendorItems_.items) {
        owner_.queryItemInfo(item.itemId, 0);
    }
}

// ============================================================
// Trainer
// ============================================================

void InventoryHandler::handleTrainerList(network::Packet& packet) {
    const bool isClassic = isClassicLikeExpansion();
    if (!TrainerListParser::parse(packet, currentTrainerList_, isClassic)) return;
    trainerWindowOpen_ = true;
    owner_.gossipWindowOpen = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRAINER_SHOW", {});

    LOG_INFO("Trainer list: ", currentTrainerList_.spells.size(), " spells");

    owner_.loadSpellNameCache();
    owner_.loadSkillLineDbc();
    owner_.loadSkillLineAbilityDbc();
    categorizeTrainerSpells();
}

void InventoryHandler::trainSpell(uint32_t spellId) {
    LOG_INFO("trainSpell called: spellId=", spellId, " state=", (int)owner_.state, " socket=", (owner_.socket ? "yes" : "no"));
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) {
        LOG_WARNING("trainSpell: Not in world or no socket connection");
        return;
    }

    uint32_t spellCost = 0;
    for (const auto& spell : currentTrainerList_.spells) {
        if (spell.spellId == spellId) {
            spellCost = spell.spellCost;
            break;
        }
    }
    LOG_INFO("Player money: ", owner_.playerMoneyCopper_, " copper, spell cost: ", spellCost, " copper");

    LOG_INFO("Sending CMSG_TRAINER_BUY_SPELL: guid=", currentTrainerList_.trainerGuid,
             " spellId=", spellId);
    auto packet = TrainerBuySpellPacket::build(
        currentTrainerList_.trainerGuid,
        spellId);
    owner_.socket->send(packet);
    LOG_INFO("CMSG_TRAINER_BUY_SPELL sent");
}

void InventoryHandler::closeTrainer() {
    trainerWindowOpen_ = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRAINER_CLOSED", {});
    currentTrainerList_ = TrainerListData{};
    trainerTabs_.clear();
}

void InventoryHandler::categorizeTrainerSpells() {
    trainerTabs_.clear();

    static constexpr uint32_t SKILLLINE_CATEGORY_CLASS = 7;

    std::map<uint32_t, std::vector<const TrainerSpell*>> specialtySpells;
    std::vector<const TrainerSpell*> generalSpells;

    for (const auto& spell : currentTrainerList_.spells) {
        auto slIt = owner_.spellToSkillLine_.find(spell.spellId);
        if (slIt != owner_.spellToSkillLine_.end()) {
            uint32_t skillLineId = slIt->second;
            auto catIt = owner_.skillLineCategories_.find(skillLineId);
            if (catIt != owner_.skillLineCategories_.end() && catIt->second == SKILLLINE_CATEGORY_CLASS) {
                specialtySpells[skillLineId].push_back(&spell);
                continue;
            }
        }
        generalSpells.push_back(&spell);
    }

    auto byName = [this](const TrainerSpell* a, const TrainerSpell* b) {
        return owner_.getSpellName(a->spellId) < owner_.getSpellName(b->spellId);
    };

    std::vector<std::pair<std::string, std::vector<const TrainerSpell*>>> named;
    for (auto& [skillLineId, spells] : specialtySpells) {
        auto nameIt = owner_.skillLineNames_.find(skillLineId);
        std::string tabName = (nameIt != owner_.skillLineNames_.end()) ? nameIt->second : "Specialty";
        std::sort(spells.begin(), spells.end(), byName);
        named.push_back({std::move(tabName), std::move(spells)});
    }
    std::sort(named.begin(), named.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& [name, spells] : named) {
        trainerTabs_.push_back({std::move(name), std::move(spells)});
    }

    if (!generalSpells.empty()) {
        std::sort(generalSpells.begin(), generalSpells.end(), byName);
        trainerTabs_.push_back({"General", std::move(generalSpells)});
    }

    LOG_INFO("Trainer: Categorized into ", trainerTabs_.size(), " tabs");
}

// ============================================================
// Mail
// ============================================================

void InventoryHandler::closeMailbox() {
    mailboxOpen_ = false;
    mailboxGuid_ = 0;
    showMailCompose_ = false;
    clearMailAttachments();
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("MAIL_CLOSED", {});
}

void InventoryHandler::refreshMailList() {
    if (!mailboxOpen_ || mailboxGuid_ == 0) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = GetMailListPacket::build(mailboxGuid_);
    owner_.socket->send(packet);
}

void InventoryHandler::sendMail(const std::string& recipient, const std::string& subject,
                                const std::string& body, uint64_t money, uint64_t cod) {
    if (owner_.state != WorldState::IN_WORLD) {
        LOG_WARNING("sendMail: not in world");
        return;
    }
    if (!owner_.socket) {
        LOG_WARNING("sendMail: no socket");
        return;
    }
    if (mailboxGuid_ == 0) {
        LOG_WARNING("sendMail: mailboxGuid_ is 0 (mailbox closed?)");
        return;
    }
    // Collect attached item GUIDs
    std::vector<uint64_t> itemGuids;
    for (const auto& att : mailAttachments_) {
        if (att.occupied()) {
            itemGuids.push_back(att.itemGuid);
        }
    }
    auto packet = owner_.packetParsers_->buildSendMail(mailboxGuid_, recipient, subject, body, money, cod, itemGuids);
    LOG_INFO("sendMail: to='", recipient, "' subject='", subject, "' money=", money,
             " attachments=", itemGuids.size(), " mailboxGuid=", mailboxGuid_);
    owner_.socket->send(packet);
    clearMailAttachments();
}

bool InventoryHandler::attachItemFromBackpack(int backpackIndex) {
    if (backpackIndex < 0 || backpackIndex >= owner_.inventory.getBackpackSize()) return false;
    const auto& slot = owner_.inventory.getBackpackSlot(backpackIndex);
    if (slot.empty()) return false;
    uint64_t itemGuid = owner_.backpackSlotGuids_[backpackIndex];
    if (itemGuid == 0) return false;
    for (int i = 0; i < MAIL_MAX_ATTACHMENTS; ++i) {
        if (!mailAttachments_[i].occupied()) {
            mailAttachments_[i].itemGuid = itemGuid;
            mailAttachments_[i].item = slot.item;
            mailAttachments_[i].srcBag = 0xFF;
            mailAttachments_[i].srcSlot = static_cast<uint8_t>(23 + backpackIndex);
            return true;
        }
    }
    return false;
}

bool InventoryHandler::attachItemFromBag(int bagIndex, int slotIndex) {
    if (bagIndex < 0 || bagIndex >= owner_.inventory.NUM_BAG_SLOTS) return false;
    if (slotIndex < 0 || slotIndex >= owner_.inventory.getBagSize(bagIndex)) return false;
    const auto& slot = owner_.inventory.getBagSlot(bagIndex, slotIndex);
    if (slot.empty()) return false;
    uint64_t bagGuid = owner_.equipSlotGuids_[19 + bagIndex];
    if (bagGuid == 0) return false;
    auto it = owner_.containerContents_.find(bagGuid);
    if (it == owner_.containerContents_.end()) return false;
    if (slotIndex >= static_cast<int>(it->second.numSlots)) return false;
    uint64_t itemGuid = it->second.slotGuids[slotIndex];
    if (itemGuid == 0) return false;
    for (int i = 0; i < MAIL_MAX_ATTACHMENTS; ++i) {
        if (!mailAttachments_[i].occupied()) {
            mailAttachments_[i].itemGuid = itemGuid;
            mailAttachments_[i].item = slot.item;
            mailAttachments_[i].srcBag = static_cast<uint8_t>(19 + bagIndex);
            mailAttachments_[i].srcSlot = static_cast<uint8_t>(slotIndex);
            return true;
        }
    }
    return false;
}

bool InventoryHandler::detachMailAttachment(int attachIndex) {
    if (attachIndex < 0 || attachIndex >= MAIL_MAX_ATTACHMENTS) return false;
    mailAttachments_[attachIndex] = MailAttachSlot{};
    return true;
}

void InventoryHandler::clearMailAttachments() {
    for (auto& a : mailAttachments_) a = MailAttachSlot{};
}

int InventoryHandler::getMailAttachmentCount() const {
    int count = 0;
    for (const auto& a : mailAttachments_)
        if (a.occupied()) ++count;
    return count;
}

void InventoryHandler::mailTakeMoney(uint32_t mailId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || mailboxGuid_ == 0) return;
    auto packet = MailTakeMoneyPacket::build(mailboxGuid_, mailId);
    owner_.socket->send(packet);
}

void InventoryHandler::mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || mailboxGuid_ == 0) return;
    auto packet = MailTakeItemPacket::build(mailboxGuid_, mailId, itemGuidLow);
    owner_.socket->send(packet);
}

void InventoryHandler::mailDelete(uint32_t mailId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || mailboxGuid_ == 0) return;
    auto packet = MailDeletePacket::build(mailboxGuid_, mailId, 0);
    owner_.socket->send(packet);
}

void InventoryHandler::mailMarkAsRead(uint32_t mailId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || mailboxGuid_ == 0) return;
    auto packet = MailMarkAsReadPacket::build(mailboxGuid_, mailId);
    owner_.socket->send(packet);
}

void InventoryHandler::handleShowMailbox(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 8) return;
    mailboxGuid_ = packet.readUInt64();
    mailboxOpen_ = true;
    selectedMailIndex_ = -1;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("MAIL_SHOW", {});
    refreshMailList();
}

void InventoryHandler::handleMailListResult(network::Packet& packet) {
    if (!owner_.packetParsers_->parseMailList(packet, mailInbox_)) return;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("MAIL_INBOX_UPDATE", {});
    for (const auto& mail : mailInbox_) {
        for (const auto& att : mail.attachments) {
            if (att.itemId != 0) owner_.ensureItemInfo(att.itemId);
        }
    }
}

void InventoryHandler::handleSendMailResult(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 12) return;
    uint32_t mailId = packet.readUInt32();
    uint32_t action = packet.readUInt32();
    uint32_t error  = packet.readUInt32();
    (void)mailId;
    if (action == 0) { // SEND
        if (error == 0) {
            owner_.addSystemChatMessage("Mail sent.");
            clearMailAttachments();
            showMailCompose_ = false;
        } else {
            owner_.addSystemChatMessage("Failed to send mail (error " + std::to_string(error) + ").");
        }
    } else if (action == 4) { // TAKE_ITEM
        if (error == 0) {
            owner_.addSystemChatMessage("Item taken from mail.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("BAG_UPDATE", {});
        } else {
            owner_.addSystemChatMessage("Failed to take item (error " + std::to_string(error) + ").");
        }
    } else if (action == 5) { // TAKE_MONEY
        if (error == 0) {
            owner_.addSystemChatMessage("Money taken from mail.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYER_MONEY", {});
        }
    } else if (action == 2) { // DELETE
        if (error == 0) {
            owner_.addSystemChatMessage("Mail deleted.");
        }
    }
    refreshMailList();
}

void InventoryHandler::handleReceivedMail(network::Packet& packet) {
    (void)packet;
    hasNewMail_ = true;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("UPDATE_PENDING_MAIL", {});
}

void InventoryHandler::handleQueryNextMailTime(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 8) return;
    float nextTime = *reinterpret_cast<const float*>(&packet.getData()[packet.getReadPos()]);
    packet.readUInt32(); // skip
    uint32_t count = packet.readUInt32();
    hasNewMail_ = (nextTime >= 0.0f && count > 0);
    packet.setReadPos(packet.getSize());
}

// ============================================================
// Bank
// ============================================================

void InventoryHandler::openBank(uint64_t guid) {
    bankerGuid_ = guid;
    bankOpen_ = true;
    if (isClassicLikeExpansion()) {
        effectiveBankSlots_ = 24;
        effectiveBankBagSlots_ = 6;
    } else {
        effectiveBankSlots_ = 28;
        effectiveBankBagSlots_ = 7;
    }
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("BANKFRAME_OPENED", {});
}

void InventoryHandler::closeBank() {
    bankOpen_ = false;
    bankerGuid_ = 0;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("BANKFRAME_CLOSED", {});
}

void InventoryHandler::buyBankSlot() {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || bankerGuid_ == 0) return;
    auto packet = BuyBankSlotPacket::build(bankerGuid_);
    owner_.socket->send(packet);
}

void InventoryHandler::depositItem(uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    int freeBankSlot = -1;
    for (int i = 0; i < effectiveBankSlots_; ++i) {
        if (bankSlotGuids_[i] == 0) { freeBankSlot = i; break; }
    }
    if (freeBankSlot < 0) {
        owner_.addSystemChatMessage("Bank is full.");
        return;
    }
    uint8_t dstSlot = static_cast<uint8_t>(39 + freeBankSlot);
    auto packet = SwapItemPacket::build(0xFF, dstSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::withdrawItem(uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    int freeSlot = owner_.inventory.findFreeBackpackSlot();
    if (freeSlot < 0) {
        owner_.addSystemChatMessage("Inventory is full.");
        return;
    }
    uint8_t dstSlot = static_cast<uint8_t>(23 + freeSlot);
    auto packet = SwapItemPacket::build(0xFF, dstSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::handleShowBank(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 8) return;
    uint64_t guid = packet.readUInt64();
    openBank(guid);
}

void InventoryHandler::handleBuyBankSlotResult(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t result = packet.readUInt32();
    if (result == 0) {
        owner_.addSystemChatMessage("Bank slot purchased.");
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("PLAYERBANKBAGSLOTS_CHANGED", {});
    } else {
        owner_.addSystemChatMessage("Failed to purchase bank slot.");
    }
}

// ============================================================
// Guild Bank
// ============================================================

void InventoryHandler::openGuildBank(uint64_t guid) {
    guildBankerGuid_ = guid;
    guildBankOpen_ = true;
    guildBankActiveTab_ = 0;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GUILDBANKFRAME_OPENED", {});
    queryGuildBankTab(0);
}

void InventoryHandler::closeGuildBank() {
    guildBankOpen_ = false;
    guildBankerGuid_ = 0;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GUILDBANKFRAME_CLOSED", {});
}

void InventoryHandler::queryGuildBankTab(uint8_t tabId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    auto packet = GuildBankQueryTabPacket::build(guildBankerGuid_, tabId, false);
    owner_.socket->send(packet);
}

void InventoryHandler::buyGuildBankTab() {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    uint8_t nextTab = static_cast<uint8_t>(guildBankData_.tabs.size());
    auto packet = GuildBankBuyTabPacket::build(guildBankerGuid_, nextTab);
    owner_.socket->send(packet);
}

void InventoryHandler::depositGuildBankMoney(uint32_t amount) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    auto packet = GuildBankDepositMoneyPacket::build(guildBankerGuid_, amount);
    owner_.socket->send(packet);
}

void InventoryHandler::withdrawGuildBankMoney(uint32_t amount) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    auto packet = GuildBankWithdrawMoneyPacket::build(guildBankerGuid_, amount);
    owner_.socket->send(packet);
}

void InventoryHandler::guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag, uint8_t destSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    auto packet = GuildBankSwapItemsPacket::buildBankToInventory(guildBankerGuid_, tabId, bankSlot, destBag, destSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || guildBankerGuid_ == 0) return;
    auto packet = GuildBankSwapItemsPacket::buildInventoryToBank(guildBankerGuid_, tabId, bankSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::handleGuildBankList(network::Packet& packet) {
    if (!GuildBankListParser::parse(packet, guildBankData_)) return;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("GUILDBANKBAGSLOTS_CHANGED", {});
    for (const auto& tab : guildBankData_.tabs) {
        for (const auto& item : tab.items) {
            if (item.itemEntry != 0) owner_.ensureItemInfo(item.itemEntry);
        }
    }
}

// ============================================================
// Auction House
// ============================================================

void InventoryHandler::openAuctionHouse(uint64_t guid) {
    auctioneerGuid_ = guid;
    auctionOpen_ = true;
    auctionActiveTab_ = 0;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_HOUSE_SHOW", {});
}

void InventoryHandler::closeAuctionHouse() {
    auctionOpen_ = false;
    auctioneerGuid_ = 0;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_HOUSE_CLOSED", {});
}

void InventoryHandler::auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                                      uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                                      uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    lastAuctionSearch_ = {name, levelMin, levelMax, quality, itemClass, itemSubClass, invTypeMask, usableOnly, offset};
    pendingAuctionTarget_ = AuctionResultTarget::BROWSE;
    auto packet = AuctionListItemsPacket::build(auctioneerGuid_, offset, name,
                                                  levelMin, levelMax, invTypeMask,
                                                  itemClass, itemSubClass, quality, usableOnly, 0);
    owner_.socket->send(packet);
    auctionSearchDelayTimer_ = 5.0f;
}

void InventoryHandler::auctionSellItem(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                                        uint32_t buyout, uint32_t duration) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    auto packet = AuctionSellItemPacket::build(auctioneerGuid_, itemGuid, stackCount, bid, buyout, duration);
    owner_.socket->send(packet);
}

void InventoryHandler::auctionPlaceBid(uint32_t auctionId, uint32_t amount) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    auto packet = AuctionPlaceBidPacket::build(auctioneerGuid_, auctionId, amount);
    owner_.socket->send(packet);
}

void InventoryHandler::auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    auto packet = AuctionPlaceBidPacket::build(auctioneerGuid_, auctionId, buyoutPrice);
    owner_.socket->send(packet);
}

void InventoryHandler::auctionCancelItem(uint32_t auctionId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    auto packet = AuctionRemoveItemPacket::build(auctioneerGuid_, auctionId);
    owner_.socket->send(packet);
}

void InventoryHandler::auctionListOwnerItems(uint32_t offset) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    pendingAuctionTarget_ = AuctionResultTarget::OWNER;
    auto packet = AuctionListOwnerItemsPacket::build(auctioneerGuid_, offset);
    owner_.socket->send(packet);
}

void InventoryHandler::auctionListBidderItems(uint32_t offset) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket || auctioneerGuid_ == 0) return;
    pendingAuctionTarget_ = AuctionResultTarget::BIDDER;
    auto packet = AuctionListBidderItemsPacket::build(auctioneerGuid_, offset);
    owner_.socket->send(packet);
}

void InventoryHandler::handleAuctionHello(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 12) return;
    uint64_t guid = packet.readUInt64();
    uint32_t houseId = packet.readUInt32();
    auctioneerGuid_ = guid;
    auctionHouseId_ = houseId;
    auctionOpen_ = true;
    auctionActiveTab_ = 0;
    owner_.gossipWindowOpen = false;
    if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_HOUSE_SHOW", {});
}

void InventoryHandler::handleAuctionListResult(network::Packet& packet) {
    AuctionListResult result;
    if (!AuctionListResultParser::parse(packet, result)) return;

    if (pendingAuctionTarget_ == AuctionResultTarget::OWNER) {
        auctionOwnerResults_ = std::move(result);
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_OWNED_LIST_UPDATE", {});
    } else if (pendingAuctionTarget_ == AuctionResultTarget::BIDDER) {
        auctionBidderResults_ = std::move(result);
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_BIDDER_LIST_UPDATE", {});
    } else {
        auctionBrowseResults_ = std::move(result);
        if (owner_.addonEventCallback_) owner_.addonEventCallback_("AUCTION_ITEM_LIST_UPDATE", {});
    }

    // Ensure item info for all entries
    auto ensureEntries = [this](const AuctionListResult& r) {
        for (const auto& e : r.auctions) {
            owner_.ensureItemInfo(e.itemEntry);
        }
    };
    if (pendingAuctionTarget_ == AuctionResultTarget::OWNER) ensureEntries(auctionOwnerResults_);
    else if (pendingAuctionTarget_ == AuctionResultTarget::BIDDER) ensureEntries(auctionBidderResults_);
    else ensureEntries(auctionBrowseResults_);
}

void InventoryHandler::handleAuctionOwnerListResult(network::Packet& packet) {
    pendingAuctionTarget_ = AuctionResultTarget::OWNER;
    handleAuctionListResult(packet);
}

void InventoryHandler::handleAuctionBidderListResult(network::Packet& packet) {
    pendingAuctionTarget_ = AuctionResultTarget::BIDDER;
    handleAuctionListResult(packet);
}

void InventoryHandler::handleAuctionCommandResult(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 12) return;
    uint32_t auctionId = packet.readUInt32();
    uint32_t action = packet.readUInt32();
    uint32_t error  = packet.readUInt32();
    (void)auctionId;

    const char* actionNames[] = {"sell", "cancel", "bid/buyout"};
    const char* actionStr = (action < 3) ? actionNames[action] : "unknown";

    if (error == 0) {
        std::string msg = std::string("Auction ") + actionStr + " successful.";
        owner_.addSystemChatMessage(msg);
        if (owner_.addonEventCallback_) {
            owner_.addonEventCallback_("PLAYER_MONEY", {});
            owner_.addonEventCallback_("BAG_UPDATE", {});
        }
        // Re-query after successful buy/bid
        if (action == 2 && lastAuctionSearch_.name.length() > 0) {
            auctionSearch(lastAuctionSearch_.name, lastAuctionSearch_.levelMin, lastAuctionSearch_.levelMax,
                         lastAuctionSearch_.quality, lastAuctionSearch_.itemClass, lastAuctionSearch_.itemSubClass,
                         lastAuctionSearch_.invTypeMask, lastAuctionSearch_.usableOnly, lastAuctionSearch_.offset);
        }
    } else {
        const char* errMsg = "Unknown error.";
        switch (error) {
            case 1: errMsg = "Not enough money."; break;
            case 2: errMsg = "Item not found."; break;
            case 5: errMsg = "Bid too low."; break;
            case 6: errMsg = "Bid increment too low."; break;
            case 7: errMsg = "You cannot bid on your own auction."; break;
            case 8: errMsg = "Database error."; break;
            default: break;
        }
        owner_.addUIError(std::string("Auction ") + actionStr + " failed: " + errMsg);
        owner_.addSystemChatMessage(std::string("Auction ") + actionStr + " failed: " + errMsg);
    }
}

// ============================================================
// Item Text
// ============================================================

void InventoryHandler::queryItemText(uint64_t itemGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_ITEM_TEXT_QUERY));
    pkt.writeUInt64(itemGuid);
    owner_.socket->send(pkt);
}

void InventoryHandler::handleItemTextQueryResponse(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 1) return;
    std::string text = packet.readString();
    if (!text.empty()) {
        itemText_ = std::move(text);
        itemTextOpen_ = true;
    }
}

// ============================================================
// Trade
// ============================================================

void InventoryHandler::acceptTradeRequest() {
    if (tradeStatus_ != TradeStatus::PendingIncoming) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = BeginTradePacket::build();
    owner_.socket->send(packet);
}

void InventoryHandler::declineTradeRequest() {
    if (tradeStatus_ != TradeStatus::PendingIncoming) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = CancelTradePacket::build();
    owner_.socket->send(packet);
    resetTradeState();
}

void InventoryHandler::acceptTrade() {
    if (tradeStatus_ != TradeStatus::Open) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = AcceptTradePacket::build();
    owner_.socket->send(packet);
}

void InventoryHandler::cancelTrade() {
    if (tradeStatus_ == TradeStatus::None) return;
    if (owner_.state == WorldState::IN_WORLD && owner_.socket) {
        auto packet = CancelTradePacket::build();
        owner_.socket->send(packet);
    }
    resetTradeState();
}

void InventoryHandler::setTradeItem(uint8_t tradeSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (tradeStatus_ != TradeStatus::Open) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = SetTradeItemPacket::build(tradeSlot, srcBag, srcSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::clearTradeItem(uint8_t tradeSlot) {
    if (tradeStatus_ != TradeStatus::Open) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = ClearTradeItemPacket::build(tradeSlot);
    owner_.socket->send(packet);
}

void InventoryHandler::setTradeGold(uint64_t amount) {
    if (tradeStatus_ != TradeStatus::Open) return;
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    auto packet = SetTradeGoldPacket::build(static_cast<uint32_t>(amount));
    owner_.socket->send(packet);
}

void InventoryHandler::resetTradeState() {
    tradeStatus_ = TradeStatus::None;
    tradePeerGuid_ = 0;
    tradePeerName_.clear();
    myTradeSlots_ = {};
    peerTradeSlots_ = {};
    myTradeGold_ = 0;
    peerTradeGold_ = 0;
}

void InventoryHandler::handleTradeStatus(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t status = packet.readUInt32();
    switch (status) {
        case 0: // TRADE_STATUS_PLAYER_BUSY
            resetTradeState();
            owner_.addSystemChatMessage("Trade failed: player is busy.");
            break;
        case 1: { // TRADE_STATUS_PROPOSED
            if (packet.getSize() - packet.getReadPos() >= 8)
                tradePeerGuid_ = packet.readUInt64();
            tradeStatus_ = TradeStatus::PendingIncoming;
            // Resolve name
            auto nit = owner_.playerNameCache.find(tradePeerGuid_);
            if (nit != owner_.playerNameCache.end()) tradePeerName_ = nit->second;
            else tradePeerName_ = "Unknown";
            owner_.addSystemChatMessage(tradePeerName_ + " wants to trade with you.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_REQUEST", {tradePeerName_});
            break;
        }
        case 2: // TRADE_STATUS_INITIATED
            tradeStatus_ = TradeStatus::Open;
            owner_.addSystemChatMessage("Trade opened.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_SHOW", {});
            break;
        case 3: // TRADE_STATUS_CANCELLED
            resetTradeState();
            owner_.addSystemChatMessage("Trade cancelled.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_CLOSED", {});
            break;
        case 4: // TRADE_STATUS_ACCEPTED
            tradeStatus_ = TradeStatus::Accepted;
            owner_.addSystemChatMessage("Trade partner accepted.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_ACCEPT_UPDATE", {});
            break;
        case 5: // TRADE_STATUS_ALREADY_TRADING
            owner_.addSystemChatMessage("You are already trading.");
            break;
        case 7: // TRADE_STATUS_COMPLETE
            resetTradeState();
            owner_.addSystemChatMessage("Trade complete.");
            if (owner_.addonEventCallback_) {
                owner_.addonEventCallback_("TRADE_CLOSED", {});
                owner_.addonEventCallback_("BAG_UPDATE", {});
                owner_.addonEventCallback_("PLAYER_MONEY", {});
            }
            break;
        case 9: // TRADE_STATUS_TARGET_TO_FAR
            resetTradeState();
            owner_.addSystemChatMessage("Trade failed: target is too far away.");
            break;
        case 13: // TRADE_STATUS_FAILED
            resetTradeState();
            owner_.addSystemChatMessage("Trade failed.");
            if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_CLOSED", {});
            break;
        case 17: // TRADE_STATUS_PETITION
            owner_.addSystemChatMessage("You cannot trade while petition is active.");
            break;
        case 18: // TRADE_STATUS_PLAYER_IGNORED
            owner_.addSystemChatMessage("That player is ignoring you.");
            break;
        default:
            LOG_DEBUG("Unhandled SMSG_TRADE_STATUS: ", status);
            break;
    }
}

void InventoryHandler::handleTradeStatusExtended(network::Packet& packet) {
    // Parse trade items from both players
    // WotLK: whichPlayer(1) + 7 items × (slot(1) + itemId(4) + displayId(4) + stackCount(4) + ...
    //        + enchant(4) + creator(8) + suffixFactor(4) + charges(4)) + gold(4)
    if (packet.getSize() - packet.getReadPos() < 1) return;
    uint8_t whichPlayer = packet.readUInt8();
    // 0 = own items, 1 = peer items
    auto& slots = (whichPlayer == 0) ? myTradeSlots_ : peerTradeSlots_;

    // Read trader item count (up to 7, but we only track TRADE_SLOT_COUNT = 6)
    uint32_t tradeCount = packet.readUInt32();
    if (tradeCount > 7) tradeCount = 7;

    for (uint32_t i = 0; i < tradeCount; ++i) {
        if (packet.getSize() - packet.getReadPos() < 1) break;
        uint8_t slotNum = packet.readUInt8();
        if (packet.getSize() - packet.getReadPos() < 60) { packet.setReadPos(packet.getSize()); return; }
        uint32_t itemId    = packet.readUInt32();
        uint32_t displayId = packet.readUInt32();
        uint32_t stackCnt  = packet.readUInt32();
        /*uint32_t unk1 =*/ packet.readUInt32(); // wrapped?
        uint64_t giftCreator = packet.readUInt64();
        uint32_t enchant    = packet.readUInt32();
        for (int g = 0; g < 3; ++g) packet.readUInt32(); // gem enchant IDs
        /*uint32_t maxDur =*/ packet.readUInt32();
        /*uint32_t curDur =*/ packet.readUInt32();
        /*uint32_t unk3 =*/ packet.readUInt32();
        (void)enchant; (void)giftCreator;

        if (slotNum < TRADE_SLOT_COUNT) {
            slots[slotNum].itemId = itemId;
            slots[slotNum].displayId = displayId;
            slots[slotNum].stackCount = stackCnt;
        }
        if (itemId != 0) owner_.ensureItemInfo(itemId);
    }

    // Gold
    if (packet.getSize() - packet.getReadPos() >= 4) {
        uint32_t gold = packet.readUInt32();
        if (whichPlayer == 0) myTradeGold_ = gold;
        else peerTradeGold_ = gold;
    }

    if (owner_.addonEventCallback_) owner_.addonEventCallback_("TRADE_UPDATE", {});
}

// ============================================================
// Equipment Sets
// ============================================================

bool InventoryHandler::supportsEquipmentSets() const {
    return isActiveExpansion("wotlk");
}

void InventoryHandler::useEquipmentSet(uint32_t setId) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_EQUIPMENT_SET_USE);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    const EquipmentSet* es = nullptr;
    for (const auto& s : equipmentSets_) {
        if (s.setId == setId) { es = &s; break; }
    }
    if (!es) {
        owner_.addSystemChatMessage("Equipment set not found.");
        return;
    }
    network::Packet pkt(wire);
    for (int slot = 0; slot < 19; ++slot) {
        uint64_t itemGuid = es->itemGuids[slot];
        pkt.writePackedGuid(itemGuid);
        uint8_t srcBag = 0xFF;
        uint8_t srcSlot = 0;
        if (itemGuid != 0) {
            bool found = false;
            for (int eq = 0; eq < 19 && !found; ++eq) {
                if (owner_.getEquipSlotGuid(eq) == itemGuid) {
                    srcBag = 0xFF;
                    srcSlot = static_cast<uint8_t>(eq);
                    found = true;
                }
            }
            for (int bp = 0; bp < 16 && !found; ++bp) {
                if (owner_.getBackpackItemGuid(bp) == itemGuid) {
                    srcBag = 0xFF;
                    srcSlot = static_cast<uint8_t>(23 + bp);
                    found = true;
                }
            }
            for (int bag = 0; bag < 4 && !found; ++bag) {
                int bagSize = owner_.inventory.getBagSize(bag);
                for (int s = 0; s < bagSize && !found; ++s) {
                    if (owner_.getBagItemGuid(bag, s) == itemGuid) {
                        srcBag = static_cast<uint8_t>(19 + bag);
                        srcSlot = static_cast<uint8_t>(s);
                        found = true;
                    }
                }
            }
        }
        pkt.writeUInt8(srcBag);
        pkt.writeUInt8(srcSlot);
    }
    owner_.socket->send(pkt);
}

void InventoryHandler::saveEquipmentSet(const std::string& name, const std::string& iconName,
                                         uint64_t existingGuid, uint32_t setIndex) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_EQUIPMENT_SET_SAVE);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    pendingSaveSetName_ = name;
    pendingSaveSetIcon_ = iconName;
    if (setIndex == 0xFFFFFFFF) {
        setIndex = 0;
        for (const auto& es : equipmentSets_) {
            if (es.setId >= setIndex) setIndex = es.setId + 1;
        }
    }
    network::Packet pkt(wire);
    pkt.writeUInt64(existingGuid);
    pkt.writeUInt32(setIndex);
    pkt.writeString(name);
    pkt.writeString(iconName);
    for (int i = 0; i < 19; ++i) {
        pkt.writePackedGuid(owner_.getEquipSlotGuid(i));
    }
    owner_.socket->send(pkt);
}

void InventoryHandler::deleteEquipmentSet(uint64_t setGuid) {
    if (owner_.state != WorldState::IN_WORLD || !owner_.socket) return;
    uint16_t wire = wireOpcode(Opcode::CMSG_DELETEEQUIPMENT_SET);
    if (wire == 0xFFFF) { owner_.addUIError("Equipment sets not supported."); return; }
    network::Packet pkt(wire);
    pkt.writeUInt64(setGuid);
    owner_.socket->send(pkt);
    equipmentSets_.erase(
        std::remove_if(equipmentSets_.begin(), equipmentSets_.end(),
                        [setGuid](const EquipmentSet& es) { return es.setGuid == setGuid; }),
        equipmentSets_.end());
    equipmentSetInfo_.erase(
        std::remove_if(equipmentSetInfo_.begin(), equipmentSetInfo_.end(),
                        [setGuid](const EquipmentSetInfo& info) { return info.setGuid == setGuid; }),
        equipmentSetInfo_.end());
}

void InventoryHandler::handleEquipmentSetList(network::Packet& packet) {
    if (packet.getSize() - packet.getReadPos() < 4) return;
    uint32_t count = packet.readUInt32();
    if (count > 10) {
        LOG_WARNING("SMSG_EQUIPMENT_SET_LIST: unexpected count ", count, ", ignoring");
        packet.setReadPos(packet.getSize());
        return;
    }
    equipmentSets_.clear();
    equipmentSets_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getSize() - packet.getReadPos() < 16) break;
        EquipmentSet es;
        es.setGuid        = packet.readUInt64();
        es.setId          = packet.readUInt32();
        es.name           = packet.readString();
        es.iconName       = packet.readString();
        es.ignoreSlotMask = packet.readUInt32();
        for (int slot = 0; slot < 19; ++slot) {
            if (packet.getSize() - packet.getReadPos() < 8) break;
            es.itemGuids[slot] = packet.readUInt64();
        }
        equipmentSets_.push_back(std::move(es));
    }
    equipmentSetInfo_.clear();
    equipmentSetInfo_.reserve(equipmentSets_.size());
    for (const auto& es : equipmentSets_) {
        EquipmentSetInfo info;
        info.setGuid  = es.setGuid;
        info.setId    = es.setId;
        info.name     = es.name;
        info.iconName = es.iconName;
        equipmentSetInfo_.push_back(std::move(info));
    }
    LOG_INFO("SMSG_EQUIPMENT_SET_LIST: ", equipmentSets_.size(), " equipment sets received");
}

// ============================================================
// Inventory field / rebuild methods (moved from GameHandler)
// ============================================================

void InventoryHandler::queryItemInfo(uint32_t entry, uint64_t guid) {
    if (owner_.itemInfoCache_.count(entry) || owner_.pendingItemQueries_.count(entry)) return;
    if (!owner_.isInWorld()) return;

    owner_.pendingItemQueries_.insert(entry);
    // Some cores reject CMSG_ITEM_QUERY_SINGLE when the GUID is 0.
    // If we don't have the item object's GUID (e.g. visible equipment decoding),
    // fall back to the player's GUID to keep the request non-zero.
    uint64_t queryGuid = (guid != 0) ? guid : owner_.playerGuid;
    auto packet = owner_.packetParsers_
        ? owner_.packetParsers_->buildItemQuery(entry, queryGuid)
        : ItemQueryPacket::build(entry, queryGuid);
    owner_.socket->send(packet);
    LOG_DEBUG("queryItemInfo: entry=", entry, " guid=0x", std::hex, queryGuid, std::dec,
              " pending=", owner_.pendingItemQueries_.size());
}

void InventoryHandler::handleItemQueryResponse(network::Packet& packet) {
    ItemQueryResponseData data;
    bool parsed = owner_.packetParsers_
        ? owner_.packetParsers_->parseItemQueryResponse(packet, data)
        : ItemQueryResponseParser::parse(packet, data);
    if (!parsed) {
        LOG_WARNING("handleItemQueryResponse: parse failed, size=", packet.getSize());
        return;
    }

    owner_.pendingItemQueries_.erase(data.entry);
    LOG_DEBUG("handleItemQueryResponse: entry=", data.entry, " name='", data.name,
              "' displayInfoId=", data.displayInfoId, " pending=", owner_.pendingItemQueries_.size());

    if (data.valid) {
        owner_.itemInfoCache_[data.entry] = data;
        rebuildOnlineInventory();
        maybeDetectVisibleItemLayout();

        // Flush any deferred loot notifications waiting on this item's name/quality.
        for (auto it = owner_.pendingItemPushNotifs_.begin(); it != owner_.pendingItemPushNotifs_.end(); ) {
            if (it->itemId == data.entry) {
                std::string itemName = data.name.empty() ? ("item #" + std::to_string(data.entry)) : data.name;
                std::string link = buildItemLink(data.entry, data.quality, itemName);
                std::string msg = "Received: " + link;
                if (it->count > 1) msg += " x" + std::to_string(it->count);
                owner_.addSystemChatMessage(msg);
                if (auto* renderer = core::Application::getInstance().getRenderer()) {
                    if (auto* sfx = renderer->getUiSoundManager()) sfx->playLootItem();
                }
                if (owner_.itemLootCallback_) owner_.itemLootCallback_(data.entry, it->count, data.quality, itemName);
                it = owner_.pendingItemPushNotifs_.erase(it);
            } else {
                ++it;
            }
        }

        // Selectively re-emit only players whose equipment references this item entry
        const uint32_t resolvedEntry = data.entry;
        for (const auto& [guid, entries] : owner_.otherPlayerVisibleItemEntries_) {
            for (uint32_t e : entries) {
                if (e == resolvedEntry) {
                    emitOtherPlayerEquipment(guid);
                    break;
                }
            }
        }
        // Same for inspect-based entries
        if (owner_.playerEquipmentCallback_) {
            for (const auto& [guid, entries] : owner_.inspectedPlayerItemEntries_) {
                bool relevant = false;
                for (uint32_t e : entries) {
                    if (e == resolvedEntry) { relevant = true; break; }
                }
                if (!relevant) continue;
                std::array<uint32_t, 19> displayIds{};
                std::array<uint8_t, 19> invTypes{};
                for (int s = 0; s < 19; s++) {
                    uint32_t entry = entries[s];
                    if (entry == 0) continue;
                    auto infoIt = owner_.itemInfoCache_.find(entry);
                    if (infoIt == owner_.itemInfoCache_.end()) continue;
                    displayIds[s] = infoIt->second.displayInfoId;
                    invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
                }
                owner_.playerEquipmentCallback_(guid, displayIds, invTypes);
            }
        }
    }
}

uint64_t InventoryHandler::resolveOnlineItemGuid(uint32_t itemId) const {
    if (itemId == 0) return 0;
    for (const auto& [guid, info] : owner_.onlineItems_) {
        if (info.entry == itemId) return guid;
    }
    return 0;
}

void InventoryHandler::detectInventorySlotBases(const std::map<uint16_t, uint32_t>& fields) {
    if (owner_.invSlotBase_ >= 0 && owner_.packSlotBase_ >= 0) return;
    if (fields.empty()) return;

    std::vector<uint16_t> matchingPairs;
    matchingPairs.reserve(32);

    for (const auto& [idx, low] : fields) {
        if ((idx % 2) != 0) continue;
        auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
        if (itHigh == fields.end()) continue;
        uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
        if (guid == 0) continue;
        // Primary signal: GUID pairs that match spawned ITEM objects.
        if (!owner_.onlineItems_.empty() && owner_.onlineItems_.count(guid)) {
            matchingPairs.push_back(idx);
        }
    }

    // Fallback signal (when ITEM objects haven't been seen yet):
    // collect any plausible non-zero GUID pairs and derive a base by density.
    if (matchingPairs.empty()) {
        for (const auto& [idx, low] : fields) {
            if ((idx % 2) != 0) continue;
            auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
            if (itHigh == fields.end()) continue;
            uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
            if (guid == 0) continue;
            // Heuristic: item GUIDs tend to be non-trivial and change often; ignore tiny values.
            if (guid < 0x10000ull) continue;
            matchingPairs.push_back(idx);
        }
    }

    if (matchingPairs.empty()) return;
    std::sort(matchingPairs.begin(), matchingPairs.end());

    if (owner_.invSlotBase_ < 0) {
        // The lowest matching field is the first EQUIPPED slot (not necessarily HEAD).
        // With 2+ matches we can derive the true base: all matches must be at
        // even offsets from the base, spaced 2 fields per slot.
        const int knownBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD));
        constexpr int slotStride = 2;
        bool allAlign = true;
        for (uint16_t p : matchingPairs) {
            if (p < knownBase || (p - knownBase) % slotStride != 0) {
                allAlign = false;
                break;
            }
        }
        if (allAlign) {
            owner_.invSlotBase_ = knownBase;
        } else {
            // Fallback: if we have 2+ matches, derive base from their spacing
            if (matchingPairs.size() >= 2) {
                uint16_t lo = matchingPairs[0];
                // lo must be base + 2*slotN, and slotN is 0..22
                // Try each possible slot for 'lo' and see if all others also land on valid slots
                for (int s = 0; s <= 22; s++) {
                    int candidate = lo - s * slotStride;
                    if (candidate < 0) break;
                    bool ok = true;
                    for (uint16_t p : matchingPairs) {
                        int off = p - candidate;
                        if (off < 0 || off % slotStride != 0 || off / slotStride > 22) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        owner_.invSlotBase_ = candidate;
                        break;
                    }
                }
                if (owner_.invSlotBase_ < 0) owner_.invSlotBase_ = knownBase;
            } else {
                owner_.invSlotBase_ = knownBase;
            }
        }
        owner_.packSlotBase_ = owner_.invSlotBase_ + (game::Inventory::NUM_EQUIP_SLOTS * 2);
        LOG_INFO("Detected inventory field base: equip=", owner_.invSlotBase_,
                 " pack=", owner_.packSlotBase_);
    }
}

bool InventoryHandler::applyInventoryFields(const std::map<uint16_t, uint32_t>& fields) {
    bool slotsChanged = false;
    int equipBase = (owner_.invSlotBase_ >= 0) ? owner_.invSlotBase_ : static_cast<int>(fieldIndex(UF::PLAYER_FIELD_INV_SLOT_HEAD));
    int packBase = (owner_.packSlotBase_ >= 0) ? owner_.packSlotBase_ : static_cast<int>(fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1));
    int bankBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_BANK_SLOT_1));
    int bankBagBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_BANKBAG_SLOT_1));

    // Derive slot counts from field gap (Classic=24/6, TBC/WotLK=28/7).
    if (bankBase != 0xFFFF && bankBagBase != 0xFFFF) {
        effectiveBankSlots_ = std::min((bankBagBase - bankBase) / 2, 28);
        effectiveBankBagSlots_ = (effectiveBankSlots_ <= 24) ? 6 : 7;
    }

    int keyringBase = static_cast<int>(fieldIndex(UF::PLAYER_FIELD_KEYRING_SLOT_1));
    if (keyringBase == 0xFFFF && bankBagBase != 0xFFFF) {
        // Layout fallback for profiles that don't define PLAYER_FIELD_KEYRING_SLOT_1.
        // Bank bag slots are followed by 12 vendor buyback slots (24 fields), then keyring.
        keyringBase = bankBagBase + (effectiveBankBagSlots_ * 2) + 24;
    }

    for (const auto& [key, val] : fields) {
        if (key >= equipBase && key <= equipBase + (game::Inventory::NUM_EQUIP_SLOTS * 2 - 1)) {
            int slotIndex = (key - equipBase) / 2;
            bool isLow = ((key - equipBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.equipSlotGuids_.size())) {
                uint64_t& guid = owner_.equipSlotGuids_[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        } else if (key >= packBase && key <= packBase + (game::Inventory::BACKPACK_SLOTS * 2 - 1)) {
            int slotIndex = (key - packBase) / 2;
            bool isLow = ((key - packBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.backpackSlotGuids_.size())) {
                uint64_t& guid = owner_.backpackSlotGuids_[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        } else if (keyringBase != 0xFFFF &&
                   key >= keyringBase &&
                   key <= keyringBase + (game::Inventory::KEYRING_SLOTS * 2 - 1)) {
            int slotIndex = (key - keyringBase) / 2;
            bool isLow = ((key - keyringBase) % 2 == 0);
            if (slotIndex < static_cast<int>(owner_.keyringSlotGuids_.size())) {
                uint64_t& guid = owner_.keyringSlotGuids_[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        }
        if (bankBase != 0xFFFF && key >= static_cast<uint16_t>(bankBase) &&
            key <= static_cast<uint16_t>(bankBase) + (effectiveBankSlots_ * 2 - 1)) {
            int slotIndex = (key - bankBase) / 2;
            bool isLow = ((key - bankBase) % 2 == 0);
            if (slotIndex < static_cast<int>(bankSlotGuids_.size())) {
                uint64_t& guid = bankSlotGuids_[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        }

        // Bank bag slots starting at PLAYER_FIELD_BANKBAG_SLOT_1
        if (bankBagBase != 0xFFFF && key >= static_cast<uint16_t>(bankBagBase) &&
            key <= static_cast<uint16_t>(bankBagBase) + (effectiveBankBagSlots_ * 2 - 1)) {
            int slotIndex = (key - bankBagBase) / 2;
            bool isLow = ((key - bankBagBase) % 2 == 0);
            if (slotIndex < static_cast<int>(bankBagSlotGuids_.size())) {
                uint64_t& guid = bankBagSlotGuids_[slotIndex];
                if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
                else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
                slotsChanged = true;
            }
        }
    }

    return slotsChanged;
}

void InventoryHandler::extractContainerFields(uint64_t containerGuid, const std::map<uint16_t, uint32_t>& fields) {
    const uint16_t numSlotsIdx = fieldIndex(UF::CONTAINER_FIELD_NUM_SLOTS);
    const uint16_t slot1Idx = fieldIndex(UF::CONTAINER_FIELD_SLOT_1);
    if (numSlotsIdx == 0xFFFF || slot1Idx == 0xFFFF) return;

    auto& info = owner_.containerContents_[containerGuid];

    // Read number of slots
    auto numIt = fields.find(numSlotsIdx);
    if (numIt != fields.end()) {
        info.numSlots = std::min(numIt->second, 36u);
    }

    // Read slot GUIDs (each is 2 uint32 fields: lo + hi)
    for (const auto& [key, val] : fields) {
        if (key < slot1Idx) continue;
        int offset = key - slot1Idx;
        int slotIndex = offset / 2;
        if (slotIndex >= 36) continue;
        bool isLow = (offset % 2 == 0);
        uint64_t& guid = info.slotGuids[slotIndex];
        if (isLow) guid = (guid & 0xFFFFFFFF00000000ULL) | val;
        else guid = (guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32);
    }
}

void InventoryHandler::rebuildOnlineInventory() {

    uint8_t savedBankBagSlots = owner_.inventory.getPurchasedBankBagSlots();
    owner_.inventory = Inventory();
    owner_.inventory.setPurchasedBankBagSlots(savedBankBagSlots);

    // Equipment slots
    for (int i = 0; i < 23; i++) {
        uint64_t guid = owner_.equipSlotGuids_[i];
        if (guid == 0) continue;

        auto itemIt = owner_.onlineItems_.find(guid);
        if (itemIt == owner_.onlineItems_.end()) continue;

        ItemDef def;
        def.itemId = itemIt->second.entry;
        def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
        def.maxStack = 1;

        auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
        if (infoIt != owner_.itemInfoCache_.end()) {
            def.name = infoIt->second.name;
            def.quality = static_cast<ItemQuality>(infoIt->second.quality);
            def.inventoryType = infoIt->second.inventoryType;
            def.maxStack = std::max(1, infoIt->second.maxStack);
            def.displayInfoId = infoIt->second.displayInfoId;
            def.subclassName = infoIt->second.subclassName;
            def.damageMin = infoIt->second.damageMin;
            def.damageMax = infoIt->second.damageMax;
            def.delayMs = infoIt->second.delayMs;
            def.armor = infoIt->second.armor;
            def.stamina = infoIt->second.stamina;
            def.strength = infoIt->second.strength;
            def.agility = infoIt->second.agility;
            def.intellect = infoIt->second.intellect;
            def.spirit = infoIt->second.spirit;
            def.sellPrice = infoIt->second.sellPrice;
            def.itemLevel = infoIt->second.itemLevel;
            def.requiredLevel = infoIt->second.requiredLevel;
            def.bindType = infoIt->second.bindType;
            def.description = infoIt->second.description;
            def.startQuestId = infoIt->second.startQuestId;
            def.extraStats.clear();
            for (const auto& es : infoIt->second.extraStats)
                def.extraStats.push_back({es.statType, es.statValue});
        } else {
            def.name = "Item " + std::to_string(def.itemId);
            queryItemInfo(def.itemId, guid);
        }

        owner_.inventory.setEquipSlot(static_cast<EquipSlot>(i), def);
    }

    // Backpack slots
    for (int i = 0; i < 16; i++) {
        uint64_t guid = owner_.backpackSlotGuids_[i];
        if (guid == 0) continue;

        auto itemIt = owner_.onlineItems_.find(guid);
        if (itemIt == owner_.onlineItems_.end()) continue;

        ItemDef def;
        def.itemId = itemIt->second.entry;
        def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
        def.maxStack = 1;

        auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
        if (infoIt != owner_.itemInfoCache_.end()) {
            def.name = infoIt->second.name;
            def.quality = static_cast<ItemQuality>(infoIt->second.quality);
            def.inventoryType = infoIt->second.inventoryType;
            def.maxStack = std::max(1, infoIt->second.maxStack);
            def.displayInfoId = infoIt->second.displayInfoId;
            def.subclassName = infoIt->second.subclassName;
            def.damageMin = infoIt->second.damageMin;
            def.damageMax = infoIt->second.damageMax;
            def.delayMs = infoIt->second.delayMs;
            def.armor = infoIt->second.armor;
            def.stamina = infoIt->second.stamina;
            def.strength = infoIt->second.strength;
            def.agility = infoIt->second.agility;
            def.intellect = infoIt->second.intellect;
            def.spirit = infoIt->second.spirit;
            def.sellPrice = infoIt->second.sellPrice;
            def.itemLevel = infoIt->second.itemLevel;
            def.requiredLevel = infoIt->second.requiredLevel;
            def.bindType = infoIt->second.bindType;
            def.description = infoIt->second.description;
            def.startQuestId = infoIt->second.startQuestId;
            def.extraStats.clear();
            for (const auto& es : infoIt->second.extraStats)
                def.extraStats.push_back({es.statType, es.statValue});
        } else {
            def.name = "Item " + std::to_string(def.itemId);
            queryItemInfo(def.itemId, guid);
        }

        owner_.inventory.setBackpackSlot(i, def);
    }

    // Keyring slots
    for (int i = 0; i < game::Inventory::KEYRING_SLOTS; i++) {
        uint64_t guid = owner_.keyringSlotGuids_[i];
        if (guid == 0) continue;

        auto itemIt = owner_.onlineItems_.find(guid);
        if (itemIt == owner_.onlineItems_.end()) continue;

        ItemDef def;
        def.itemId = itemIt->second.entry;
        def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
        def.maxStack = 1;

        auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
        if (infoIt != owner_.itemInfoCache_.end()) {
            def.name = infoIt->second.name;
            def.quality = static_cast<ItemQuality>(infoIt->second.quality);
            def.inventoryType = infoIt->second.inventoryType;
            def.maxStack = std::max(1, infoIt->second.maxStack);
            def.displayInfoId = infoIt->second.displayInfoId;
            def.subclassName = infoIt->second.subclassName;
            def.damageMin = infoIt->second.damageMin;
            def.damageMax = infoIt->second.damageMax;
            def.delayMs = infoIt->second.delayMs;
            def.armor = infoIt->second.armor;
            def.stamina = infoIt->second.stamina;
            def.strength = infoIt->second.strength;
            def.agility = infoIt->second.agility;
            def.intellect = infoIt->second.intellect;
            def.spirit = infoIt->second.spirit;
            def.sellPrice = infoIt->second.sellPrice;
            def.itemLevel = infoIt->second.itemLevel;
            def.requiredLevel = infoIt->second.requiredLevel;
            def.bindType = infoIt->second.bindType;
            def.description = infoIt->second.description;
            def.startQuestId = infoIt->second.startQuestId;
            def.extraStats.clear();
            for (const auto& es : infoIt->second.extraStats)
                def.extraStats.push_back({es.statType, es.statValue});
        } else {
            def.name = "Item " + std::to_string(def.itemId);
            queryItemInfo(def.itemId, guid);
        }

        owner_.inventory.setKeyringSlot(i, def);
    }

    // Bag contents (BAG1-BAG4 are equip slots 19-22)
    for (int bagIdx = 0; bagIdx < 4; bagIdx++) {
        uint64_t bagGuid = owner_.equipSlotGuids_[19 + bagIdx];
        if (bagGuid == 0) continue;

        // Determine bag size from container fields or item template
        int numSlots = 0;
        auto contIt = owner_.containerContents_.find(bagGuid);
        if (contIt != owner_.containerContents_.end()) {
            numSlots = static_cast<int>(contIt->second.numSlots);
        }
        if (numSlots <= 0) {
            auto bagItemIt = owner_.onlineItems_.find(bagGuid);
            if (bagItemIt != owner_.onlineItems_.end()) {
                auto bagInfoIt = owner_.itemInfoCache_.find(bagItemIt->second.entry);
                if (bagInfoIt != owner_.itemInfoCache_.end()) {
                    numSlots = bagInfoIt->second.containerSlots;
                }
            }
        }
        if (numSlots <= 0) continue;

        // Set the bag size in the inventory bag data
        owner_.inventory.setBagSize(bagIdx, numSlots);

        // Also set bagSlots on the equipped bag item (for UI display)
        auto& bagEquipSlot = owner_.inventory.getEquipSlot(static_cast<EquipSlot>(19 + bagIdx));
        if (!bagEquipSlot.empty()) {
            ItemDef bagDef = bagEquipSlot.item;
            bagDef.bagSlots = numSlots;
            owner_.inventory.setEquipSlot(static_cast<EquipSlot>(19 + bagIdx), bagDef);
        }

        // Populate bag slot items
        if (contIt == owner_.containerContents_.end()) continue;
        const auto& container = contIt->second;
        for (int s = 0; s < numSlots && s < 36; s++) {
            uint64_t itemGuid = container.slotGuids[s];
            if (itemGuid == 0) continue;

            auto itemIt = owner_.onlineItems_.find(itemGuid);
            if (itemIt == owner_.onlineItems_.end()) continue;

            ItemDef def;
            def.itemId = itemIt->second.entry;
            def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
            def.maxStack = 1;

            auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
            if (infoIt != owner_.itemInfoCache_.end()) {
                def.name = infoIt->second.name;
                def.quality = static_cast<ItemQuality>(infoIt->second.quality);
                def.inventoryType = infoIt->second.inventoryType;
                def.maxStack = std::max(1, infoIt->second.maxStack);
                def.displayInfoId = infoIt->second.displayInfoId;
                def.subclassName = infoIt->second.subclassName;
                def.damageMin = infoIt->second.damageMin;
                def.damageMax = infoIt->second.damageMax;
                def.delayMs = infoIt->second.delayMs;
                def.armor = infoIt->second.armor;
                def.stamina = infoIt->second.stamina;
                def.strength = infoIt->second.strength;
                def.agility = infoIt->second.agility;
                def.intellect = infoIt->second.intellect;
                def.spirit = infoIt->second.spirit;
                def.sellPrice = infoIt->second.sellPrice;
                def.itemLevel = infoIt->second.itemLevel;
                def.requiredLevel = infoIt->second.requiredLevel;
                def.bindType = infoIt->second.bindType;
                def.description = infoIt->second.description;
                def.startQuestId = infoIt->second.startQuestId;
                def.extraStats.clear();
                for (const auto& es : infoIt->second.extraStats)
                    def.extraStats.push_back({es.statType, es.statValue});
                def.bagSlots = infoIt->second.containerSlots;
            } else {
                def.name = "Item " + std::to_string(def.itemId);
                queryItemInfo(def.itemId, itemGuid);
            }

            owner_.inventory.setBagSlot(bagIdx, s, def);
        }
    }

    // Bank slots (24 for Classic, 28 for TBC/WotLK)
    for (int i = 0; i < effectiveBankSlots_; i++) {
        uint64_t guid = bankSlotGuids_[i];
        if (guid == 0) { owner_.inventory.clearBankSlot(i); continue; }

        auto itemIt = owner_.onlineItems_.find(guid);
        if (itemIt == owner_.onlineItems_.end()) { owner_.inventory.clearBankSlot(i); continue; }

        ItemDef def;
        def.itemId = itemIt->second.entry;
        def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
        def.maxStack = 1;

        auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
        if (infoIt != owner_.itemInfoCache_.end()) {
            def.name = infoIt->second.name;
            def.quality = static_cast<ItemQuality>(infoIt->second.quality);
            def.inventoryType = infoIt->second.inventoryType;
            def.maxStack = std::max(1, infoIt->second.maxStack);
            def.displayInfoId = infoIt->second.displayInfoId;
            def.subclassName = infoIt->second.subclassName;
            def.damageMin = infoIt->second.damageMin;
            def.damageMax = infoIt->second.damageMax;
            def.delayMs = infoIt->second.delayMs;
            def.armor = infoIt->second.armor;
            def.stamina = infoIt->second.stamina;
            def.strength = infoIt->second.strength;
            def.agility = infoIt->second.agility;
            def.intellect = infoIt->second.intellect;
            def.spirit = infoIt->second.spirit;
            def.itemLevel = infoIt->second.itemLevel;
            def.requiredLevel = infoIt->second.requiredLevel;
            def.bindType = infoIt->second.bindType;
            def.description = infoIt->second.description;
            def.startQuestId = infoIt->second.startQuestId;
            def.extraStats.clear();
            for (const auto& es : infoIt->second.extraStats)
                def.extraStats.push_back({es.statType, es.statValue});
            def.sellPrice = infoIt->second.sellPrice;
            def.bagSlots = infoIt->second.containerSlots;
        } else {
            def.name = "Item " + std::to_string(def.itemId);
            queryItemInfo(def.itemId, guid);
        }

        owner_.inventory.setBankSlot(i, def);
    }

    // Bank bag contents (6 for Classic, 7 for TBC/WotLK)
    for (int bagIdx = 0; bagIdx < effectiveBankBagSlots_; bagIdx++) {
        uint64_t bagGuid = bankBagSlotGuids_[bagIdx];
        if (bagGuid == 0) { owner_.inventory.setBankBagSize(bagIdx, 0); continue; }

        int numSlots = 0;
        auto contIt = owner_.containerContents_.find(bagGuid);
        if (contIt != owner_.containerContents_.end()) {
            numSlots = static_cast<int>(contIt->second.numSlots);
        }

        // Populate the bag item itself (for icon/name in the bank bag equip slot)
        auto bagItemIt = owner_.onlineItems_.find(bagGuid);
        if (bagItemIt != owner_.onlineItems_.end()) {
            if (numSlots <= 0) {
                auto bagInfoIt = owner_.itemInfoCache_.find(bagItemIt->second.entry);
                if (bagInfoIt != owner_.itemInfoCache_.end()) {
                    numSlots = bagInfoIt->second.containerSlots;
                }
            }
            ItemDef bagDef;
            bagDef.itemId = bagItemIt->second.entry;
            bagDef.stackCount = 1;
            bagDef.inventoryType = 18; // bag
            auto bagInfoIt = owner_.itemInfoCache_.find(bagItemIt->second.entry);
            if (bagInfoIt != owner_.itemInfoCache_.end()) {
                bagDef.name = bagInfoIt->second.name;
                bagDef.quality = static_cast<ItemQuality>(bagInfoIt->second.quality);
                bagDef.displayInfoId = bagInfoIt->second.displayInfoId;
                bagDef.bagSlots = bagInfoIt->second.containerSlots;
            } else {
                bagDef.name = "Bag";
                queryItemInfo(bagDef.itemId, bagGuid);
            }
            owner_.inventory.setBankBagItem(bagIdx, bagDef);
        }
        if (numSlots <= 0) continue;

        owner_.inventory.setBankBagSize(bagIdx, numSlots);

        if (contIt == owner_.containerContents_.end()) continue;
        const auto& container = contIt->second;
        for (int s = 0; s < numSlots && s < 36; s++) {
            uint64_t itemGuid = container.slotGuids[s];
            if (itemGuid == 0) continue;

            auto itemIt = owner_.onlineItems_.find(itemGuid);
            if (itemIt == owner_.onlineItems_.end()) continue;

            ItemDef def;
            def.itemId = itemIt->second.entry;
            def.stackCount = itemIt->second.stackCount;
        def.curDurability = itemIt->second.curDurability;
        def.maxDurability = itemIt->second.maxDurability;
            def.maxStack = 1;

            auto infoIt = owner_.itemInfoCache_.find(itemIt->second.entry);
            if (infoIt != owner_.itemInfoCache_.end()) {
                def.name = infoIt->second.name;
                def.quality = static_cast<ItemQuality>(infoIt->second.quality);
                def.inventoryType = infoIt->second.inventoryType;
                def.maxStack = std::max(1, infoIt->second.maxStack);
                def.displayInfoId = infoIt->second.displayInfoId;
                def.subclassName = infoIt->second.subclassName;
                def.damageMin = infoIt->second.damageMin;
                def.damageMax = infoIt->second.damageMax;
                def.delayMs = infoIt->second.delayMs;
                def.armor = infoIt->second.armor;
                def.stamina = infoIt->second.stamina;
                def.strength = infoIt->second.strength;
                def.agility = infoIt->second.agility;
                def.intellect = infoIt->second.intellect;
                def.spirit = infoIt->second.spirit;
                def.itemLevel = infoIt->second.itemLevel;
                def.requiredLevel = infoIt->second.requiredLevel;
                def.sellPrice = infoIt->second.sellPrice;
                def.bindType = infoIt->second.bindType;
                def.description = infoIt->second.description;
                def.startQuestId = infoIt->second.startQuestId;
                def.extraStats.clear();
                for (const auto& es : infoIt->second.extraStats)
                    def.extraStats.push_back({es.statType, es.statValue});
                def.bagSlots = infoIt->second.containerSlots;
            } else {
                def.name = "Item " + std::to_string(def.itemId);
                queryItemInfo(def.itemId, itemGuid);
            }

            owner_.inventory.setBankBagSlot(bagIdx, s, def);
        }
    }

    // Only mark equipment dirty if equipped item displayInfoIds actually changed
    std::array<uint32_t, 19> currentEquipDisplayIds{};
    for (int i = 0; i < 19; i++) {
        const auto& slot = owner_.inventory.getEquipSlot(static_cast<EquipSlot>(i));
        if (!slot.empty()) currentEquipDisplayIds[i] = slot.item.displayInfoId;
    }
    if (currentEquipDisplayIds != owner_.lastEquipDisplayIds_) {
        owner_.lastEquipDisplayIds_ = currentEquipDisplayIds;
        owner_.onlineEquipDirty_ = true;
    }

    LOG_DEBUG("Rebuilt online inventory: equip=", [&](){
        int c = 0; for (auto g : owner_.equipSlotGuids_) if (g) c++; return c;
    }(), " backpack=", [&](){
        int c = 0; for (auto g : owner_.backpackSlotGuids_) if (g) c++; return c;
    }(), " keyring=", [&](){
        int c = 0; for (auto g : owner_.keyringSlotGuids_) if (g) c++; return c;
    }());
}

void InventoryHandler::maybeDetectVisibleItemLayout() {
    if (owner_.visibleItemLayoutVerified_) return;
    if (owner_.lastPlayerFields_.empty()) return;

    std::array<uint32_t, 19> equipEntries{};
    int nonZero = 0;
    // Prefer authoritative equipped item entry IDs derived from item objects (onlineItems_),
    // because Inventory::ItemDef may not be populated yet if templates haven't been queried.
    for (int i = 0; i < 19; i++) {
        uint64_t itemGuid = owner_.equipSlotGuids_[i];
        if (itemGuid != 0) {
            auto it = owner_.onlineItems_.find(itemGuid);
            if (it != owner_.onlineItems_.end() && it->second.entry != 0) {
                equipEntries[i] = it->second.entry;
            }
        }
        if (equipEntries[i] == 0) {
            const auto& slot = owner_.inventory.getEquipSlot(static_cast<EquipSlot>(i));
            equipEntries[i] = slot.empty() ? 0u : slot.item.itemId;
        }
        if (equipEntries[i] != 0) nonZero++;
    }
    if (nonZero < 2) return;

    const uint16_t maxKey = owner_.lastPlayerFields_.rbegin()->first;
    int bestBase = -1;
    int bestStride = 0;
    int bestMatches = 0;
    int bestMismatches = 9999;
    int bestScore = -999999;

    const int strides[] = {2, 3, 4, 1};
    for (int stride : strides) {
        for (const auto& [baseIdxU16, _v] : owner_.lastPlayerFields_) {
            const int base = static_cast<int>(baseIdxU16);
            if (base + 18 * stride > static_cast<int>(maxKey)) continue;

            int matches = 0;
            int mismatches = 0;
            for (int s = 0; s < 19; s++) {
                uint32_t want = equipEntries[s];
                if (want == 0) continue;
                const uint16_t idx = static_cast<uint16_t>(base + s * stride);
                auto it = owner_.lastPlayerFields_.find(idx);
                if (it == owner_.lastPlayerFields_.end()) continue;
                if (it->second == want) {
                    matches++;
                } else if (it->second != 0) {
                    mismatches++;
                }
            }

            int score = matches * 2 - mismatches * 3;
            if (score > bestScore ||
                (score == bestScore && matches > bestMatches) ||
                (score == bestScore && matches == bestMatches && mismatches < bestMismatches) ||
                (score == bestScore && matches == bestMatches && mismatches == bestMismatches && base < bestBase)) {
                bestScore = score;
                bestMatches = matches;
                bestMismatches = mismatches;
                bestBase = base;
                bestStride = stride;
            }
        }
    }

    if (bestMatches >= 2 && bestBase >= 0 && bestStride > 0 && bestMismatches <= 1) {
        owner_.visibleItemEntryBase_ = bestBase;
        owner_.visibleItemStride_ = bestStride;
        owner_.visibleItemLayoutVerified_ = true;
        LOG_INFO("Detected PLAYER_VISIBLE_ITEM entry layout: base=", owner_.visibleItemEntryBase_,
                 " stride=", owner_.visibleItemStride_, " (matches=", bestMatches,
                 " mismatches=", bestMismatches, " score=", bestScore, ")");

        // Backfill existing player entities already in view.
        for (const auto& [guid, ent] : owner_.entityManager.getEntities()) {
            if (!ent || ent->getType() != ObjectType::PLAYER) continue;
            if (guid == owner_.playerGuid) continue;
            updateOtherPlayerVisibleItems(guid, ent->getFields());
        }
    }
    // If heuristic didn't find a match, keep using the default WotLK layout (base=284, stride=2).
}

void InventoryHandler::updateOtherPlayerVisibleItems(uint64_t guid, const std::map<uint16_t, uint32_t>& fields) {
    if (guid == 0 || guid == owner_.playerGuid) return;

    // Use the current base/stride (defaults are correct for WotLK 3.3.5a: base=284, stride=2).
    // The heuristic may refine these later, but we proceed immediately with whatever values
    // are set rather than waiting for verification.
    const int base = owner_.visibleItemEntryBase_;
    const int stride = owner_.visibleItemStride_;
    if (base < 0 || stride <= 0) return; // Defensive: should never happen with defaults.

    std::array<uint32_t, 19> newEntries{};
    for (int s = 0; s < 19; s++) {
        uint16_t idx = static_cast<uint16_t>(base + s * stride);
        auto it = fields.find(idx);
        if (it != fields.end()) newEntries[s] = it->second;
    }

    int nonZero = 0;
    for (uint32_t e : newEntries) { if (e != 0) nonZero++; }
    if (nonZero > 0) {
        LOG_INFO("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                 " nonZero=", nonZero, " base=", base, " stride=", stride,
                 " head=", newEntries[0], " shoulders=", newEntries[2],
                 " chest=", newEntries[4], " legs=", newEntries[6],
                 " mainhand=", newEntries[15], " offhand=", newEntries[16]);
    }

    bool changed = false;
    auto& old = owner_.otherPlayerVisibleItemEntries_[guid];
    if (old != newEntries) {
        old = newEntries;
        changed = true;
    }

    // Request item templates for any new visible entries.
    for (uint32_t entry : newEntries) {
        if (entry == 0) continue;
        if (!owner_.itemInfoCache_.count(entry) && !owner_.pendingItemQueries_.count(entry)) {
            queryItemInfo(entry, 0);
        }
    }

    // Only fall back to auto-inspect if ALL extracted entries are zero (server didn't
    // send visible item fields at all). If we got at least one non-zero entry, the
    // update-field approach is working and inspect is unnecessary.
    if (nonZero == 0) {
        LOG_DEBUG("updateOtherPlayerVisibleItems: guid=0x", std::hex, guid, std::dec,
                  " all entries zero (base=", base, " stride=", stride,
                  " fieldCount=", fields.size(), ") — queuing auto-inspect");
        if (owner_.socket && owner_.state == WorldState::IN_WORLD) {
            owner_.pendingAutoInspect_.insert(guid);
        }
    }

    if (changed) {
        owner_.otherPlayerVisibleDirty_.insert(guid);
        emitOtherPlayerEquipment(guid);
    }
}

void InventoryHandler::emitOtherPlayerEquipment(uint64_t guid) {
    if (!owner_.playerEquipmentCallback_) return;
    auto it = owner_.otherPlayerVisibleItemEntries_.find(guid);
    if (it == owner_.otherPlayerVisibleItemEntries_.end()) return;

    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> invTypes{};
    bool anyEntry = false;
    int resolved = 0, unresolved = 0;

    for (int s = 0; s < 19; s++) {
        uint32_t entry = it->second[s];
        if (entry == 0) continue;
        anyEntry = true;
        auto infoIt = owner_.itemInfoCache_.find(entry);
        if (infoIt == owner_.itemInfoCache_.end()) { unresolved++; continue; }
        displayIds[s] = infoIt->second.displayInfoId;
        invTypes[s] = static_cast<uint8_t>(infoIt->second.inventoryType);
        resolved++;
    }

    LOG_INFO("emitOtherPlayerEquipment: guid=0x", std::hex, guid, std::dec,
             " entries=", (anyEntry ? "yes" : "none"),
             " resolved=", resolved, " unresolved=", unresolved,
             " head=", displayIds[0], " shoulders=", displayIds[2],
             " chest=", displayIds[4], " legs=", displayIds[6],
             " mainhand=", displayIds[15], " offhand=", displayIds[16]);

    owner_.playerEquipmentCallback_(guid, displayIds, invTypes);
    owner_.otherPlayerVisibleDirty_.erase(guid);

    // If we had entries but couldn't resolve any templates, also try inspect as a fallback.
    if (anyEntry && !resolved) {
        owner_.pendingAutoInspect_.insert(guid);
    }
}

void InventoryHandler::emitAllOtherPlayerEquipment() {
    if (!owner_.playerEquipmentCallback_) return;
    for (const auto& [guid, _] : owner_.otherPlayerVisibleItemEntries_) {
        emitOtherPlayerEquipment(guid);
    }
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void InventoryHandler::handleTrainerBuySucceeded(network::Packet& packet) {
    /*uint64_t guid =*/ packet.readUInt64();
    uint32_t spellId = packet.readUInt32();
    if (owner_.spellHandler_ && !owner_.spellHandler_->knownSpells_.count(spellId)) {
        owner_.spellHandler_->knownSpells_.insert(spellId);
    }
    const std::string& name = owner_.getSpellName(spellId);
    if (!name.empty())
        owner_.addSystemChatMessage("You have learned " + name + ".");
    else
        owner_.addSystemChatMessage("Spell learned.");
    if (auto* renderer = core::Application::getInstance().getRenderer())
        if (auto* sfx = renderer->getUiSoundManager()) sfx->playQuestActivate();
    owner_.fireAddonEvent("TRAINER_UPDATE", {});
    owner_.fireAddonEvent("SPELLS_CHANGED", {});
}

void InventoryHandler::handleTrainerBuyFailed(network::Packet& packet) {
    /*uint64_t trainerGuid =*/ packet.readUInt64();
    uint32_t spellId = packet.readUInt32();
    uint32_t errorCode = 0;
    if (packet.hasRemaining(4))
        errorCode = packet.readUInt32();
    const std::string& spellName = owner_.getSpellName(spellId);
    std::string msg = "Cannot learn ";
    if (!spellName.empty()) msg += spellName;
    else msg += "spell #" + std::to_string(spellId);
    if (errorCode == 0) msg += " (not enough money)";
    else if (errorCode == 1) msg += " (not enough skill)";
    else if (errorCode == 2) msg += " (already known)";
    else if (errorCode != 0) msg += " (error " + std::to_string(errorCode) + ")";
    owner_.addUIError(msg);
    owner_.addSystemChatMessage(msg);
    if (auto* renderer = core::Application::getInstance().getRenderer())
        if (auto* sfx = renderer->getUiSoundManager()) sfx->playError();
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void InventoryHandler::initiateTrade(uint64_t targetGuid) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot initiate trade: not in world or not connected");
        return;
    }

    if (targetGuid == 0) {
        owner_.addSystemChatMessage("You must target a player to trade with.");
        return;
    }

    auto packet = InitiateTradePacket::build(targetGuid);
    owner_.socket->send(packet);
    owner_.addSystemChatMessage("Requesting trade with target.");
    LOG_INFO("Initiated trade with target: 0x", std::hex, targetGuid, std::dec);
}

uint32_t InventoryHandler::getTempEnchantRemainingMs(uint32_t slot) const {
    uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (const auto& t : owner_.tempEnchantTimers_) {
        if (t.slot == slot) {
            return (t.expireMs > nowMs)
                ? static_cast<uint32_t>(t.expireMs - nowMs) : 0u;
        }
    }
    return 0u;
}

void InventoryHandler::addMoneyCopper(uint32_t amount) {
    if (amount == 0) return;
    owner_.playerMoneyCopper_ += amount;
    uint32_t gold = amount / 10000;
    uint32_t silver = (amount / 100) % 100;
    uint32_t copper = amount % 100;
    std::string msg = "You receive ";
    msg += std::to_string(gold) + "g ";
    msg += std::to_string(silver) + "s ";
    msg += std::to_string(copper) + "c.";
    owner_.addSystemChatMessage(msg);
    owner_.fireAddonEvent("CHAT_MSG_MONEY", {msg});
}

} // namespace game
} // namespace wowee
