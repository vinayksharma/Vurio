Project Operating Rules (Load into Memory)

You are an AI coding agent working on this repository. Follow the workflow below and the rules checklist on every task.

Workflow 1. Plan → Confirm → Implement
• Draft a concise implementation plan (scope, files to touch, tests to add).
• Pause and ask for user confirmation before coding. 2. Implement in Small, Incremental Steps
• After each logical change, build and run unit tests before proceeding.
• Keep changesets small to avoid complex merges/fixes later. 3. Review Rules After Each Task
• When you finish a task (or a subtask), re-read the Rules Checklist below.
• If anything was missed, update the code/tests/docs immediately to close gaps.

Commit/Push Policy
• Do not commit or push unless the user explicitly asks you to.

Configuration Management
• When using any configuration property in target code:
• Subscribe to config changes and make sure the code reacts to runtime updates.
• Keep the default configuration generation code (used when no config file exists) in sync whenever new config keys are added or existing ones change.
• Keep the configuration reference/documentation up to date with every config change.

Testing & Build
• Always add comprehensive unit tests for new behavior and regressions.
• Test location: place all tests under /tests.
• Build & test after each logical feature addition, then continue.

Scripts & One-offs
• Place all helper scripts and one-offs under /scripts.

Communication
• Ask for clarifications whenever in doubt—do not assume.

⸻

Rules Checklist (run after every task)
• Plan was proposed and approved before implementation.
• No commits/pushes were made without explicit user request.
• Code subscribes to and reacts to config changes for any used property.
• Default config generator updated for any new/changed keys.
• Config reference/docs updated to reflect latest changes.
• Comprehensive unit tests added/updated under /tests.
• Build and tests run after the change; issues resolved before proceeding.
• Any helper/one-off scripts placed under /scripts.
• Final pass: re-read these rules; if anything missed, fix now.

⸻

Acknowledgment: Rules read and loaded into memory. I will follow the workflow, re-run the checklist after each task, and hold commits/pushes until you ask.
