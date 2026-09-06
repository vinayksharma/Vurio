# Documentation Index

All project documentation is consolidated in this directory for easy navigation and maintenance.

## Project Guidelines

- **[CURSOR_RULES.md](CURSOR_RULES.md)** - Core development workflow and rules for AI agents and developers
- **[configuration_guide.md](configuration_guide.md)** - Complete configuration management rules, properties reference, and best practices

## Architecture & Design

- **[architecture_design.md](architecture_design.md)** - System architecture, database design, and component technical specifications
- **[api_reference.md](api_reference.md)** - External-facing and internal API specifications
- **[ui_requirements.md](ui_requirements.md)** - Product requirements for the SwiftUI desktop frontend

## Tasks

- **[TODO.md](TODO.md)** - The project task list; each entry links to its detailed specification

## Technical Analysis

- **[technical_analysis.md](technical_analysis.md)** - Technical analyses and ongoing research
- **[todo_discovery_watermark.md](todo_discovery_watermark.md)** - DISC-1 (done): the id-based
  discovery watermark dropped files that finished out of id order; measured impact, the shipped
  stopgap, and the per-file-flag replacement
- **[developer_operations_guide.md](developer_operations_guide.md)** - Testing, building, operations, and status code references

## Troubleshooting & Fixes

- **[instance_broker.md](instance_broker.md)** - BROKER-1: concurrent server+UI pairs — port-elected broker, per-instance sockets, Session lock, UI discovery
- **[troubleshooting_and_fixes.md](troubleshooting_and_fixes.md)** - Incident reports, specific bug fixes, and troubleshooting logs

---

## Documentation Standards

Per `CURSOR_RULES.md`:

- ✅ ALL documentation (.md files) MUST be in `/docs` directory
- ✅ ONLY exception: `README.md` (stays in project root - GitHub standard)
- ✅ Never create .md files in the project root (except README.md)
- ✅ All technical, API, architecture, and troubleshooting docs go in `/docs`

## Total Documentation

The documentation has been consolidated into core category files for easier maintenance.
