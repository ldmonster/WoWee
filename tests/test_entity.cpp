// Entity, Unit, Player, GameObject, EntityManager tests
#include <catch_amalgamated.hpp>
#include "game/entity.hpp"
#include <memory>

using namespace wowee::game;

TEST_CASE("Entity default construction", "[entity]") {
    Entity e;
    REQUIRE(e.getGuid() == 0);
    REQUIRE(e.getType() == ObjectType::OBJECT);
    REQUIRE(e.getX() == 0.0f);
    REQUIRE(e.getY() == 0.0f);
    REQUIRE(e.getZ() == 0.0f);
    REQUIRE(e.getOrientation() == 0.0f);
}

TEST_CASE("Entity GUID constructor", "[entity]") {
    Entity e(0xDEADBEEF);
    REQUIRE(e.getGuid() == 0xDEADBEEF);
}

TEST_CASE("Entity position set/get", "[entity]") {
    Entity e;
    e.setPosition(1.0f, 2.0f, 3.0f, 1.57f);
    REQUIRE(e.getX() == Catch::Approx(1.0f));
    REQUIRE(e.getY() == Catch::Approx(2.0f));
    REQUIRE(e.getZ() == Catch::Approx(3.0f));
    REQUIRE(e.getOrientation() == Catch::Approx(1.57f));
}

TEST_CASE("Entity field set/get/has", "[entity]") {
    Entity e;
    REQUIRE_FALSE(e.hasField(10));

    e.setField(10, 0xCAFE);
    REQUIRE(e.hasField(10));
    REQUIRE(e.getField(10) == 0xCAFE);

    // Overwrite
    e.setField(10, 0xBEEF);
    REQUIRE(e.getField(10) == 0xBEEF);

    // Non-existent returns 0
    REQUIRE(e.getField(999) == 0);
}

TEST_CASE("Unit construction and type", "[entity]") {
    Unit u;
    REQUIRE(u.getType() == ObjectType::UNIT);

    Unit u2(0x123);
    REQUIRE(u2.getGuid() == 0x123);
    REQUIRE(u2.getType() == ObjectType::UNIT);
}

TEST_CASE("Unit name", "[entity]") {
    Unit u;
    REQUIRE(u.getName().empty());
    u.setName("Hogger");
    REQUIRE(u.getName() == "Hogger");
}

TEST_CASE("Unit health", "[entity]") {
    Unit u;
    REQUIRE(u.getHealth() == 0);
    REQUIRE(u.getMaxHealth() == 0);

    u.setHealth(500);
    u.setMaxHealth(1000);
    REQUIRE(u.getHealth() == 500);
    REQUIRE(u.getMaxHealth() == 1000);
}

TEST_CASE("Unit power by type", "[entity]") {
    Unit u;
    u.setPowerType(0); // mana
    u.setPower(200);
    u.setMaxPower(500);

    REQUIRE(u.getPower() == 200);
    REQUIRE(u.getMaxPower() == 500);
    REQUIRE(u.getPowerByType(0) == 200);
    REQUIRE(u.getMaxPowerByType(0) == 500);

    // Set rage (type 1)
    u.setPowerByType(1, 50);
    u.setMaxPowerByType(1, 100);
    REQUIRE(u.getPowerByType(1) == 50);
    REQUIRE(u.getMaxPowerByType(1) == 100);

    // Out of bounds clamps
    REQUIRE(u.getPowerByType(7) == 0);
    REQUIRE(u.getMaxPowerByType(7) == 0);
}

TEST_CASE("Unit level, entry, displayId", "[entity]") {
    Unit u;
    REQUIRE(u.getLevel() == 1); // default
    u.setLevel(80);
    REQUIRE(u.getLevel() == 80);

    u.setEntry(1234);
    REQUIRE(u.getEntry() == 1234);

    u.setDisplayId(5678);
    REQUIRE(u.getDisplayId() == 5678);
}

TEST_CASE("Unit flags", "[entity]") {
    Unit u;
    u.setUnitFlags(0x01);
    REQUIRE(u.getUnitFlags() == 0x01);

    u.setDynamicFlags(0x02);
    REQUIRE(u.getDynamicFlags() == 0x02);

    u.setNpcFlags(0x04);
    REQUIRE(u.getNpcFlags() == 0x04);
    REQUIRE(u.isInteractable());

    u.setNpcFlags(0);
    REQUIRE_FALSE(u.isInteractable());
}

TEST_CASE("Unit faction and hostility", "[entity]") {
    Unit u;
    u.setFactionTemplate(14); // Undercity
    REQUIRE(u.getFactionTemplate() == 14);

    REQUIRE_FALSE(u.isHostile());
    u.setHostile(true);
    REQUIRE(u.isHostile());
}

TEST_CASE("Unit mount display ID", "[entity]") {
    Unit u;
    REQUIRE(u.getMountDisplayId() == 0);
    u.setMountDisplayId(14374);
    REQUIRE(u.getMountDisplayId() == 14374);
}

TEST_CASE("Player inherits Unit", "[entity]") {
    Player p(0xABC);
    REQUIRE(p.getType() == ObjectType::PLAYER);
    REQUIRE(p.getGuid() == 0xABC);

    // Player inherits Unit name — regression test for the shadowed-field fix
    p.setName("Arthas");
    REQUIRE(p.getName() == "Arthas");

    p.setLevel(80);
    REQUIRE(p.getLevel() == 80);
}

TEST_CASE("GameObject construction", "[entity]") {
    GameObject go(0x999);
    REQUIRE(go.getType() == ObjectType::GAMEOBJECT);
    REQUIRE(go.getGuid() == 0x999);

    go.setName("Mailbox");
    REQUIRE(go.getName() == "Mailbox");

    go.setEntry(42);
    REQUIRE(go.getEntry() == 42);

    go.setDisplayId(100);
    REQUIRE(go.getDisplayId() == 100);
}

TEST_CASE("EntityManager add/get/has/remove", "[entity]") {
    EntityManager mgr;
    REQUIRE(mgr.getEntityCount() == 0);

    auto unit = std::make_shared<Unit>(1);
    unit->setName("TestUnit");
    mgr.addEntity(1, unit);

    REQUIRE(mgr.getEntityCount() == 1);
    REQUIRE(mgr.hasEntity(1));
    REQUIRE_FALSE(mgr.hasEntity(2));

    auto retrieved = mgr.getEntity(1);
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->getGuid() == 1);

    mgr.removeEntity(1);
    REQUIRE_FALSE(mgr.hasEntity(1));
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager clear", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, std::make_shared<Entity>(1));
    mgr.addEntity(2, std::make_shared<Entity>(2));
    REQUIRE(mgr.getEntityCount() == 2);

    mgr.clear();
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager null entity rejected", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, nullptr);
    // Null should be rejected (logged warning, not stored)
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager getEntities returns all", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(10, std::make_shared<Unit>(10));
    mgr.addEntity(20, std::make_shared<Player>(20));
    mgr.addEntity(30, std::make_shared<GameObject>(30));

    const auto& all = mgr.getEntities();
    REQUIRE(all.size() == 3);
    REQUIRE(all.count(10) == 1);
    REQUIRE(all.count(20) == 1);
    REQUIRE(all.count(30) == 1);
}
