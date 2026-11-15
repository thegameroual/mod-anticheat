#!/usr/bin/env python3
"""Run against a new private mysqld datadir/socket; never an existing database.

Requires Linux, Python 3, g++, mysqld, mysql and mysqladmin on PATH. No Python
packages or network listener are used. Run as a non-root user.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise RuntimeError("Run as a non-root user; this script owns a disposable mysqld only")
    for command in ("g++", "mysqld", "mysql", "mysqladmin"):
        if not shutil.which(command):
            raise RuntimeError(f"Missing command: {command}")
    root = Path(__file__).resolve().parents[1]
    workspace = args.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    # A unique directory prevents accidental reuse of a user's real data.
    work = Path(tempfile.mkdtemp(prefix="sql-validation-", dir=workspace))
    data = work / "data"
    data.mkdir()
    socket = work / "mysql.sock"
    log = work / "mysql.log"
    fixture = work / "sql_fixture"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "src"), str(root / "tests/SqlFixture.cpp"),
                    str(root / "src/persistence/ReportSql.cpp"), "-o", str(fixture)], check=True)
    statements = subprocess.check_output([str(fixture)], text=True).splitlines()
    assert len(statements) == 7
    subprocess.run(["mysqld", "--no-defaults", "--initialize-insecure",
                    f"--datadir={data}", f"--log-error={log}"], check=True)
    server = subprocess.Popen(["mysqld", "--no-defaults", f"--datadir={data}",
                               "--skip-networking", "--mysqlx=OFF", f"--socket={socket}",
                               f"--pid-file={work / 'mysql.pid'}", f"--log-error={log}",
                               "--innodb-buffer-pool-size=32M", "--secure-file-priv=NULL"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = ["mysql", "--no-defaults", "--protocol=socket", f"--socket={socket}",
              "--user=root", "--batch", "--skip-column-names", "--binary-mode",
              "--default-character-set=utf8mb4"]
    admin = ["mysqladmin", "--no-defaults", "--protocol=socket", f"--socket={socket}", "--user=root"]
    passed = False

    def sql(query, database=True, expect_success=True):
        result = subprocess.run(client + (["anticheat_validation"] if database else []),
                                input=query, text=True, capture_output=True)
        if expect_success and result.returncode:
            raise RuntimeError(result.stderr)
        if not expect_success and not result.returncode:
            raise AssertionError("Expected SQL transaction failure")
        return result.stdout.strip()

    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError(log.read_text())
            if subprocess.run(admin + ["ping"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Disposable mysqld did not become ready")
        print("Database:", sql("SELECT VERSION();", database=False))
        sql("CREATE DATABASE anticheat_validation CHARACTER SET utf8mb4;", database=False)
        base = (root / "data/sql/db-characters/base/mod_anticheat.sql").read_text()
        migration = (root / "data/sql/db-characters/updates/2026_11_15_00_anticheat_events_v2.sql").read_text()
        incidents = (root / "data/sql/db-characters/updates/2026_11_15_01_anticheat_incidents_v3.sql").read_text()
        sql(base)
        sql("INSERT INTO anticheat_reports (guid,name,details) VALUES (42,'Legacy','retain');"
            "INSERT INTO anticheat_daily_stats (guid,total_reports,average_score) VALUES (42,17,99);")
        sql(base)
        sql(migration)
        sql(migration)
        sql("INSERT INTO anticheat_events_v2 (event_key,guid,account,cheat_type,map_id,position_x,position_y,position_z,confidence,score,created_at) "
            "VALUES ('retained-v2',42,1,0,0,0,0,0,1,17,1730000000)")
        sql(incidents)
        sql(incidents)
        assert sql("SELECT score FROM anticheat_events_v2 WHERE event_key='retained-v2'") == "17"
        assert sql("SELECT COUNT(*),MIN(details) FROM anticheat_reports") == "1\tretain"
        assert sql("SELECT total_reports,average_score FROM anticheat_daily_stats WHERE guid=42") == "17\t99"
        print("PASS: reapplying base and migration preserves legacy rows")

        # A failed transaction must roll back its first insert on disconnect.
        sql("START TRANSACTION;" + statements[0] +
            "INSERT INTO intentionally_missing_table VALUES (1); COMMIT;", expect_success=False)
        assert sql("SELECT COUNT(*) FROM anticheat_events_v3") == "0"
        # Exercise the generated encoding with strict mode and no backslash escapes.
        inserts = "\n".join(statements)
        sql("SET SESSION sql_mode='STRICT_TRANS_TABLES,NO_BACKSLASH_ESCAPES';"
            "START TRANSACTION;" + inserts + "COMMIT;")
        sql("START TRANSACTION;" + inserts + "COMMIT;")
        assert sql("SELECT COUNT(*) FROM anticheat_events_v3") == "7"
        print("PASS: rollback and identical-key retry are atomic and idempotent")
        expected = "quote' backslash\\ newline\n utf8:é; DROP TABLE never;".encode().hex().upper()
        assert sql("SELECT HEX(details) FROM anticheat_events_v3 WHERE event_key='sql-test-1'") == expected
        assert sql("SELECT HEX(name),position_x FROM anticheat_events_v3 WHERE event_key='sql-test-1'") == "4F27427269656E\t1.5"
        assert sql("SELECT details FROM anticheat_events_v3 WHERE event_key='sql-test-5'") == "bad:?"
        assert sql("SELECT COUNT(DISTINCT incident_key) FROM anticheat_events_v3") == "1"
        print("PASS: quotes, UTF-8, invalid bytes, backslashes and numeric locale")
        expected_stats = "20000\t2\t20\n20001\t1\t50"
        assert sql("SELECT utc_day,total_reports,average_score FROM anticheat_daily_stats_v3 "
                   "WHERE guid=7 ORDER BY utc_day") == expected_stats
        assert sql("SET time_zone='+05:00'; SELECT utc_day,total_reports,average_score FROM anticheat_daily_stats_v3 "
                   "WHERE guid=7 ORDER BY utc_day") == expected_stats
        assert sql("SELECT SUM(total_reports) FROM anticheat_daily_stats_v3") == "4"
        sql(base)
        assert sql("SELECT COUNT(*) FROM anticheat_events_v3") == "7"
        assert sql("SELECT score FROM anticheat_events_v2 WHERE event_key='retained-v2'") == "17"
        print("PASS: UTC-day incident counts exclude lifecycle/alert/action snapshots and preserve v2 history")
        passed = True
    finally:
        subprocess.run(admin + ["shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server.terminate()
            server.wait(timeout=5)
        print("Disposable database stopped. Artifacts:", work)
    if passed:
        print("SQL integration passed")


if __name__ == "__main__":
    main()
