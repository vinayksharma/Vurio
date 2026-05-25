import sqlite3

def check_db(db_path):
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    try:
        cur.execute("SELECT * FROM ejected_files_history")
        rows = cur.fetchall()
        for r in rows:
            print("History:", r)
    except Exception:
        pass

if __name__ == "__main__":
    check_db("/Users/vinaysharma/developer/vurio/dedup_server/data/dedup_server.db")
