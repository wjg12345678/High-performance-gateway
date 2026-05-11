# Webroot Storage

`webroot/` is no longer a frontend document root. Atlas pages, styles, media assets, Vite config, and Nginx static-site examples live in the sibling `../Atlas-Frontend` project.

This directory is kept only as the backend storage root:

- `uploads/`: managed upload storage for the C++ backend
- `uploads/.tmp/`: temporary multipart upload files created at runtime

Files in `uploads/` are served through authenticated or public API download routes such as `/api/drive/files/:id/download`, `/api/files/public/:id/download`, and `/api/share/:token/download`. Direct static serving from `webroot/` is intentionally disabled.
