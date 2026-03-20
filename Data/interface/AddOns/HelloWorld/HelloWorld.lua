-- HelloWorld addon — test the WoWee addon system
print("|cff00ff00[HelloWorld]|r Addon loaded! Lua 5.1 is working.")

-- Register for game events
RegisterEvent("PLAYER_ENTERING_WORLD", function(event)
    local name = UnitName("player")
    local level = UnitLevel("player")
    local health = UnitHealth("player")
    local maxHealth = UnitHealthMax("player")
    local _, _, classId = UnitClass("player")
    local gold = math.floor(GetMoney() / 10000)

    print("|cff00ff00[HelloWorld]|r Welcome, " .. name .. "! (Level " .. level .. ")")
    if maxHealth > 0 then
        print("|cff00ff00[HelloWorld]|r Health: " .. health .. "/" .. maxHealth)
    end
    if gold > 0 then
        print("|cff00ff00[HelloWorld]|r Gold: " .. gold .. "g")
    end
end)

RegisterEvent("PLAYER_LEAVING_WORLD", function(event)
    print("|cff00ff00[HelloWorld]|r Goodbye!")
end)
