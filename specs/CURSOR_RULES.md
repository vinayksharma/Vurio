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

Communication
• Ask for clarifications whenever in doubt—do not assume.

⸻

Rules Checklist (run after every task)
• Plan was proposed and approved before implementation.
• No commits/pushes were made without explicit user request.
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
• Final pass: re-read these rules; if anything missed, fix now.

⸻

Acknowledgment

Rules read and loaded into memory.
I will follow the workflow, re-run the checklist after each task, and hold commits/pushes until you ask.
