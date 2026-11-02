#!/usr/bin/env python3
"""
Runs the *actual* SQL statements of TrueCompanionSqlRepository.cpp against a real
MySQL/MariaDB server, after applying the module's migration.

The statement templates are extracted from the C++ source (adjacent string
literals are concatenated, fmt placeholders are filled with sample values), so
a typo in the C++ SQL fails this test.

    python3 tests/test_sql.py .  [mysql command, default: "sudo mysql"]
"""
import re
import subprocess
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
mysql = (sys.argv[2] if len(sys.argv) > 2 else "sudo mysql").split()
DB = "true_companion_sqltest"
passed = failed = 0


def run(sql, db=DB, expect_ok=True):
    global passed, failed
    p = subprocess.run(mysql + ([db] if db else []) + ["-N", "-B", "-e", sql], capture_output=True, text=True)
    ok = p.returncode == 0
    if ok == expect_ok:
        passed += 1
    else:
        failed += 1
        print("FAIL:", sql[:300], "\n  ", p.stderr.strip())
    return p.stdout.strip()


def check(cond, msg):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", msg)


run(f"DROP DATABASE IF EXISTS {DB}; CREATE DATABASE {DB} CHARACTER SET utf8mb4;", db=None)
import glob
for path in sorted(glob.glob(f"{root}/data/sql/db-characters/base/*.sql")):
    migration = open(path, encoding="utf-8").read()
    run(migration)
    run(migration)  # idempotent (CREATE TABLE IF NOT EXISTS)

# the rename from mod-romance: only the new tables are left
tables = run("SELECT table_name FROM information_schema.tables WHERE table_schema = DATABASE() ORDER BY table_name").split()
check(len([t for t in tables if t.startswith("true_companion_")]) == 7, f"7 true_companion_* tables after the migrations: {tables}")
check(not [t for t in tables if t.startswith("romance_")], f"no romance_* tables left: {tables}")

# an existing mod-romance database: the data moves along with the rename
OLD = "true_companion_sqltest_upgrade"
run(f"DROP DATABASE IF EXISTS {OLD}; CREATE DATABASE {OLD} CHARACTER SET utf8mb4;", db=None)
paths = sorted(glob.glob(f"{root}/data/sql/db-characters/base/*.sql"))
for path in [p for p in paths if "/mod_romance_" in p]:
    run(open(path, encoding="utf-8").read(), db=OLD)
run("INSERT INTO romance_preset (account_id, preset_id, label, profile) VALUES (1, 1, 'kept', '{}')", db=OLD)
for path in [p for p in paths if "/mod_romance_" not in p]:
    run(open(path, encoding="utf-8").read(), db=OLD)
check(run("SELECT label FROM true_companion_preset WHERE account_id = 1", db=OLD) == "kept", "upgrade: rows are kept by the rename")
check(run("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name LIKE 'romance\\_%'", db=OLD) == "0",
      "upgrade: no romance_* tables left")
run(f"DROP DATABASE {OLD}", db=None)

src = open(f"{root}/src/ac/TrueCompanionSqlRepository.cpp", encoding="utf-8").read()
columns = re.search(r'kCompanionColumns =\s*((?:"[^"]*"\s*)+);', src).group(1)
columns = "".join(re.findall(r'"([^"]*)"', columns))

# Extract every SQL template passed to Query/Execute/Append.
templates = []
for m in re.finditer(r'(?:CharacterDatabase\.(?:Query|Execute|DirectExecute)|trans->Append)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', src):
    templates.append("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))))
check(len(templates) >= 20, f"found {len(templates)} SQL templates in TrueCompanionSqlRepository.cpp")


def fill(t):
    t = t.replace("SELECT {} FROM true_companion_companion", f"SELECT {columns} FROM true_companion_companion")
    t = t.replace("INSERT INTO true_companion_companion ({}, stage)", f"INSERT INTO true_companion_companion ({columns}, stage)")
    t = re.sub(r"\{0\}", "1", t)
    t = re.sub(r"\{1\}", "2", t)
    t = re.sub(r"\{2\}", "5", t)
    t = t.replace("'{}'", "'x'")
    t = t.replace("IN ({})", "IN (1,2)")
    t = t.replace("{}", "1")
    return t


for t in templates:
    if "information_schema" in t:
        t = t.replace("'{}'", "'true_companion_memory'")
        out = run(t)
        check(out == "1", "schema check finds table")
        continue
    run(fill(t))

# ---- semantic checks with realistic values
run("INSERT INTO true_companion_companion (" + columns + ", stage) VALUES (1, 2, 100, 'Elara', 1, '{\"name\":\"Elara\"}', '{\"stage\":\"dating\"}', '{}', 0, 10, 10, 5) "
    "ON DUPLICATE KEY UPDATE relationship = VALUES(relationship), stage = VALUES(stage)")
run("INSERT INTO true_companion_companion (" + columns + ", stage) VALUES (1, 2, 100, 'Elara', 1, '{\"name\":\"Elara\"}', '{\"stage\":\"engaged\"}', '{}', 0, 10, 20, 6) "
    "ON DUPLICATE KEY UPDATE relationship = VALUES(relationship), stage = VALUES(stage)")
check(run("SELECT stage FROM true_companion_companion WHERE owner_guid=1 AND companion_guid=2") == "6", "companion upsert updates in place")

# unicode + quotes in memory text survive (escaped like EscapeString would)
run("INSERT INTO true_companion_memory (owner_guid, companion_guid, id, kind, source, importance, text, pinned, recall_count, created_at, updated_at, last_recalled_at) "
    "VALUES (1, 2, 1, 'gift', 'observed', 80, 'Arthas gave Elara a ring \\'Grüße\\' ♥', 0, 0, 1, 1, 0)")
check("Grüße" in run("SELECT text FROM true_companion_memory WHERE id=1"), "utf8mb4 text stored")

# gift replay protection: the same item instance can be logged only once per companion
gift = ("INSERT IGNORE INTO true_companion_gift_log (owner_guid, companion_guid, item_guid, item_entry, item_count, item_name, category, score, created_at) "
        "VALUES (1, 2, {g}, 43250, 1, 'Ring', 'ring', 70, 5)")
run(gift.format(g="777"))
run(gift.format(g="777"))
run(gift.format(g="NULL"))
run(gift.format(g="NULL"))
check(run("SELECT COUNT(*) FROM true_companion_gift_log WHERE item_guid = 777") == "1", "duplicate item guid ignored")
check(run("SELECT COUNT(*) FROM true_companion_gift_log WHERE item_guid IS NULL") == "2", "gold gifts (NULL guid) allowed repeatedly")

# chat pruning keeps the newest N rows
run("DELETE FROM true_companion_chat_log")
for i in range(20):
    run(f"INSERT INTO true_companion_chat_log (owner_guid, companion_guid, role, text, created_at) VALUES (1, 2, {i % 2}, 'line {i}', {i})")
prune = [t for t in templates if t.startswith("DELETE FROM true_companion_chat_log WHERE owner_guid = {0}")][0]
run(prune.replace("{0}", "1").replace("{1}", "2").replace("{2}", "5"))
check(run("SELECT COUNT(*) FROM true_companion_chat_log WHERE owner_guid=1 AND companion_guid=2") == "5", "prune keeps newest 5")
check(run("SELECT MIN(text) FROM true_companion_chat_log") == "line 15", "prune removed the oldest")

# custom presets
upsert = [t for t in templates if t.startswith("INSERT INTO true_companion_preset")][0]
for label in ("Herbalist", "Herbalist v2"):
    sql = upsert
    for value in ("100", "1", label, "desc", "{}"):
        sql = sql.replace("{}", value, 1)
    run(sql)
check(run("SELECT label FROM true_companion_preset WHERE account_id=100 AND preset_id=1") == "Herbalist v2", "preset upsert updates in place")
check(run("SELECT COUNT(*) FROM true_companion_preset") == "1", "one preset row")

run(f"DROP DATABASE {DB}", db=None)
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
