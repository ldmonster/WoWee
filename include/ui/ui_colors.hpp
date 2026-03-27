#pragma once

#include <imgui.h>
#include "game/inventory.hpp"

namespace wowee::ui {

// ---- Common UI colors ----
namespace colors {
    constexpr ImVec4 kRed         = {1.0f, 0.3f, 0.3f, 1.0f};
    constexpr ImVec4 kGreen       = {0.4f, 1.0f, 0.4f, 1.0f};
    constexpr ImVec4 kBrightGreen = {0.3f, 1.0f, 0.3f, 1.0f};
    constexpr ImVec4 kYellow      = {1.0f, 1.0f, 0.3f, 1.0f};
    constexpr ImVec4 kGray        = {0.6f, 0.6f, 0.6f, 1.0f};
    constexpr ImVec4 kDarkGray    = {0.5f, 0.5f, 0.5f, 1.0f};
    constexpr ImVec4 kLightGray   = {0.7f, 0.7f, 0.7f, 1.0f};
    constexpr ImVec4 kWhite       = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr ImVec4 kTooltipGold = {1.0f, 0.82f, 0.0f, 1.0f};
    constexpr ImVec4 kBrightGold  = {1.0f, 0.85f, 0.0f, 1.0f};
    constexpr ImVec4 kPaleRed     = {1.0f, 0.5f, 0.5f, 1.0f};
    constexpr ImVec4 kBrightRed   = {1.0f, 0.2f, 0.2f, 1.0f};
    constexpr ImVec4 kLightBlue   = {0.4f, 0.6f, 1.0f, 1.0f};
    constexpr ImVec4 kManaBlue    = {0.2f, 0.2f, 0.9f, 1.0f};
    constexpr ImVec4 kCyan        = {0.0f, 0.8f, 1.0f, 1.0f};
    constexpr ImVec4 kDarkRed     = {0.9f, 0.2f, 0.2f, 1.0f};
    constexpr ImVec4 kSoftRed     = {1.0f, 0.4f, 0.4f, 1.0f};
    constexpr ImVec4 kHostileRed  = {1.0f, 0.35f, 0.35f, 1.0f};
    constexpr ImVec4 kMediumGray  = {0.65f, 0.65f, 0.65f, 1.0f};
    constexpr ImVec4 kWarmGold    = {1.0f, 0.84f, 0.0f, 1.0f};
    constexpr ImVec4 kOrange      = {0.9f, 0.6f, 0.1f, 1.0f};
    constexpr ImVec4 kFriendlyGreen = {0.2f, 0.7f, 0.2f, 1.0f};
    constexpr ImVec4 kHealthGreen   = {0.2f, 0.8f, 0.2f, 1.0f};
    constexpr ImVec4 kLightGreen    = {0.6f, 1.0f, 0.6f, 1.0f};
    constexpr ImVec4 kActiveGreen   = {0.5f, 1.0f, 0.5f, 1.0f};
    constexpr ImVec4 kSocketGreen   = {0.5f, 0.8f, 0.5f, 1.0f};

    // UI element colors
    constexpr ImVec4 kInactiveGray   = {0.55f, 0.55f, 0.55f, 1.0f};
    constexpr ImVec4 kVeryLightGray  = {0.85f, 0.85f, 0.85f, 1.0f};
    constexpr ImVec4 kSymbolGold     = {1.0f, 0.85f, 0.1f, 1.0f};
    constexpr ImVec4 kLowHealthRed   = {0.8f, 0.2f, 0.2f, 1.0f};
    constexpr ImVec4 kDangerRed      = {0.7f, 0.2f, 0.2f, 1.0f};

    // Power-type colors (unit resource bars)
    constexpr ImVec4 kEnergyYellow    = {0.9f, 0.9f, 0.2f, 1.0f};
    constexpr ImVec4 kHappinessGreen  = {0.5f, 0.9f, 0.3f, 1.0f};
    constexpr ImVec4 kRunicRed        = {0.8f, 0.1f, 0.2f, 1.0f};
    constexpr ImVec4 kSoulShardPurple = {0.4f, 0.1f, 0.6f, 1.0f};

    // Coin colors
    constexpr ImVec4 kGold   = {1.00f, 0.82f, 0.00f, 1.0f};
    constexpr ImVec4 kSilver = {0.80f, 0.80f, 0.80f, 1.0f};
    constexpr ImVec4 kCopper = {0.72f, 0.45f, 0.20f, 1.0f};
} // namespace colors

// ---- Item quality colors ----
inline ImVec4 getQualityColor(game::ItemQuality quality) {
    switch (quality) {
        case game::ItemQuality::POOR:      return {0.62f, 0.62f, 0.62f, 1.0f};
        case game::ItemQuality::COMMON:    return {1.0f, 1.0f, 1.0f, 1.0f};
        case game::ItemQuality::UNCOMMON:  return {0.12f, 1.0f, 0.0f, 1.0f};
        case game::ItemQuality::RARE:      return {0.0f, 0.44f, 0.87f, 1.0f};
        case game::ItemQuality::EPIC:      return {0.64f, 0.21f, 0.93f, 1.0f};
        case game::ItemQuality::LEGENDARY: return {1.0f, 0.50f, 0.0f, 1.0f};
        case game::ItemQuality::ARTIFACT:  return {0.90f, 0.80f, 0.50f, 1.0f};
        case game::ItemQuality::HEIRLOOM:  return {0.90f, 0.80f, 0.50f, 1.0f};
        default:                           return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}

// ---- Coin display (gold/silver/copper) ----
inline void renderCoinsText(uint32_t g, uint32_t s, uint32_t c) {
    bool any = false;
    if (g > 0) {
        ImGui::TextColored(colors::kGold, "%ug", g);
        any = true;
    }
    if (s > 0 || g > 0) {
        if (any) ImGui::SameLine(0, 3);
        ImGui::TextColored(colors::kSilver, "%us", s);
        any = true;
    }
    if (any) ImGui::SameLine(0, 3);
    ImGui::TextColored(colors::kCopper, "%uc", c);
}

// Convenience overload: decompose copper amount and render as gold/silver/copper
inline void renderCoinsFromCopper(uint64_t copper) {
    renderCoinsText(static_cast<uint32_t>(copper / 10000),
                    static_cast<uint32_t>((copper / 100) % 100),
                    static_cast<uint32_t>(copper % 100));
}

// ---- Inventory slot name from WoW inventory type ----
inline const char* getInventorySlotName(uint32_t inventoryType) {
    switch (inventoryType) {
        case 1:  return "Head";
        case 2:  return "Neck";
        case 3:  return "Shoulder";
        case 4:  return "Shirt";
        case 5:  return "Chest";
        case 6:  return "Waist";
        case 7:  return "Legs";
        case 8:  return "Feet";
        case 9:  return "Wrist";
        case 10: return "Hands";
        case 11: return "Finger";
        case 12: return "Trinket";
        case 13: return "One-Hand";
        case 14: return "Shield";
        case 15: return "Ranged";
        case 16: return "Back";
        case 17: return "Two-Hand";
        case 18: return "Bag";
        case 19: return "Tabard";
        case 20: return "Robe";
        case 21: return "Main Hand";
        case 22: return "Off Hand";
        case 23: return "Held In Off-hand";
        case 25: return "Thrown";
        case 26: return "Ranged";
        case 28: return "Relic";
        default: return "";
    }
}

// ---- Binding type display ----
inline void renderBindingType(uint32_t bindType) {
    const auto& kBindColor = colors::kTooltipGold;
    switch (bindType) {
        case 1: ImGui::TextColored(kBindColor, "Binds when picked up"); break;
        case 2: ImGui::TextColored(kBindColor, "Binds when equipped"); break;
        case 3: ImGui::TextColored(kBindColor, "Binds when used"); break;
        case 4: ImGui::TextColored(kBindColor, "Quest Item"); break;
        default: break;
    }
}

// ---- WoW class colors (Blizzard canonical) ----
inline ImVec4 getClassColor(uint8_t classId) {
    switch (classId) {
        case 1:  return {0.78f, 0.61f, 0.43f, 1.0f}; // Warrior  #C79C6E
        case 2:  return {0.96f, 0.55f, 0.73f, 1.0f}; // Paladin  #F58CBA
        case 3:  return {0.67f, 0.83f, 0.45f, 1.0f}; // Hunter   #ABD473
        case 4:  return {1.00f, 0.96f, 0.41f, 1.0f}; // Rogue    #FFF569
        case 5:  return {1.00f, 1.00f, 1.00f, 1.0f}; // Priest   #FFFFFF
        case 6:  return {0.77f, 0.12f, 0.23f, 1.0f}; // DK       #C41F3B
        case 7:  return {0.00f, 0.44f, 0.87f, 1.0f}; // Shaman   #0070DE
        case 8:  return {0.41f, 0.80f, 0.94f, 1.0f}; // Mage     #69CCF0
        case 9:  return {0.58f, 0.51f, 0.79f, 1.0f}; // Warlock  #9482C9
        case 11: return {1.00f, 0.49f, 0.04f, 1.0f}; // Druid    #FF7D0A
        default: return colors::kVeryLightGray;
    }
}

inline ImU32 getClassColorU32(uint8_t classId, int alpha = 255) {
    ImVec4 c = getClassColor(classId);
    return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                    static_cast<int>(c.z * 255), alpha);
}

} // namespace wowee::ui
