import sqlite3
import os

def migrate():
    db_path = 'data/dedup_server.db'
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    cursor.execute("SELECT key, value FROM user_settings WHERE key LIKE 'ejectionPath%'")
    settings = cursor.fetchall()

    for key, value in settings:
        old_path = value
        
        # Check if it ends with _ejected, _errored, or _ignored
        new_path = old_path
        if old_path.endswith("_ejected"):
            base_dir = os.path.dirname(old_path)
            folder_name = os.path.basename(old_path)
            uuid = folder_name.replace("_ejected", "")
            new_path = os.path.join(base_dir, f"ejected_{uuid}")
        elif old_path.endswith("_errored"):
            base_dir = os.path.dirname(old_path)
            folder_name = os.path.basename(old_path)
            uuid = folder_name.replace("_errored", "")
            new_path = os.path.join(base_dir, f"errored_{uuid}")
        elif old_path.endswith("_ignored"):
            base_dir = os.path.dirname(old_path)
            folder_name = os.path.basename(old_path)
            uuid = folder_name.replace("_ignored", "")
            new_path = os.path.join(base_dir, f"ignored_{uuid}")
            
        if old_path != new_path:
            print(f"Migrating:\n  From: {old_path}\n  To:   {new_path}")
            
            # Rename physical directory if it exists
            if os.path.exists(old_path):
                print(f"  -> Renaming physical directory")
                os.rename(old_path, new_path)
            else:
                print(f"  -> Physical directory does not exist, skipping rename")
                
            # Update user_settings
            cursor.execute("UPDATE user_settings SET value=? WHERE key=?", (new_path, key))
            
            # Helper to perform the replace
            def sql_replace(table, column, op=old_path, np=new_path):
                # Ensure we only replace paths that start with old_path
                # In SQLite, we can use REPLACE, but it replaces everywhere.
                # Since these are absolute paths, it should be safe.
                # However, to be extra safe, we only update rows where column LIKE old_path + '%'
                cursor.execute(f"UPDATE {table} SET {column} = REPLACE({column}, ?, ?) WHERE {column} LIKE ?", (op, np, op + '%'))
                if cursor.rowcount > 0:
                    print(f"  -> Updated {cursor.rowcount} rows in {table}.{column}")
            
            sql_replace('scanned_files', 'file_path')
            
            # For relative paths, we need to extract the old folder name and new folder name
            old_folder_name = os.path.basename(old_path)
            new_folder_name = os.path.basename(new_path)
            
            cursor.execute("UPDATE scanned_files SET relative_path = REPLACE(relative_path, ?, ?) WHERE file_path LIKE ?", (old_folder_name, new_folder_name, new_path + '%'))
            if cursor.rowcount > 0:
                print(f"  -> Updated {cursor.rowcount} rows in scanned_files.relative_path")
                
            sql_replace('ejected_files_history', 'ejected_path')
            sql_replace('duplicate_members', 'file_path')
            sql_replace('duplicate_groups', 'representative_file_path')
            sql_replace('image_artifacts', 'file_path')
            sql_replace('processing_errors', 'file_path')
            sql_replace('thumbnail_cache', 'source_path')
            sql_replace('preview_cache', 'source_path')
            
    conn.commit()
    conn.close()
    print("Migration complete!")

if __name__ == '__main__':
    migrate()
