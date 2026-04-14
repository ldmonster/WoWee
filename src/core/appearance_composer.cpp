#include "core/appearance_composer.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"

namespace wowee {
namespace core {

AppearanceComposer::AppearanceComposer(rendering::Renderer* renderer,
                                       pipeline::AssetManager* assetManager,
                                       game::GameHandler* gameHandler,
                                       pipeline::DBCLayout* dbcLayout,
                                       EntitySpawner* entitySpawner)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , dbcLayout_(dbcLayout)
    , entitySpawner_(entitySpawner)
{
}

std::string AppearanceComposer::getPlayerModelPath(game::Race race, game::Gender gender) const {
    return game::getPlayerModelPath(race, gender);
}

PlayerTextureInfo AppearanceComposer::resolvePlayerTextures(pipeline::M2Model& model,
                                                            game::Race race, game::Gender gender,
                                                            uint32_t appearanceBytes) {
    PlayerTextureInfo result;

    uint32_t targetRaceId = static_cast<uint32_t>(race);
    uint32_t targetSexId = (gender == game::Gender::FEMALE) ? 1u : 0u;

    // Race name for fallback texture paths
    const char* raceFolderName = "Human";
    switch (race) {
        case game::Race::HUMAN:    raceFolderName = "Human"; break;
        case game::Race::ORC:      raceFolderName = "Orc"; break;
        case game::Race::DWARF:    raceFolderName = "Dwarf"; break;
        case game::Race::NIGHT_ELF: raceFolderName = "NightElf"; break;
        case game::Race::UNDEAD:    raceFolderName = "Scourge"; break;
        case game::Race::TAUREN:    raceFolderName = "Tauren"; break;
        case game::Race::GNOME:     raceFolderName = "Gnome"; break;
        case game::Race::TROLL:     raceFolderName = "Troll"; break;
        case game::Race::BLOOD_ELF: raceFolderName = "BloodElf"; break;
        case game::Race::DRAENEI:   raceFolderName = "Draenei"; break;
        default: break;
    }
    const char* genderFolder = (gender == game::Gender::FEMALE) ? "Female" : "Male";
    std::string raceGender = std::string(raceFolderName) + genderFolder;
    result.bodySkinPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "Skin00_00.blp";
    std::string pelvisPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "NakedPelvisSkin00_00.blp";

    // Extract appearance bytes for texture lookups
    uint8_t charSkinId = appearanceBytes & 0xFF;
    uint8_t charFaceId = (appearanceBytes >> 8) & 0xFF;
    uint8_t charHairStyleId = (appearanceBytes >> 16) & 0xFF;
    uint8_t charHairColorId = (appearanceBytes >> 24) & 0xFF;
    LOG_INFO("Appearance: skin=", static_cast<int>(charSkinId), " face=", static_cast<int>(charFaceId),
             " hairStyle=", static_cast<int>(charHairStyleId), " hairColor=", static_cast<int>(charHairColorId));

    // Parse CharSections.dbc for skin/face/hair/underwear texture paths
    auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
    if (charSectionsDbc) {
        LOG_INFO("CharSections.dbc loaded: ", charSectionsDbc->getRecordCount(), " records");
        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);
        bool foundSkin = false;
        bool foundUnderwear = false;
        bool foundFaceLower = false;
        bool foundHair = false;
        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
            uint32_t raceId = charSectionsDbc->getUInt32(r, csF.raceId);
            uint32_t sexId = charSectionsDbc->getUInt32(r, csF.sexId);
            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

            if (raceId != targetRaceId || sexId != targetSexId) continue;

            // Section 0 = skin: match by colorIndex = skin byte
            if (baseSection == 0 && !foundSkin && colorIndex == charSkinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) {
                    result.bodySkinPath = tex1;
                    foundSkin = true;
                    LOG_INFO("  DBC body skin: ", result.bodySkinPath, " (skin=", static_cast<int>(charSkinId), ")");
                }
            }
            // Section 3 = hair: match variation=hairStyle, color=hairColor
            else if (baseSection == 3 && !foundHair &&
                     variationIndex == charHairStyleId && colorIndex == charHairColorId) {
                result.hairTexturePath = charSectionsDbc->getString(r, csF.texture1);
                if (!result.hairTexturePath.empty()) {
                    foundHair = true;
                    LOG_INFO("  DBC hair texture: ", result.hairTexturePath,
                             " (style=", static_cast<int>(charHairStyleId), " color=", static_cast<int>(charHairColorId), ")");
                }
            }
            // Section 1 = face: match variation=faceId, colorIndex=skinId
            // Texture1 = face lower, Texture2 = face upper
            else if (baseSection == 1 && !foundFaceLower &&
                     variationIndex == charFaceId && colorIndex == charSkinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) {
                    result.faceLowerPath = tex1;
                    LOG_INFO("  DBC face lower: ", result.faceLowerPath);
                }
                if (!tex2.empty()) {
                    result.faceUpperPath = tex2;
                    LOG_INFO("  DBC face upper: ", result.faceUpperPath);
                }
                foundFaceLower = true;
            }
            // Section 4 = underwear
            else if (baseSection == 4 && !foundUnderwear && colorIndex == charSkinId) {
                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                    std::string tex = charSectionsDbc->getString(r, f);
                    if (!tex.empty() && assetManager_->fileExists(tex)) {
                        result.underwearPaths.push_back(tex);
                        LOG_INFO("  DBC underwear texture: ", tex);
                    }
                }
                foundUnderwear = !result.underwearPaths.empty();
            }

            if (foundSkin && foundHair && foundFaceLower && foundUnderwear) break;
        }

        if (!foundHair) {
            LOG_WARNING("No DBC hair match for style=", static_cast<int>(charHairStyleId),
                        " color=", static_cast<int>(charHairColorId),
                        " race=", targetRaceId, " sex=", targetSexId);
        }
    } else {
        LOG_WARNING("Failed to load CharSections.dbc, using hardcoded textures");
    }

    // Fill model texture slots with resolved paths
    for (auto& tex : model.textures) {
        if (tex.type == 1 && tex.filename.empty()) {
            tex.filename = result.bodySkinPath;
        } else if (tex.type == 6) {
            if (!result.hairTexturePath.empty()) {
                tex.filename = result.hairTexturePath;
            } else if (tex.filename.empty()) {
                tex.filename = std::string("Character\\") + raceFolderName + "\\Hair00_00.blp";
            }
        } else if (tex.type == 8 && tex.filename.empty()) {
            if (!result.underwearPaths.empty()) {
                tex.filename = result.underwearPaths[0];
            } else {
                tex.filename = pelvisPath;
            }
        }
    }

    return result;
}

void AppearanceComposer::compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo) {
    if (!renderer_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;

    // Save skin composite state for re-compositing on equipment changes
    // Include face textures so compositeWithRegions can rebuild the full base
    bodySkinPath_ = texInfo.bodySkinPath;
    underwearPaths_.clear();
    if (!texInfo.faceLowerPath.empty()) underwearPaths_.push_back(texInfo.faceLowerPath);
    if (!texInfo.faceUpperPath.empty()) underwearPaths_.push_back(texInfo.faceUpperPath);
    for (const auto& up : texInfo.underwearPaths) underwearPaths_.push_back(up);

    // Composite body skin + face + underwear overlays
    {
        std::vector<std::string> layers;
        layers.push_back(texInfo.bodySkinPath);
        if (!texInfo.faceLowerPath.empty()) layers.push_back(texInfo.faceLowerPath);
        if (!texInfo.faceUpperPath.empty()) layers.push_back(texInfo.faceUpperPath);
        for (const auto& up : texInfo.underwearPaths) {
            layers.push_back(up);
        }
        if (layers.size() > 1) {
            rendering::VkTexture* compositeTex = charRenderer->compositeTextures(layers);
            if (compositeTex != 0) {
                // Find type-1 (skin) texture slot and replace with composite
                // We need model texture info — walk slots via charRenderer
                // Use the model slot ID to find the right texture index
                auto* modelData = charRenderer->getModelData(modelSlotId);
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        if (modelData->textures[ti].type == 1) {
                            charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), compositeTex);
                            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
                            LOG_INFO("Replaced type-1 texture slot ", ti, " with composited body+face+underwear");
                            break;
                        }
                    }
                }
            }
        }
    }

    // Override hair texture on GPU (type-6 slot) after model load
    if (!texInfo.hairTexturePath.empty()) {
        rendering::VkTexture* hairTex = charRenderer->loadTexture(texInfo.hairTexturePath);
        if (hairTex) {
            auto* modelData = charRenderer->getModelData(modelSlotId);
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    if (modelData->textures[ti].type == 6) {
                        charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), hairTex);
                        LOG_INFO("Applied DBC hair texture to slot ", ti, ": ", texInfo.hairTexturePath);
                        break;
                    }
                }
            }
        }
    }

    // Find cloak (type-2, Object Skin) texture slot index
    {
        auto* modelData = charRenderer->getModelData(modelSlotId);
        if (modelData) {
            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                if (modelData->textures[ti].type == 2) {
                    cloakTextureSlotIndex_ = static_cast<uint32_t>(ti);
                    LOG_INFO("Cloak texture slot: ", ti);
                    break;
                }
            }
        }
    }
}

std::unordered_set<uint16_t> AppearanceComposer::buildDefaultPlayerGeosets(uint8_t raceId, uint8_t sexId,
                                                                           uint8_t hairStyleId, uint8_t facialId) {
    std::unordered_set<uint16_t> activeGeosets;

    // Look up the correct hair scalp geoset from CharHairGeosets.dbc
    uint16_t selectedHairScalp = 1; // default
    std::unordered_set<uint16_t> allHairScalpGeosets;
    if (entitySpawner_) {
        const auto& hairMap = entitySpawner_->getHairGeosetMap();
        uint32_t hairKey = (static_cast<uint32_t>(raceId) << 16) |
                           (static_cast<uint32_t>(sexId) << 8) |
                           static_cast<uint32_t>(hairStyleId);
        auto it = hairMap.find(hairKey);
        if (it != hairMap.end() && it->second > 0)
            selectedHairScalp = it->second;

        // Collect all hair scalp geosets for this race/sex to exclude the others
        for (const auto& [k, v] : hairMap) {
            if (static_cast<uint8_t>((k >> 16) & 0xFF) == raceId &&
                static_cast<uint8_t>((k >> 8) & 0xFF) == sexId &&
                v > 0 && v < 100)
                allHairScalpGeosets.insert(v);
        }
    }

    // Group 0: add body base submeshes (0) but exclude other hair scalp variants
    activeGeosets.insert(0);  // body base
    activeGeosets.insert(selectedHairScalp);
    // Some models have additional non-hair body submeshes (e.g. earrings, jaw);
    // these are typically < 100 and NOT in the hair geoset set
    for (uint16_t i = 1; i < 100; i++) {
        if (allHairScalpGeosets.count(i) == 0)
            activeGeosets.insert(i);  // not a hair geoset, safe to include
    }

    // Hair connector: group 1 = 100 + geoset
    activeGeosets.insert(static_cast<uint16_t>(100 + std::max<uint16_t>(selectedHairScalp, 1)));

    // Facial hair geosets from CharacterFacialHairStyles.dbc
    if (entitySpawner_) {
        const auto& facialMap = entitySpawner_->getFacialHairGeosetMap();
        uint32_t facialKey = (static_cast<uint32_t>(raceId) << 16) |
                             (static_cast<uint32_t>(sexId) << 8) |
                             static_cast<uint32_t>(facialId);
        auto it = facialMap.find(facialKey);
        if (it != facialMap.end()) {
            activeGeosets.insert(static_cast<uint16_t>(200 + std::max<uint16_t>(it->second.geoset200, 1)));
            activeGeosets.insert(static_cast<uint16_t>(300 + std::max<uint16_t>(it->second.geoset300, 1)));
        } else {
            activeGeosets.insert(201);
            activeGeosets.insert(301);
        }
    } else {
        activeGeosets.insert(201);
        activeGeosets.insert(301);
    }

    activeGeosets.insert(kGeosetBareForearms);
    activeGeosets.insert(kGeosetBareShins);
    activeGeosets.insert(kGeosetDefaultEars);
    activeGeosets.insert(kGeosetBareSleeves);
    activeGeosets.insert(kGeosetDefaultKneepads);
    activeGeosets.insert(kGeosetBarePants);
    activeGeosets.insert(kGeosetWithCape);
    activeGeosets.insert(kGeosetBareFeet);
    return activeGeosets;
}

bool AppearanceComposer::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) return false;
    outModel = pipeline::M2Loader::load(m2Data);
    // Load skin (WotLK+ M2 format): strip .m2, append 00.skin
    std::string skinPath = m2Path;
    size_t dotPos = skinPath.rfind('.');
    if (dotPos != std::string::npos) skinPath = skinPath.substr(0, dotPos);
    skinPath += "00.skin";
    auto skinData = assetManager_->readFile(skinPath);
    if (!skinData.empty() && outModel.version >= 264)
        pipeline::M2Loader::loadSkin(skinData, outModel);
    return outModel.isValid();
}

void AppearanceComposer::loadEquippedWeapons() {
    showingRanged_ = false;
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized())
        return;
    if (!gameHandler_) return;

    auto* charRenderer = renderer_->getCharacterRenderer();
    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    auto& inventory = gameHandler_->getInventory();

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) {
        LOG_WARNING("loadEquippedWeapons: failed to load ItemDisplayInfo.dbc");
        return;
    }
    // Mapping: EquipSlot → attachment ID (1=RightHand, 2=LeftHand)
    struct WeaponSlot {
        game::EquipSlot slot;
        uint32_t attachmentId;
    };
    WeaponSlot weaponSlots[] = {
        { game::EquipSlot::MAIN_HAND, 1 },
        { game::EquipSlot::OFF_HAND,  2 },
    };

    if (weaponsSheathed_) {
        for (const auto& ws : weaponSlots) {
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
        }
        charRenderer->detachWeapon(charInstanceId, 1); // ranged may also use right hand
        return;
    }

    bool rightHandFilled = false;

    for (const auto& ws : weaponSlots) {
        const auto& equipSlot = inventory.getEquipSlot(ws.slot);

        // If slot is empty or has no displayInfoId, detach any existing weapon
        if (equipSlot.empty() || equipSlot.item.displayInfoId == 0) {
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        uint32_t displayInfoId = equipSlot.item.displayInfoId;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) {
            LOG_WARNING("loadEquippedWeapons: displayInfoId ", displayInfoId, " not found in DBC");
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
        std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
        std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);

        if (modelName.empty()) {
            LOG_WARNING("loadEquippedWeapons: empty model name for displayInfoId ", displayInfoId);
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        // Convert .mdx → .m2
        std::string modelFile = modelName;
        {
            size_t dotPos = modelFile.rfind('.');
            if (dotPos != std::string::npos) {
                modelFile = modelFile.substr(0, dotPos) + ".m2";
            } else {
                modelFile += ".m2";
            }
        }

        // Try Weapon directory first, then Shield
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                LOG_WARNING("loadEquippedWeapons: failed to load ", modelFile);
                charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
                continue;
            }
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
            }
        }

        uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
        bool ok = charRenderer->attachWeapon(charInstanceId, ws.attachmentId,
                                              weaponModel, weaponModelId, texturePath);
        if (ok) {
            LOG_INFO("Equipped weapon: ", m2Path, " at attachment ", ws.attachmentId);
            if (ws.attachmentId == 1) rightHandFilled = true;
        }
    }

    // --- RANGED slot (bow, gun, crossbow, thrown) ---
    // Show ranged weapon in right hand when main hand is empty.
    const auto& rangedSlot = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (!rightHandFilled && !rangedSlot.empty() && rangedSlot.item.displayInfoId != 0) {
        uint32_t displayInfoId = rangedSlot.item.displayInfoId;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx >= 0) {
            const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
            std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
            std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);

            if (!modelName.empty()) {
                std::string modelFile = modelName;
                {
                    size_t dotPos = modelFile.rfind('.');
                    if (dotPos != std::string::npos) {
                        modelFile = modelFile.substr(0, dotPos) + ".m2";
                    } else {
                        modelFile += ".m2";
                    }
                }

                std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
                pipeline::M2Model weaponModel;
                if (!loadWeaponM2(m2Path, weaponModel)) {
                    m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
                    loadWeaponM2(m2Path, weaponModel);
                }

                if (weaponModel.vertices.size() > 0) {
                    std::string texturePath;
                    if (!textureName.empty()) {
                        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
                        if (!assetManager_->fileExists(texturePath)) {
                            texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                        }
                    }

                    uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
                    bool ok = charRenderer->attachWeapon(charInstanceId, 1,
                                                          weaponModel, weaponModelId, texturePath);
                    if (ok) {
                        LOG_INFO("Equipped ranged weapon: ", m2Path, " at attachment 1 (right hand)");
                    }
                }
            }
        }
    }
}

void AppearanceComposer::showRangedWeapon(bool show) {
    if (show == showingRanged_) return;
    showingRanged_ = show;

    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ || !assetManager_ || !assetManager_->isInitialized())
        return;

    auto* charRenderer = renderer_->getCharacterRenderer();
    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    if (!show) {
        // Swap back to normal melee weapons
        loadEquippedWeapons();
        return;
    }

    auto& inventory = gameHandler_->getInventory();
    const auto& rangedSlot = inventory.getEquipSlot(game::EquipSlot::RANGED);
    if (rangedSlot.empty() || rangedSlot.item.displayInfoId == 0) return;

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    uint32_t displayInfoId = rangedSlot.item.displayInfoId;
    int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
    if (recIdx < 0) return;

    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    std::string modelFile = modelName;
    {
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos)
            modelFile = modelFile.substr(0, dotPos) + ".m2";
        else
            modelFile += ".m2";
    }

    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model weaponModel;
    if (!loadWeaponM2(m2Path, weaponModel)) {
        m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
        if (!loadWeaponM2(m2Path, weaponModel)) return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
        if (!assetManager_->fileExists(texturePath))
            texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
    }

    // Detach current right-hand weapon and attach ranged weapon
    charRenderer->detachWeapon(charInstanceId, 1);
    uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
    bool ok = charRenderer->attachWeapon(charInstanceId, 1, weaponModel, weaponModelId, texturePath);
    if (ok) {
        LOG_INFO("Swapped to ranged weapon: ", m2Path, " at attachment 1 (right hand)");
    }
}

} // namespace core
} // namespace wowee
