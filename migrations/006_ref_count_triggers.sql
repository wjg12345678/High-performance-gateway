DROP TRIGGER IF EXISTS trg_files_after_insert;
DROP TRIGGER IF EXISTS trg_files_after_delete;

UPDATE physical_files p
LEFT JOIN (
    SELECT physical_id, COUNT(*) AS refs
    FROM files
    WHERE physical_id > 0
    GROUP BY physical_id
) f ON f.physical_id = p.id
SET p.ref_count = COALESCE(f.refs, 0);

CREATE TRIGGER trg_files_after_insert
AFTER INSERT ON files
FOR EACH ROW
UPDATE physical_files
SET ref_count = ref_count + 1
WHERE id = NEW.physical_id;

CREATE TRIGGER trg_files_after_delete
AFTER DELETE ON files
FOR EACH ROW
UPDATE physical_files
SET ref_count = GREATEST(ref_count - 1, 0)
WHERE id = OLD.physical_id;
