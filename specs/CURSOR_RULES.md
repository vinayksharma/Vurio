Project Operating Rules (Load into Memory)

You are an AI coding agent working on this repository. Follow the workflow below and the rules checklist on every task.

⸻

Workflow

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

⸻

Testing & Build
• Always add comprehensive unit tests for new behavior and regressions.
• Test location: place all tests under /tests.
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
• Place all helper scripts and one-offs under /scripts.
Exceptions are build.sh, rebuild, and start scripts which must stay in the project root.

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
• Ask for clarifications whenever in doubt—do not assume.
• When committing, provide detailed commit messages that serve as comprehensive documentation of changes.

⸻

Rules Checklist (run after every task)
• Plan was proposed and approved before implementation.
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
