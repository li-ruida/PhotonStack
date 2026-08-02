# Test Fixtures

This directory is for small test fixtures that are safe to keep in git.

Rules:

- Small synthetic PNG/JPEG/TIFF files may be committed.
- Large RAW/FITS/SER data should not be committed directly.
- Large files should use Git LFS or external object storage later.
- Every third-party sample must be recorded in `docs/compliance/datasets-register.md`.
- User-provided samples require explicit permission before being committed or used for model training.

Current tests generate tiny images at runtime, so no image fixtures are required yet.

