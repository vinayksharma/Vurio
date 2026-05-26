import sqlite3

def check_db(db_path):
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    try:
        cur.execute("SELECT file_path FROM scanned_files WHERE file_path LIKE '%ejected_%' OR file_path LIKE '%_ejected%'")
        rows = cur.fetchall()
        print("scanned_files with ejected:", len(rows))
        for r in rows[:5]:
            print(" -", r[0])
    except Exception:
        pass

if __name__ == "__main__":
    check_db("/Users/vinaysharma/developer/vurio/dedup_server/data/dedup_server.db")
