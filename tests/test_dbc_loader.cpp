// DBC binary parsing tests with synthetic data
#include <catch_amalgamated.hpp>
#include "pipeline/dbc_loader.hpp"
#include <cstring>

using wowee::pipeline::DBCFile;

// Build a minimal valid DBC in memory:
//   Header: "WDBC" + recordCount(uint32) + fieldCount(uint32) + recordSize(uint32) + stringBlockSize(uint32)
//   Records: contiguous fixed-size rows
//   String block: null-terminated strings
static std::vector<uint8_t> buildSyntheticDBC(
    uint32_t numRecords, uint32_t numFields,
    const std::vector<std::vector<uint32_t>>& records,
    const std::string& stringBlock)
{
    const uint32_t recordSize = numFields * 4;
    const uint32_t stringBlockSize = static_cast<uint32_t>(stringBlock.size());

    std::vector<uint8_t> data;
    // Reserve enough space
    data.reserve(20 + numRecords * recordSize + stringBlockSize);

    // Magic
    data.push_back('W'); data.push_back('D'); data.push_back('B'); data.push_back('C');

    auto writeU32 = [&](uint32_t v) {
        data.push_back(static_cast<uint8_t>(v & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    writeU32(numRecords);
    writeU32(numFields);
    writeU32(recordSize);
    writeU32(stringBlockSize);

    // Records
    for (const auto& rec : records) {
        for (uint32_t field : rec) {
            writeU32(field);
        }
    }

    // String block
    for (char c : stringBlock) {
        data.push_back(static_cast<uint8_t>(c));
    }

    return data;
}

TEST_CASE("DBCFile default state", "[dbc]") {
    DBCFile dbc;
    REQUIRE_FALSE(dbc.isLoaded());
    REQUIRE(dbc.getRecordCount() == 0);
    REQUIRE(dbc.getFieldCount() == 0);
}

TEST_CASE("DBCFile load valid DBC", "[dbc]") {
    // 2 records, 3 fields each: [id, intVal, stringOffset]
    // String block: "\0Hello\0World\0" → offset 0="" 1="Hello" 7="World"
    std::string strings;
    strings += '\0';             // offset 0: empty string
    strings += "Hello";
    strings += '\0';             // offset 1-6: "Hello"
    strings += "World";
    strings += '\0';             // offset 7-12: "World"

    auto data = buildSyntheticDBC(2, 3,
        {
            {1, 100, 1},   // Record 0: id=1, intVal=100, stringOffset=1 → "Hello"
            {2, 200, 7},   // Record 1: id=2, intVal=200, stringOffset=7 → "World"
        },
        strings);

    DBCFile dbc;
    REQUIRE(dbc.load(data));
    REQUIRE(dbc.isLoaded());
    REQUIRE(dbc.getRecordCount() == 2);
    REQUIRE(dbc.getFieldCount() == 3);
    REQUIRE(dbc.getRecordSize() == 12);
    REQUIRE(dbc.getStringBlockSize() == strings.size());
}

TEST_CASE("DBCFile getUInt32 and getInt32", "[dbc]") {
    auto data = buildSyntheticDBC(1, 2,
        { {42, 0xFFFFFFFF} },
        std::string(1, '\0'));

    DBCFile dbc;
    REQUIRE(dbc.load(data));

    REQUIRE(dbc.getUInt32(0, 0) == 42);
    REQUIRE(dbc.getUInt32(0, 1) == 0xFFFFFFFF);
    REQUIRE(dbc.getInt32(0, 1) == -1);
}

TEST_CASE("DBCFile getFloat", "[dbc]") {
    float testVal = 3.14f;
    uint32_t bits;
    std::memcpy(&bits, &testVal, 4);

    auto data = buildSyntheticDBC(1, 2,
        { {1, bits} },
        std::string(1, '\0'));

    DBCFile dbc;
    REQUIRE(dbc.load(data));
    REQUIRE(dbc.getFloat(0, 1) == Catch::Approx(3.14f));
}

TEST_CASE("DBCFile getString", "[dbc]") {
    std::string strings;
    strings += '\0';
    strings += "TestString";
    strings += '\0';

    auto data = buildSyntheticDBC(1, 2,
        { {1, 1} },  // field 1 = string offset 1 → "TestString"
        strings);

    DBCFile dbc;
    REQUIRE(dbc.load(data));
    REQUIRE(dbc.getString(0, 1) == "TestString");
}

TEST_CASE("DBCFile getStringView", "[dbc]") {
    std::string strings;
    strings += '\0';
    strings += "ViewTest";
    strings += '\0';

    auto data = buildSyntheticDBC(1, 2,
        { {1, 1} },
        strings);

    DBCFile dbc;
    REQUIRE(dbc.load(data));
    REQUIRE(dbc.getStringView(0, 1) == "ViewTest");
}

TEST_CASE("DBCFile findRecordById", "[dbc]") {
    auto data = buildSyntheticDBC(3, 2,
        {
            {10, 100},
            {20, 200},
            {30, 300},
        },
        std::string(1, '\0'));

    DBCFile dbc;
    REQUIRE(dbc.load(data));

    REQUIRE(dbc.findRecordById(10) == 0);
    REQUIRE(dbc.findRecordById(20) == 1);
    REQUIRE(dbc.findRecordById(30) == 2);
    REQUIRE(dbc.findRecordById(99) == -1);
}

TEST_CASE("DBCFile getRecord returns pointer", "[dbc]") {
    auto data = buildSyntheticDBC(1, 2,
        { {0xAB, 0xCD} },
        std::string(1, '\0'));

    DBCFile dbc;
    REQUIRE(dbc.load(data));

    const uint8_t* rec = dbc.getRecord(0);
    REQUIRE(rec != nullptr);

    // First field should be 0xAB in little-endian
    uint32_t val;
    std::memcpy(&val, rec, 4);
    REQUIRE(val == 0xAB);
}

TEST_CASE("DBCFile load too small data", "[dbc]") {
    std::vector<uint8_t> tiny = {'W', 'D', 'B'};
    DBCFile dbc;
    REQUIRE_FALSE(dbc.load(tiny));
}

TEST_CASE("DBCFile load wrong magic", "[dbc]") {
    auto data = buildSyntheticDBC(0, 1, {}, std::string(1, '\0'));
    // Corrupt magic
    data[0] = 'X';

    DBCFile dbc;
    REQUIRE_FALSE(dbc.load(data));
}

TEST_CASE("DBCFile getStringByOffset", "[dbc]") {
    std::string strings;
    strings += '\0';
    strings += "Offset5";  // This would be at offset 1 actually, let me be precise
    strings += '\0';

    auto data = buildSyntheticDBC(1, 1,
        { {0} },
        strings);

    DBCFile dbc;
    REQUIRE(dbc.load(data));
    REQUIRE(dbc.getStringByOffset(1) == "Offset5");
    REQUIRE(dbc.getStringByOffset(0).empty());
}
