UPDATE physical_files p
LEFT JOIN (
    SELECT physical_id, COUNT(*) AS refs
    FROM files
    WHERE physical_id > 0
    GROUP BY physical_id
) f ON f.physical_id = p.id
SET p.ref_count = COALESCE(f.refs, 0);
