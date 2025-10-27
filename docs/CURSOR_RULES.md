Project Operating Rules (Load into Memory)

You are an AI coding agent working on this repository. Follow the workflow below and the rules checklist on every task.

⸻

Workflow

0. Verify Existing Functionality
   • Before planning any implementation, scan the codebase, tests, and configs to confirm whether the requested feature (or parts of it) already exists.
   • Look for existing endpoints, flags, config keys, feature toggles, or UI elements that satisfy the request.
   • If the feature exists or is partially implemented, summarize current behavior, reference the exact files/symbols/tests, and propose either usage guidance, small gaps to close, or de-duplication instead of re‑implementing.

   Quick Verification Checklist (5–10 min)
   • Search code, tests, and docs for likely keywords (feature name, endpoints, routes, flags, UI labels).

   - Prefer ripgrep if available: `rg -n "<keyword1>|<keyword2>" src/ tests/ docs/ --hidden -g '!node_modules'`
   - Fall back to grep: `grep -RIn "<keyword>" src tests docs`
     • Handlers & APIs: scan server/router files for routes, controllers, RPC handlers; check OpenAPI/Swagger files if present (e.g., `api/openapi.json`).
     • Config: check default config generator, schemas, and env-var parsing for relevant keys/flags.
     • Tests: look under `/tests` for unit/integration names that match the feature; read assertions to learn current behavior.
     • UI: search for visible labels/tooltips and existing components that might already expose the feature (even behind a flag).
     • Docs: scan `README`, `docs/`, and inline comments for notes about the feature or known limitations.
     • If you find overlap or a partial implementation, summarize what exists, paste exact file paths/symbols, and propose consolidation or small gap-fills instead of re-implementing.

1. Plan → Confirm → Implement
   • Draft a concise implementation plan (scope, files to touch, tests to add).
   • Pause and ask for user confirmation before coding.

2. Implement in Small, Incremental Steps
   • After each logical change, build and run tests before proceeding.
   • Keep changesets small to avoid complex merges/fixes later.

3. Review Rules After Each Task
   • When you finish a task (or a subtask), re-read the Rules Checklist below.
   • If anything was missed, update the code/tests/docs immediately to close gaps.

⸻

Commit/Push Policy
• Do not commit or push unless the user explicitly asks you to.
• Every commit MUST include a comprehensive, detailed commit message with:

- Clear feature/change description in the subject line
- Detailed explanation of what was implemented/changed
- Technical details about implementation approach
- Files modified and their purpose
- Testing coverage and validation performed
- Any breaking changes or backward compatibility notes
- Configuration changes and their impact
- Dependencies added/removed and their purpose
- Performance implications if applicable
- Security considerations if relevant
- Follow conventional commit format when possible (feat:, fix:, docs:, etc.)

⸻

Configuration Management
• When using any configuration property in target code:
• Subscribe to config changes and make sure the code reacts to runtime updates.
• Keep the default configuration generation code (used when no config file exists) in sync whenever new config keys are added or existing ones change.
• Keep the configuration reference/documentation up to date with every config change.

Configuration File Organization
• The config/config.yaml file MUST be organized into the following sections IN THIS ORDER:

1. SERVER CONFIGURATION - All server.\* settings
2. DATABASE CONFIGURATION - All database.\* settings
3. CACHE CONFIGURATION - All cache.\* settings
4. LOGGING CONFIGURATION - All logging.\* settings
5. THREAD POOL MANAGER (TPM) CONFIGURATION - All tpm.\* settings
6. FILES MANAGER CONFIGURATION - All files.\* settings
7. MEDIA PROCESSOR CONFIGURATION - All media.processor.\* settings
8. IMAGE PROCESSING CONFIGURATION - All media.image(s).\* settings
   - Image Formats (standard formats)
   - RAW Image Formats (raw image formats alphabetically)
   - Image Processing Settings
   - Image Transcoding
   - ONNX Model Configuration
9. VIDEO PROCESSING CONFIGURATION - All media.video.\* settings
10. AUDIO PROCESSING CONFIGURATION - All media.audio.\* settings
11. THUMBNAIL CONFIGURATION - All thumbnail.\* settings
12. DUPLICATE DETECTION CONFIGURATION - All duplicates.\* settings
13. SCHEDULER CONFIGURATION - All scheduler.\* settings

• Each section MUST:

- Start with a clear header comment block using = separators
- Group related settings together with sub-headers where appropriate
- List settings alphabetically within each subsection
- Use consistent indentation and spacing

• When adding new configuration keys:

- Place them in the appropriate section based on their prefix/category
- If creating a new category, add it in a logical position and update this list
- Never append new settings at the end without proper categorization
- Always maintain the established section order

⸻

API Documentation & OpenAPI Specification
• ALL exposed HTTP API endpoints MUST be documented in the OpenAPI specification.
• Location: src/core/webserver/static/api/openapi.json
• This includes:

- Request parameters (path, query, body)
- Response schemas and status codes
- Error responses
- Content types
- Descriptions and examples
  • When adding a new API endpoint:

1. Implement the handler (src/core/webserver/web*handlers*\*.cpp)
2. Register the route (src/core/webserver/web_server_core.cpp)
3. Add to OpenAPI spec (src/core/webserver/static/api/openapi.json)
4. Test via Swagger UI (http://localhost:8080/)
   • Never implement API endpoints without OpenAPI documentation.
   • Keep OpenAPI spec synchronized with actual implementation.

⸻

Testing & Build
• Always add comprehensive unit tests for new behavior and regressions.
• Test location: ALL test-related files MUST be under /tests directory.
• This includes:

- Unit test files (test\_\*.cpp)
- Integration test files
- Test data directories
- Test configuration files (test*\*.yaml, test*\*.json)
- Benchmark files
- Example/demo programs
- Test executables
- Test output files
  • Never create test files, test directories, or test data in the project root.
  • Unified test policy:
  • Add all new unit tests to the all_unit_tests binary, which is executed by the rebuild shell script after the server has been successfully built.
  • Keep the all_tests binary updated so that it runs all test types defined in the project (unit, integration, etc.).
  • Unit test failures:
  • If unit tests fail because of new changes, carefully check if the issue is in the logic of the implementation or the logic of the test.
  • If pre-existing tests (not directly related to the new changes) start failing, be very careful:
  • Investigate the recent changes.
  • Fix the issue in the implementation whenever possible.
  • Modify the unit test itself only as a last resort, and if you do, explicitly inform the user about the changes.
  • Always use explicit timeouts when testing or connecting to servers.
  This avoids losing control behind a blocking call and ensures stability in test and runtime environments.
  • Build & test after each logical feature addition, then continue.

⸻

Scripts & One-offs
• ALL scripts (.sh files and executables) MUST be placed in /scripts directory.
• ONLY exceptions allowed in project root:

- build.sh - Core build script
- start - Server startup script
  • This includes (all go in /scripts):
- rebuild - Core rebuild and test script
- Helper scripts
- Utility scripts
- Test scripts
- Demo scripts
- One-off automation scripts
  • Never create new .sh files in the project root (except build.sh and start).

⸻

Documentation & Markdown Files
• ALL documentation files (.md) MUST be placed in the /docs directory.
• The ONLY exception is README.md which must stay in the project root (GitHub standard).
• This includes:

- Technical analysis documents
- Implementation summaries
- Architecture documentation
- API documentation
- Feature explanations
- Troubleshooting guides
  • Never create .md files in the project root (except README.md).
  • When creating new documentation, always use /docs as the destination.

⸻

Repository Content & Privacy
• NEVER commit or check in any image files (jpg, jpeg, png, tiff, tif, arw, cr2, dng, heic, raw, etc.).
• NEVER commit or check in any personal information, user data, or sensitive content.
• NEVER commit or check in any large binary files or media files.
• Test images should be created dynamically during test execution, not stored in the repository.
• Use .gitignore to properly exclude test data directories, models, and any generated content.
• Keep the repository lightweight and focused on source code only.
• If test data is needed, create it programmatically in test setup/teardown methods.

⸻

Communication
• First confirm whether the requested feature already exists. If it does, point to the relevant code/tests/config and explain how to use it; if there is overlap, propose consolidation or minor changes rather than re‑implementing.
• Ask for clarifications whenever in doubt—do not assume.
• When committing, provide detailed commit messages that serve as comprehensive documentation of changes.

⸻

Rules Checklist (run after every task)
• Plan was proposed and approved before implementation.

• Verified whether the requested feature already exists (fully or partially).
• Cited the files, symbols, tests, or configs that demonstrate existing behavior.
• If present, avoided duplicate work and instead documented usage or addressed only the missing gaps.

- Maintain a clear seperation of concerns.
  • No commits/pushes were made without explicit user request.
  • Commit messages include comprehensive details (what, why, how, impact, testing).
  • Code subscribes to and reacts to config changes for any used property.
  • Default config generator updated for any new/changed keys.
  • Config reference/docs updated to reflect latest changes.
  • Comprehensive tests added/updated under /tests.
  • all_unit_tests binary updated with new unit tests (for rebuild script).
  • all_tests binary updated to cover all test types.
  • Checked for unit test failures and handled them carefully (fix implementation > update test as last resort, inform user if test changed).
  • Explicit timeouts applied in server tests/connections to prevent blocking calls.
  • Build and tests run after the change; issues resolved before proceeding.
  • Any helper/one-off scripts placed under /scripts.
  • No images, personal information, or large binary files committed to repository.
  • Test data created dynamically rather than stored in repository.
  • Final pass: re-read these rules; if anything missed, fix now.

⸻

Acknowledgment

Rules read and loaded into memory.
I will follow the workflow, re-run the checklist after each task, and hold commits/pushes until you ask.
