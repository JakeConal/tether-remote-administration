// tests/file_family_test.cpp - task 6.3: the file family on REAL temporary
// disk trees through the capability agent layer (the dispatcher performs the
// confirmation gate exactly as the wired agent does), covering both
// operating systems with the one std::filesystem implementation. Scenarios
// map to the remote-commands delta:
//  - file listing (read-style) with kinds/deterministic order, structured
//    not-found on a missing directory ("well-formed parameters use the
//    structured path")
//  - "parameter containing shell metacharacters is treated literally": a
//    file literally named with ';' and '&' (both legal in NTFS and POSIX
//    names) is looked up by that literal name and deleted; a sibling guard
//    file is never touched;
//  - confirmation refusal AT THE AGENT LAYER: delete/move/copy without the
//    confirmation token return the structured refusal and the DISK proves
//    nothing happened (zero side effects) - list needs no token.
//  - destructive flows proceed only with the token.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "matrix.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tsys = tether::syscalls;
namespace fs = std::filesystem;

namespace {

std::atomic<int> g_dir_counter{0};

struct Fixture {
    Fixture() : root(fs::temp_directory_path() /
                     ("tether-file-tests-" + std::to_string(g_dir_counter++))) {
        std::error_code ec;
        fs::create_directories(root, ec);
        REQUIRE_MESSAGE(!ec, "could not create the test directory");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec); // the suite owns its tree, nothing else
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path file(const std::string& name) const { return root / name; }

    void write(const fs::path& path, const std::string& content) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE_MESSAGE(static_cast<bool>(out), "could not open the fixture write stream");
        out << content;
        out.flush();
        REQUIRE(static_cast<bool>(out)); // the write landed before tests assert on it
    }

    std::string read(const fs::path& path) const {
        std::ifstream in(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        return content;
    }

    bool exists(const fs::path& path) const {
        std::error_code ec;
        return fs::exists(path, ec) && !ec;
    }

    fs::path root;
};

// One dispatcher per case, real file system, agent-layer gates live.
tsys::CommandDispatcher wired_dispatcher() {
    tsys::CommandDispatcher dispatcher;
    dispatcher.set_file_system(tsys::make_file_system());
    return dispatcher;
}

std::string pseudo_random_blob(std::size_t count) {
    // Deterministic bytes: copy comparisons must be byte-exact.
    std::string blob;
    blob.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        blob.push_back(static_cast<char>(i * 31 % 253 + 1));
    }
    return blob;
}

} // namespace

TEST_CASE("ls lists entries with kind and size, sorted, without confirmation") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    fixture.write(fixture.file("alpha.txt"), "hello");            // 5 bytes
    fixture.write(fixture.file("c.txt"), pseudo_random_blob(26)); // 26 bytes
    std::error_code ec;
    fs::create_directory(fixture.file("beta"), ec);
    REQUIRE(!ec);

    const tsys::Outcome outcome = dispatcher.dispatch("ls", {fixture.root.string()});
    REQUIRE(outcome.ok); // a read-style op never asks for confirmation
    REQUIRE(outcome.lines.size() == 3);
    CHECK(outcome.lines[0] == "alpha.txt 5");
    CHECK(outcome.lines[1] == "beta/"); // directories render with the slash
    CHECK(outcome.lines[2] == "c.txt 26");
}

TEST_CASE("ls with no argument lists the agent working directory") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();
    std::error_code ec;
    fs::current_path(fixture.root, ec); // a known-nonempty cwd (alpha below)
    REQUIRE(!ec);
    fixture.write(fixture.file("listed.txt"), "x");

    const tsys::Outcome outcome = dispatcher.dispatch("ls", {});
    CHECK(outcome.ok);
    bool found = false;
    for (const std::string& row : outcome.lines) {
        if (row.find("listed.txt") == 0) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("ls of a missing or non-directory path is a structured not-found") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    std::error_code ec;
    fs::create_directory(fixture.file("empty"), ec);
    REQUIRE(!ec);
    // The "empty" name is the case: an existing-but-empty directory is a
    // SUCCESS with zero rows; a missing path is the classified failure.
    const tsys::Outcome empty_dir = dispatcher.dispatch("ls", {fixture.file("empty").string()});
    CHECK(empty_dir.ok);
    CHECK(empty_dir.lines.empty());

    const tsys::Outcome missing = dispatcher.dispatch("ls", {fixture.file("absent").string()});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == "not_found");

    // A file passed in the directory slot fails the same lookup, not with an
    // unclassified crash.
    fixture.write(fixture.file("plain"), "content");
    const tsys::Outcome plain = dispatcher.dispatch("ls", {fixture.file("plain").string()});
    CHECK_FALSE(plain.ok);
    CHECK(plain.code == "not_found");
}

TEST_CASE("delete without confirmation refuses at the agent layer; the disk stays intact") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();
    const fs::path guarded = fixture.file("guarded.txt");
    fixture.write(guarded, "payload");

    const tsys::Outcome refused = dispatcher.dispatch("delete",
                                                      {guarded.string()}); // no token
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.code == "missing_confirmation");
    CHECK(refused.message.find("confirm") != std::string::npos);

    // Disk proof of zero side effects: the file is byte-identical still.
    REQUIRE(fixture.exists(guarded));
    CHECK(fixture.read(guarded) == "payload");

    // A wrong token is equally refused (the first argument IS the token).
    const tsys::Outcome wrong_token = dispatcher.dispatch("delete", {"yes", guarded.string()});
    CHECK(wrong_token.code == "missing_confirmation");
    CHECK(fixture.exists(guarded));

    // An unconfirmed delete of an absent path refuses before any lookup.
    CHECK(dispatcher.dispatch("delete", {fixture.file("absent.txt").string()}).code ==
          "missing_confirmation");
}

TEST_CASE("a confirmed delete removes exactly the literal name") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    // The metacharacter scenario on a REAL file: ';' and '&' are legal in
    // NTFS and POSIX names, so the literal-name lookup is provable on disk.
    const fs::path literal = fixture.file("a;b&c.txt");
    fixture.write(literal, "gone soon");
    const fs::path guard = fixture.file("keep.txt");
    fixture.write(guard, "stays");

    const tsys::Outcome outcome = dispatcher.dispatch("delete",
                                                      {tsys::kConfirmMarker, literal.string()});
    REQUIRE(outcome.ok);
    CHECK_FALSE(fixture.exists(literal));  // the literal name was removed
    CHECK(fixture.exists(guard));          // nothing unrelated was touched
    CHECK(fixture.read(guard) == "stays");

    // And a confirmed delete of a MISSING path is the structured not-found,
    // not a crash or a silent success.
    const tsys::Outcome missing =
        dispatcher.dispatch("delete", {tsys::kConfirmMarker, fixture.file("absent.txt").string()});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == "not_found");
}

TEST_CASE("a non-empty directory delete reports a structured refusal") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    std::error_code ec;
    fs::create_directory(fixture.file("tree"), ec);
    REQUIRE(!ec);
    fixture.write(fixture.file("tree") / "inner.txt", "x");

    const tsys::Outcome outcome =
        dispatcher.dispatch("delete", {tsys::kConfirmMarker, fixture.file("tree").string()});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.code == "directory_not_empty");
    CHECK(fixture.exists(fixture.file("tree/inner.txt"))); // nothing removed
}

TEST_CASE("move without confirmation refuses; the disk keeps both sides untouched") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();
    const fs::path source = fixture.file("source.txt");
    fixture.write(source, pseudo_random_blob(128));

    const tsys::Outcome refused =
        dispatcher.dispatch("move", {source.string(), fixture.file("moved.txt").string()});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.code == "missing_confirmation");
    CHECK(fixture.exists(source));                              // source intact
    CHECK_FALSE(fixture.exists(fixture.file("moved.txt")));      // nothing appeared
    CHECK(fixture.read(source) == pseudo_random_blob(128));     // byte-identical
}

TEST_CASE("a confirmed move renames the literal name with byte-exact content") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    const fs::path weird = fixture.file("weird;name&src.txt");
    const std::string blob = pseudo_random_blob(200);
    fixture.write(weird, blob);
    const fs::path destination = fixture.file("renamed.txt");

    const tsys::Outcome outcome =
        dispatcher.dispatch("move", {tsys::kConfirmMarker, weird.string(), destination.string()});
    REQUIRE(outcome.ok);
    CHECK_FALSE(fixture.exists(weird));      // the literal name is no longer there
    CHECK(fixture.read(destination) == blob); // bytes moved verbatim
}

TEST_CASE("copy without confirmation refuses; nothing is written") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();
    const fs::path source = fixture.file("src.txt");
    fixture.write(source, "copy me");

    const tsys::Outcome refused =
        dispatcher.dispatch("copy", {source.string(), fixture.file("dst.txt").string()});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.code == "missing_confirmation");
    CHECK_FALSE(fixture.exists(fixture.file("dst.txt"))); // zero side effects on disk
}

TEST_CASE("a confirmed copy duplicates bytes and overwrites the target") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    const fs::path source = fixture.file("src.txt");
    const std::string blob = pseudo_random_blob(400);
    fixture.write(source, blob);
    const fs::path destination = fixture.file("dst.txt");
    fixture.write(destination, "old content"); // pre-existing target

    const tsys::Outcome outcome =
        dispatcher.dispatch("copy", {tsys::kConfirmMarker, source.string(), destination.string()});
    REQUIRE(outcome.ok);
    CHECK(fixture.exists(source));                  // a copy leaves the source
    CHECK(fixture.read(destination) == blob);       // byte-exact, overwrite applied

    // A confirmed copy of a missing source is the structured not-found.
    const tsys::Outcome missing = dispatcher.dispatch(
        "copy", {tsys::kConfirmMarker, fixture.file("absent.txt").string(), destination.string()});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == "not_found");
    CHECK(fixture.read(destination) == blob); // the failed op changed nothing
}

TEST_CASE("directories move and copy recursively with the token") {
    Fixture fixture;
    tsys::CommandDispatcher dispatcher = wired_dispatcher();

    std::error_code ec;
    fs::create_directory(fixture.file("srcdir"), ec);
    REQUIRE(!ec);
    fixture.write(fixture.file("srcdir/inner.txt"), "nested");
    fs::create_directories(fixture.file("srcdir/nested"), ec);
    REQUIRE(!ec);
    fixture.write(fixture.file("srcdir/nested/deep.txt"), "deeper");

    // Copy the whole tree, confirmed.
    const tsys::Outcome copied =
        dispatcher.dispatch("copy", {tsys::kConfirmMarker, fixture.file("srcdir").string(),
                                     fixture.file("copied").string()});
    REQUIRE(copied.ok);
    CHECK(fixture.read(fixture.file("copied/inner.txt")) == "nested");
    CHECK(fixture.read(fixture.file("copied/nested/deep.txt")) == "deeper");
    CHECK(fixture.read(fixture.file("srcdir/inner.txt")) == "nested"); // still there

    // Move the whole tree, confirmed: source gone, bytes preserved.
    const tsys::Outcome moved =
        dispatcher.dispatch("move", {tsys::kConfirmMarker, fixture.file("srcdir").string(),
                                     fixture.file("moved").string()});
    REQUIRE(moved.ok);
    CHECK_FALSE(fixture.exists(fixture.file("srcdir")));
    CHECK(fixture.read(fixture.file("moved/inner.txt")) == "nested");
}
